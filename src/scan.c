#include "scan.h"
#include "lib.h"

#define PATH_MAX_CH 512

static size_t dp_node_len(const EFI_DEVICE_PATH_PROTOCOL *n) {
    return (size_t)n->Length[0] | ((size_t)n->Length[1] << 8);
}

static bool dp_is_end(const EFI_DEVICE_PATH_PROTOCOL *n) {
    return n->Type == END_DEVICE_PATH_TYPE && n->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE;
}

/* Concatenate all file-path nodes of the image's device path into a normalized
 * absolute path ("\EFI\foo\bar.efi") and strip the file name. Returns the
 * directory without a trailing backslash ("" for the root), or NULL if unknown. */
static CHAR16 *image_dir(EFI_LOADED_IMAGE_PROTOCOL *li) {
    if (!li || !li->FilePath) return NULL;

    CHAR16 raw[PATH_MAX_CH] = { 0 };
    size_t n = 0;
    for (EFI_DEVICE_PATH_PROTOCOL *node = li->FilePath; !dp_is_end(node);
         node = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)node + dp_node_len(node))) {
        size_t len = dp_node_len(node);
        if (len < 4) break; /* malformed */
        if (node->Type != MEDIA_DEVICE_PATH || node->SubType != MEDIA_FILEPATH_DP) continue;
        size_t chars = (len - 4) / sizeof(CHAR16);
        const uint8_t *src = (const uint8_t *)node + 4;
        if (n && n + 1 < PATH_MAX_CH) raw[n++] = '\\';
        for (size_t i = 0; i < chars && n + 1 < PATH_MAX_CH; i++) {
            CHAR16 c;
            memcpy(&c, src + i * 2, 2); /* nodes are byte-packed */
            if (!c) break;
            raw[n++] = c;
        }
    }
    raw[n] = 0;
    if (!n) return NULL;

    /* Normalize: '/' -> '\', collapse repeats, force a leading '\'. */
    CHAR16 norm[PATH_MAX_CH];
    size_t m = 0;
    norm[m++] = '\\';
    for (size_t i = 0; i < n && m + 1 < PATH_MAX_CH; i++) {
        CHAR16 c = raw[i] == '/' ? '\\' : raw[i];
        if (c == '\\' && norm[m - 1] == '\\') continue;
        norm[m++] = c;
    }
    norm[m] = 0;

    /* Strip the file name. */
    size_t last = 0;
    for (size_t i = 0; i < m; i++)
        if (norm[i] == '\\') last = i;
    norm[last] = 0;
    return wdup(norm);
}

static EFI_FILE_PROTOCOL *open_root(EFI_HANDLE h) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(h, &gEfiSimpleFileSystemProtocolGuid, (void **)&sfs)) || !sfs)
        return NULL;
    if (EFI_ERROR(sfs->OpenVolume(sfs, &root))) return NULL;
    return root;
}

static EFI_FILE_INFO *file_info(EFI_FILE_PROTOCOL *f) {
    UINTN size = SIZE_OF_EFI_FILE_INFO + 256 * sizeof(CHAR16);
    EFI_FILE_INFO *info = xalloc(size);
    if (!info) return NULL;
    EFI_STATUS st = f->GetInfo(f, &gEfiFileInfoGuid, &size, info);
    if (st == EFI_BUFFER_TOO_SMALL) {
        xfree(info);
        info = xalloc(size);
        if (!info) return NULL;
        st = f->GetInfo(f, &gEfiFileInfoGuid, &size, info);
    }
    if (EFI_ERROR(st)) { xfree(info); return NULL; }
    return info;
}

/* Open `path` on filesystem `h` and make sure it is a directory. */
static EFI_FILE_PROTOCOL *open_dir(EFI_HANDLE h, const CHAR16 *path) {
    EFI_FILE_PROTOCOL *root = open_root(h), *dir = NULL;
    if (!root) return NULL;
    EFI_STATUS st = root->Open(root, &dir, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(st)) return NULL;
    EFI_FILE_INFO *info = file_info(dir);
    bool is_dir = info && (info->Attribute & EFI_FILE_DIRECTORY);
    xfree(info);
    if (!is_dir) { dir->Close(dir); return NULL; }
    return dir;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool read_at(EFI_FILE_PROTOCOL *f, uint64_t off, void *buf, UINTN len) {
    UINTN n = len;
    return !EFI_ERROR(f->SetPosition(f, off)) && !EFI_ERROR(f->Read(f, &n, buf)) && n == len;
}

#define WIN_CERT_REVISION_2_0          0x0200
#define WIN_CERT_TYPE_PKCS_SIGNED_DATA 0x0002
#define WIN_CERT_TYPE_EFI_GUID         0x0EF1
#define WIN_CERT_HDR                   8   /* dwLength, wRevision, wCertificateType */
#define WIN_CERT_GUID_HDR              24  /* ... + CertType GUID */
#define MAX_CERT_ENTRIES               16

/* EFI_CERT_TYPE_PKCS7_GUID {4aafd29d-68df-49ee-8aa9-347d375665a7}, as stored on disk */
static const uint8_t cert_type_pkcs7[16] = { 0x9d, 0xd2, 0xaf, 0x4a, 0xdf, 0x68, 0xee, 0x49,
                                             0x8a, 0xa9, 0x34, 0x7d, 0x37, 0x56, 0x65, 0xa7 };

/* Structural sanity check of an Authenticode blob of blob_len bytes, given
 * its first `have` bytes: a DER SEQUENCE that fits inside the certificate and
 * opens with the PKCS#7 signedData OID (1.2.840.113549.1.7.2). Rejects junk;
 * does not verify anything cryptographically. */
static bool looks_like_pkcs7(const uint8_t *b, uint32_t have, uint32_t blob_len) {
    static const uint8_t signed_data_oid[] = { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02 };
    if (have < 2 || b[0] != 0x30) return false;
    uint32_t hdr;
    uint64_t content;
    if (b[1] < 0x80) {
        hdr = 2;
        content = b[1];
    } else {
        uint32_t n = b[1] & 0x7f;
        if (n == 0 || n > 4 || have < 2 + n) return false; /* indefinite / oversized lengths aren't DER */
        hdr = 2 + n;
        content = 0;
        for (uint32_t i = 0; i < n; i++) content = content << 8 | b[2 + i];
    }
    if (hdr + content > blob_len || content < sizeof(signed_data_oid)) return false;
    if (have < hdr + sizeof(signed_data_oid)) return false;
    return memcmp(b + hdr, signed_data_oid, sizeof(signed_data_oid)) == 0;
}

/* ---- PE recognition ---------------------------------------------------- */

#define IMAGE_SUBSYSTEM_EFI_APPLICATION         10
#define IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER 11
#define IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER      12
#define IMAGE_FILE_MACHINE_EBC                  0x0EBC

#if defined(__x86_64__)
#define NATIVE_MACHINE 0x8664
#elif defined(__aarch64__)
#define NATIVE_MACHINE 0xAA64
#elif defined(__riscv) && __riscv_xlen == 64
#define NATIVE_MACHINE 0x5064
#elif defined(__loongarch64)
#define NATIVE_MACHINE 0x6264
#elif defined(__i386__)
#define NATIVE_MACHINE 0x014C
#else
#error "unsupported architecture"
#endif

struct pe_info {
    uint16_t machine, subsystem;
    uint32_t sec_off, sec_size; /* security directory; 0/0 if the header has none */
};

/* Parse just enough of the DOS, COFF and optional headers to classify the
 * image. Returns false if this isn't a PE image at all. Everything is read
 * from the file with exact-length reads and bounds-checked against fsize. */
static bool pe_parse(EFI_FILE_PROTOCOL *f, uint64_t fsize, struct pe_info *pi) {
    memset(pi, 0, sizeof(*pi));

    uint8_t dos[64];
    if (fsize < sizeof(dos) || !read_at(f, 0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z')
        return false;
    uint64_t pe = rd32(dos + 0x3c); /* 64-bit so pe + offsets can't wrap */

    /* "PE\0\0" + IMAGE_FILE_HEADER */
    uint8_t coff[24];
    if (pe + sizeof(coff) > fsize || !read_at(f, pe, coff, sizeof(coff)) || memcmp(coff, "PE\0\0", 4) != 0)
        return false;
    pi->machine = rd16(coff + 4);
    uint16_t opt_size = rd16(coff + 20);

    /* Optional header, up to and including DataDirectory[4] (152 bytes for
     * PE32+). Never read past SizeOfOptionalHeader. */
    uint8_t opt[112 + 5 * 8];
    uint32_t want = opt_size < sizeof(opt) ? opt_size : sizeof(opt);
    if (want < 70 /* through Subsystem */ || pe + sizeof(coff) + want > fsize ||
        !read_at(f, pe + sizeof(coff), opt, want))
        return false;

    uint32_t nrva_off; /* offset of NumberOfRvaAndSizes */
    switch (rd16(opt)) {
    case 0x20b: nrva_off = 108; break; /* PE32+ */
    case 0x10b: nrva_off = 92; break;  /* PE32 */
    default: return false;
    }
    pi->subsystem = rd16(opt + 68); /* same offset in PE32 and PE32+ */

    /* The data directories follow NumberOfRvaAndSizes, 8 bytes each; the
     * security directory is entry 4. It only exists if the header is long
     * enough and declares more than 4 directories. */
    uint32_t dir4 = nrva_off + 4 + 4 * 8;
    if (want >= dir4 + 8 && rd32(opt + nrva_off) > 4) {
        pi->sec_off = rd32(opt + dir4);
        pi->sec_size = rd32(opt + dir4 + 4);
    }
    return true;
}

/* NULL if this firmware can start the image as an application, otherwise a
 * short reason for the UI. */
static const CHAR16 *pe_problem(bool is_pe, const struct pe_info *pi) {
    if (!is_pe) return L"not an EFI image";
    if (pi->subsystem == IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER ||
        pi->subsystem == IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER)
        return L"EFI driver, not an application";
    if (pi->subsystem != IMAGE_SUBSYSTEM_EFI_APPLICATION) return L"not an EFI application";
    if (pi->machine == NATIVE_MACHINE || pi->machine == IMAGE_FILE_MACHINE_EBC) return NULL;
    switch (pi->machine) {
    case 0x014C: return L"built for IA32";
    case 0x8664: return L"built for x64";
    case 0xAA64: return L"built for AArch64";
    case 0x01C2: case 0x01C4: return L"built for ARM";
    case 0x5064: return L"built for RISC-V";
    case 0x6264: return L"built for LoongArch";
    default: return L"built for another CPU";
    }
}

/* Does this PE carry an Authenticode signature? Walks the attribute
 * certificate table like edk2's DxeImageVerificationLib does and looks for a
 * PKCS#7 signedData blob, either as WIN_CERT_TYPE_PKCS_SIGNED_DATA or wrapped
 * in WIN_CERT_TYPE_EFI_GUID with CertType == EFI_CERT_TYPE_PKCS7_GUID. This
 * says nothing about whether the firmware trusts the signer -- only
 * LoadImage can tell that. */
static bool pe_signed(EFI_FILE_PROTOCOL *f, uint64_t fsize, const struct pe_info *pi) {
    uint32_t off  = pi->sec_off;
    uint32_t size = pi->sec_size;

    /* Directory offset must be non-zero, 8-byte aligned, and within file bounds */
    if (!off || (off & 7) != 0 || size < WIN_CERT_HDR || (uint64_t)off + size > fsize)
        return false;

    /* The table may hold several WIN_CERTIFICATEs, each padded to 8 bytes. */
    uint64_t pos = off, end = (uint64_t)off + size;
    for (int i = 0; i < MAX_CERT_ENTRIES && end - pos > WIN_CERT_HDR; i++) {
        uint8_t cert[WIN_CERT_GUID_HDR + 32];
        if (!read_at(f, pos, cert, WIN_CERT_HDR))
            return false;

        uint32_t cert_len  = rd32(cert);
        uint16_t cert_rev  = rd16(cert + 4);
        uint16_t cert_type = rd16(cert + 6);
        uint64_t padded    = ((uint64_t)cert_len + 7) & ~(uint64_t)7;

        /* A malformed entry ends the walk: nothing after it can be trusted. */
        if (cert_len < WIN_CERT_HDR || padded > end - pos)
            return false;

        uint32_t hdr = 0;
        if (cert_rev == WIN_CERT_REVISION_2_0 && cert_type == WIN_CERT_TYPE_PKCS_SIGNED_DATA)
            hdr = WIN_CERT_HDR;
        else if (cert_rev == WIN_CERT_REVISION_2_0 && cert_type == WIN_CERT_TYPE_EFI_GUID && cert_len > WIN_CERT_GUID_HDR)
            hdr = WIN_CERT_GUID_HDR;

        if (hdr) {
            /* Read the rest of the header plus the start of the blob. */
            uint32_t have = cert_len - hdr < 32 ? cert_len - hdr : 32;
            if (!read_at(f, pos + WIN_CERT_HDR, cert + WIN_CERT_HDR, hdr - WIN_CERT_HDR + have))
                return false;
            bool type_ok = hdr == WIN_CERT_HDR || memcmp(cert + WIN_CERT_HDR, cert_type_pkcs7, 16) == 0;
            if (type_ok && looks_like_pkcs7(cert + hdr, have, cert_len - hdr))
                return true;
        }
        /* Other types (X.509, RSA2048 GUIDs, ...) aren't Authenticode: skip them. */
        pos += padded;
    }
    return false;
}

static void add_app(struct scan *s, const CHAR16 *group, const EFI_FILE_INFO *fi, bool is_signed,
                    const CHAR16 *problem) {
    if (s->count == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 16;
        struct app *na = xalloc(sizeof(*na) * (size_t)ncap);
        if (!na) return;
        if (s->apps) memcpy(na, s->apps, sizeof(*na) * (size_t)s->count);
        xfree(s->apps);
        s->apps = na;
        s->cap = ncap;
    }
    struct app *a = &s->apps[s->count];
    a->rel = group ? wcat3(group, L"\\", fi->FileName) : wdup(fi->FileName);
    a->label = wdup(fi->FileName);
    a->group = group ? wdup(group) : NULL;
    a->size = fi->FileSize;
    a->is_signed = is_signed;
    a->problem = problem;
    if (!a->rel || !a->label) {
        xfree(a->rel);
        xfree(a->label);
        xfree(a->group);
        return;
    }
    size_t n = wlen(a->label);
    if (n > 4 && wends_with_i(a->label, L".efi")) a->label[n - 4] = 0; /* "memtest.efi" -> "memtest" */
    s->count++;
}

/* Enumerate EFI applications in `dir`, recognised by their PE headers rather
 * than their names, so e.g. an EFI-stub "vmlinuz-linux" shows up too. Files
 * named *.efi that can't be started here are listed with the reason; other
 * files that aren't EFI applications (initrds, configs, ...) are ignored. At
 * the top level, also descend one level into sub-directories so an app can
 * ship with its own files. */
static void scan_dir(struct scan *s, EFI_FILE_PROTOCOL *dir, const CHAR16 *group) {
    UINTN cap = SIZE_OF_EFI_FILE_INFO + 256 * sizeof(CHAR16);
    EFI_FILE_INFO *fi = xalloc(cap);
    if (!fi) return;

    for (;;) {
        UINTN size = cap;
        EFI_STATUS st = dir->Read(dir, &size, fi);
        if (st == EFI_BUFFER_TOO_SMALL) {
            xfree(fi);
            cap = size;
            fi = xalloc(cap);
            if (!fi) return;
            continue;
        }
        if (EFI_ERROR(st) || size == 0) break;

        const CHAR16 *name = fi->FileName;
        if (name[0] == '.') continue; /* ".", "..", hidden files, macOS "._foo" */

        if (fi->Attribute & EFI_FILE_DIRECTORY) {
            if (group) continue;
            EFI_FILE_PROTOCOL *sub = NULL;
            if (!EFI_ERROR(dir->Open(dir, &sub, (CHAR16 *)name, EFI_FILE_MODE_READ, 0))) {
                CHAR16 *gname = wdup(name);
                if (gname) scan_dir(s, sub, gname);
                xfree(gname);
                sub->Close(sub);
            }
        } else if (fi->FileSize > 0) {
            EFI_FILE_PROTOCOL *file = NULL;
            if (EFI_ERROR(dir->Open(dir, &file, (CHAR16 *)name, EFI_FILE_MODE_READ, 0))) continue;
            struct pe_info pi;
            bool is_pe = pe_parse(file, fi->FileSize, &pi);
            const CHAR16 *problem = pe_problem(is_pe, &pi);
            bool named_efi = wlen(name) > 4 && wends_with_i(name, L".efi");
            if (!problem || named_efi) {
                bool is_signed = !problem && pe_signed(file, fi->FileSize, &pi);
                add_app(s, group, fi, is_signed, problem);
            }
            file->Close(file);
        }
    }
    xfree(fi);
}

static void sort_apps(struct scan *s) {
    for (int i = 1; i < s->count; i++) {
        struct app t = s->apps[i];
        int j = i - 1;
        while (j >= 0 && wicmp(s->apps[j].rel, t.rel) > 0) {
            s->apps[j + 1] = s->apps[j];
            j--;
        }
        s->apps[j + 1] = t;
    }
}

static EFI_FILE_PROTOCOL *try_dir(struct scan *s, EFI_HANDLE h, const CHAR16 *base) {
    if (!base) return NULL;
    CHAR16 *path = wcat3(base, L"\\", L"PARTYBOX");
    if (!path) return NULL;
    EFI_FILE_PROTOCOL *dir = open_dir(h, path);
    if (dir) {
        /* Show the name with its on-disk capitalization. */
        EFI_FILE_INFO *info = file_info(dir);
        if (info) {
            CHAR16 *pretty = wcat3(base, L"\\", info->FileName);
            if (pretty) { xfree(path); path = pretty; }
            xfree(info);
        }
        s->fs_handle = h;
        s->dir = path;
        return dir;
    }
    xfree(path);
    return NULL;
}

void scan_free(struct scan *s) {
    for (int i = 0; i < s->count; i++) {
        xfree(s->apps[i].rel);
        xfree(s->apps[i].label);
        xfree(s->apps[i].group);
    }
    xfree(s->apps);
    xfree(s->dir);
    memset(s, 0, sizeof(*s));
}

bool scan_image_location(EFI_HANDLE *fs, CHAR16 **dir) {
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    *fs = NULL;
    *dir = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (void **)&li)) || !li ||
        !li->DeviceHandle)
        return false;
    *dir = image_dir(li);
    if (!*dir) return false;
    *fs = li->DeviceHandle;
    return true;
}

void scan_run(struct scan *s) {
    scan_free(s);

    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (void **)&li);
    CHAR16 *base = image_dir(li);
    EFI_HANDLE own = li ? li->DeviceHandle : NULL;

    /* 1) Next to our own image, on our own volume: the normal case. */
    EFI_FILE_PROTOCOL *dir = own ? try_dir(s, own, base) : NULL;

    /* 2) Fallbacks for loaders that don't fill in DeviceHandle/FilePath
     *    usefully: same relative path on any volume, then \PARTYBOX. */
    if (!dir) {
        EFI_HANDLE *hs = NULL;
        UINTN nh = 0;
        if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                                NULL, &nh, &hs))) {
            for (UINTN i = 0; i < nh && !dir; i++) dir = try_dir(s, hs[i], base);
            if (own && !dir) dir = try_dir(s, own, L"");
            for (UINTN i = 0; i < nh && !dir; i++) dir = try_dir(s, hs[i], L"");
            xfree(hs);
        }
        s->fallback = dir != NULL;
    }
    if (!dir) {
        /* Remember where we expected it, so the UI can say so. */
        s->dir = wcat3(base ? base : L"", L"\\", L"PARTYBOX");
        xfree(base);
        return;
    }
    xfree(base);
    s->found = true;
    scan_dir(s, dir, NULL);
    dir->Close(dir);
    sort_apps(s);
}

CHAR16 *scan_app_path(const struct scan *s, const struct app *a) {
    return wcat3(s->dir, L"\\", a->rel);
}

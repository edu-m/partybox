/* Fake UEFI firmware for running partybox's real efi_main() on the host under
 * AddressSanitizer / UndefinedBehaviorSanitizer.
 *
 * What it checks beyond the sanitizers:
 *   - every pool allocation is freed (exact bookkeeping, with a backtrace of
 *     the allocation site for anything left over), FreePool only sees pool
 *     pointers, and never NULL
 *   - every EFI_FILE_PROTOCOL handle, event and loaded image is released
 *   - protocol/service use that the UEFI spec forbids: using a closed event,
 *     Blt outside the screen or from a buffer that is too small, LoadImage with
 *     a malformed device path, Unload of an image that already exited, ...
 *   - optional out-of-memory injection: fail the Nth AllocatePool
 *
 * The "disk" is a host directory ($ESP) that holds EFI/partybox/partybox.efi
 * and its PARTYBOX folder. Child applications aren't executed: StartImage
 * picks a canned behaviour from the file name (see start_image()).
 *
 * Environment:
 *   ESP=dir        fake ESP root (required)       KEYS="DOWN ENTER T T ESC"  input script
 *   W=, H=         screen size (default 1024x768) SB=1          Secure Boot on
 *   OOM_AT=n       fail the nth AllocatePool      BADCON=1      ClearScreen fails until SetMode
 *   LI=0|1|2       0 normal, 1 no DeviceHandle, 2 no FilePath
 *   LOADOPT=text   LoadOptions (UTF-16, exact-size buffer)
 *   NOGOP=1        no graphics output protocol    DUMP=file.ppm  write the final frame
 *   SETTIMER_FAIL=1 / WAIT_FAIL=1 / SETMODE_FAIL=1   make that firmware call fail
 *   EXPECT_EXHAUST=1  running out of script while idle is the expected end of the test
 * Script tokens: UP DOWN LEFT RIGHT PGUP PGDN HOME END ESC F5 ENTER TAB SPACE,
 * any single character, T = one timer tick, and | = a pause: keys before it are
 * read in one go, partybox redraws, and the keys after it arrive later.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <execinfo.h>
#include <setjmp.h>
#include <ctype.h>
#include "efi.h"

extern EFI_STATUS EFIAPI efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *);
extern EFI_GUID gEfiGraphicsOutputProtocolGuid, gEfiLoadedImageProtocolGuid, gEfiSimpleFileSystemProtocolGuid,
    gEfiDevicePathProtocolGuid, gEfiFileInfoGuid, gEfiGlobalVariableGuid;

static int g_viol;
static jmp_buf g_done;        /* longjmp target: ends the run (reset, script end, busy loop) */
static bool g_exhausted;
static int g_done_code;
#define VIOL(...) do { fprintf(stderr, "VIOLATION: " __VA_ARGS__); fputc('\n', stderr); g_viol++; } while (0)
#define UNEXPECTED(name) do { VIOL("unexpected call to %s", name); } while (0)

/* ---- UTF-16 helpers ------------------------------------------------------ */
static size_t u16len(const CHAR16 *s) { size_t n = 0; while (s[n]) n++; return n; }

static void u16_to_utf8(const CHAR16 *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned c = *s;
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xc0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xe0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}

static size_t utf8_to_u16(const char *s, CHAR16 *out) { /* returns length, writes NUL */
    size_t n = 0;
    while (*s) {
        unsigned char b = (unsigned char)*s++;
        unsigned c = b;
        int more = b < 0x80 ? 0 : (b & 0xe0) == 0xc0 ? 1 : (b & 0xf0) == 0xe0 ? 2 : 0;
        if (more == 1) c = b & 0x1f; else if (more == 2) c = b & 0x0f;
        while (more-- > 0 && *s) c = c << 6 | ((unsigned char)*s++ & 63);
        out[n++] = (CHAR16)c;
    }
    out[n] = 0;
    return n;
}

/* ---- pool allocator with leak tracking and OOM injection ------------------ */
struct alloc { void *p; size_t n; void *bt[10]; int nbt; struct alloc *next; };
static struct alloc *g_live;
static long g_allocs, g_live_n, g_peak, g_oom_at;
static bool g_oom_hit;

static void *track_alloc(size_t n) {
    g_allocs++;
    if (g_oom_at && g_allocs == g_oom_at) { g_oom_hit = true; return NULL; }
    void *p = malloc(n ? n : 1);
    memset(p, 0xA5, n ? n : 1);
    struct alloc *a = calloc(1, sizeof(*a));
    a->p = p; a->n = n; a->nbt = backtrace(a->bt, 10);
    a->next = g_live; g_live = a;
    if (++g_live_n > g_peak) g_peak = g_live_n;
    return p;
}

static EFI_STATUS track_free(void *p) {
    if (!p) { VIOL("FreePool(NULL)"); return EFI_INVALID_PARAMETER; }
    for (struct alloc **pp = &g_live; *pp; pp = &(*pp)->next) {
        if ((*pp)->p == p) {
            struct alloc *a = *pp;
            *pp = a->next;
            memset(p, 0xDD, a->n);
            free(p);
            free(a);
            g_live_n--;
            return EFI_SUCCESS;
        }
    }
    VIOL("FreePool(%p): not a live pool allocation (double free or foreign pointer)", p);
    return EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI fw_alloc_pool(EFI_MEMORY_TYPE type, UINTN size, VOID **buf) {
    if (!buf) { VIOL("AllocatePool(NULL buffer)"); return EFI_INVALID_PARAMETER; }
    if ((int)type < 0 || type >= EfiMaxMemoryType) { VIOL("AllocatePool: bad memory type %d", (int)type); return EFI_INVALID_PARAMETER; }
    void *p = track_alloc(size);
    if (!p) return EFI_OUT_OF_RESOURCES;
    *buf = p;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fw_free_pool(VOID *p) { return track_free(p); }

/* ---- handles ----------------------------------------------------------------- */
enum { H_IMAGE, H_FS, H_FS2, H_GOP, H_CHILD };
struct hnd { int kind; };
static struct hnd h_image = { H_IMAGE }, h_fs = { H_FS }, h_fs2 = { H_FS2 }, h_gop = { H_GOP };

/* ---- device paths ------------------------------------------------------------ */
static void put_node(uint8_t **p, uint8_t type, uint8_t sub, size_t len) {
    (*p)[0] = type; (*p)[1] = sub; (*p)[2] = (uint8_t)len; (*p)[3] = (uint8_t)(len >> 8);
    memset(*p + 4, 0, len - 4);
}

static uint8_t g_fs_dp[12 + 6 + 42 + 4], g_gop_dp[12 + 6 + 4], g_fs2_dp[12 + 6 + 42 + 4];

static void build_hw_dp(uint8_t *buf, bool disk) {
    uint8_t *p = buf;
    put_node(&p, ACPI_DEVICE_PATH, ACPI_DP, 12); p += 12;
    put_node(&p, HARDWARE_DEVICE_PATH, HW_PCI_DP, 6); p += 6;
    if (disk) { put_node(&p, MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP, 42); p += 42; }
    put_node(&p, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4);
}

static uint8_t g_img_fp[4 + 2 * 32 + 4];

/* ---- fake file system over a host directory ---------------------------------- */
static char g_root[4096];
static int g_open_files;

struct hf {
    EFI_FILE_PROTOCOL p;
    char path[4096];
    bool dir, is_root;
    int fd;
    char **names;
    int nnames, idx;
    uint64_t pos;
};

static int ci_lookup(const char *dirpath, const char *want, char *out, size_t cap) {
    DIR *d = opendir(dirpath);
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, want)) { snprintf(out, cap, "%s", e->d_name); rc = 0; break; }
    }
    closedir(d);
    return rc;
}

/* Resolve a UEFI path (relative to `base`, or to the root if it starts with '\') case-insensitively. */
static int resolve(const char *base, const char *rel, char *out, size_t cap) {
    char cur[4096];
    snprintf(cur, sizeof cur, "%s", (rel[0] == '\\' || rel[0] == '/') ? g_root : base);
    char *dup = strdup(rel), *save = NULL;
    for (char *tok = strtok_r(dup, "\\/", &save); tok; tok = strtok_r(NULL, "\\/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) {
            if (strlen(cur) > strlen(g_root)) *strrchr(cur, '/') = 0;
            continue;
        }
        char real[512];
        if (ci_lookup(cur, tok, real, sizeof real)) { free(dup); return -1; }
        size_t l = strlen(cur);
        snprintf(cur + l, sizeof cur - l, "/%s", real);
    }
    free(dup);
    snprintf(out, cap, "%s", cur);
    return 0;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static EFI_STATUS EFIAPI f_open(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *, UINT64, UINT64);

static struct hf *hf_new(const char *path, bool is_root) {
    struct stat st;
    if (stat(path, &st)) return NULL;
    struct hf *f = calloc(1, sizeof(*f));
    snprintf(f->path, sizeof f->path, "%s", path);
    f->dir = S_ISDIR(st.st_mode);
    f->is_root = is_root;
    f->fd = -1;
    if (f->dir) {
        DIR *d = opendir(path);
        struct dirent *e;
        int cap = 0;
        if (!is_root) { f->names = malloc(2 * sizeof(char *)); f->names[0] = strdup("."); f->names[1] = strdup(".."); f->nnames = 2; cap = 2; }
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (f->nnames == cap) { cap = cap ? cap * 2 : 16; f->names = realloc(f->names, cap * sizeof(char *)); }
            f->names[f->nnames++] = strdup(e->d_name);
        }
        if (d) closedir(d);
        if (f->nnames > (is_root ? 0 : 2)) qsort(f->names + (is_root ? 0 : 2), f->nnames - (is_root ? 0 : 2), sizeof(char *), cmp_names);
    } else {
        f->fd = open(path, O_RDONLY);
    }
    extern EFI_STATUS EFIAPI f_close(EFI_FILE_PROTOCOL *), f_read(EFI_FILE_PROTOCOL *, UINTN *, VOID *),
        f_setpos(EFI_FILE_PROTOCOL *, UINT64), f_getinfo(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, VOID *);
    f->p.Revision = EFI_FILE_PROTOCOL_REVISION;
    f->p.Open = f_open;
    f->p.Close = f_close;
    f->p.Read = f_read;
    f->p.SetPosition = f_setpos;
    f->p.GetInfo = f_getinfo;
    g_open_files++;
    return f;
}

static EFI_STATUS EFIAPI f_open(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr) {
    struct hf *f = (struct hf *)This;
    if (!New || !Name) { VIOL("Open: NULL argument"); return EFI_INVALID_PARAMETER; }
    if (Mode != EFI_FILE_MODE_READ) VIOL("Open: unexpected mode 0x%llx (partybox is read-only)", (unsigned long long)Mode);
    if (!f->dir) { VIOL("Open called on a file handle"); return EFI_UNSUPPORTED; }
    char name8[2048], path[4096];
    u16_to_utf8(Name, name8, sizeof name8);
    if (resolve(f->path, name8, path, sizeof path)) return EFI_NOT_FOUND;
    struct hf *n = hf_new(path, !strcmp(path, g_root));
    if (!n) return EFI_NOT_FOUND;
    *New = &n->p;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI f_close(EFI_FILE_PROTOCOL *This) {
    struct hf *f = (struct hf *)This;
    if (f->fd >= 0) close(f->fd);
    for (int i = 0; i < f->nnames; i++) free(f->names[i]);
    free(f->names);
    memset(f, 0xDD, sizeof(*f));
    free(f);
    g_open_files--;
    return EFI_SUCCESS;
}

static UINTN fill_info(const char *dirpath, const char *name, EFI_FILE_INFO *info, UINTN cap) {
    char full[4096];
    struct stat st;
    snprintf(full, sizeof full, "%s/%s", dirpath, name);
    if (!name[0] || !strcmp(name, ".")) snprintf(full, sizeof full, "%s", dirpath);
    else if (!strcmp(name, "..")) snprintf(full, sizeof full, "%s/..", dirpath);
    if (stat(full, &st)) memset(&st, 0, sizeof st);
    CHAR16 wname[512];
    size_t wl = utf8_to_u16(name, wname);
    UINTN need = SIZE_OF_EFI_FILE_INFO + (wl + 1) * sizeof(CHAR16);
    if (cap < need) return need;
    memset(info, 0, need);
    info->Size = need;
    info->FileSize = S_ISDIR(st.st_mode) ? 0 : (UINT64)st.st_size;
    info->PhysicalSize = info->FileSize;
    info->Attribute = S_ISDIR(st.st_mode) ? EFI_FILE_DIRECTORY : 0;
    memcpy(info->FileName, wname, (wl + 1) * sizeof(CHAR16));
    return need;
}

EFI_STATUS EFIAPI f_read(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf) {
    struct hf *f = (struct hf *)This;
    if (!Size || (!Buf && *Size)) { VIOL("Read: NULL argument"); return EFI_INVALID_PARAMETER; }
    if (f->dir) {
        if (f->idx >= f->nnames) { *Size = 0; return EFI_SUCCESS; }
        UINTN need = fill_info(f->path, f->names[f->idx], Buf, *Size);
        if (need > *Size) { *Size = need; return EFI_BUFFER_TOO_SMALL; }
        *Size = need;
        f->idx++;
        return EFI_SUCCESS;
    }
    ssize_t n = pread(f->fd, Buf, *Size, (off_t)f->pos);
    if (n < 0) return EFI_DEVICE_ERROR;
    f->pos += (uint64_t)n;
    *Size = (UINTN)n;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI f_setpos(EFI_FILE_PROTOCOL *This, UINT64 pos) {
    struct hf *f = (struct hf *)This;
    if (f->dir) { if (pos) return EFI_UNSUPPORTED; f->idx = 0; return EFI_SUCCESS; }
    if (pos == ~0ULL) { struct stat st; fstat(f->fd, &st); pos = (uint64_t)st.st_size; }
    f->pos = pos;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI f_getinfo(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN *Size, VOID *Buf) {
    struct hf *f = (struct hf *)This;
    if (!Type || !Size || (!Buf && *Size)) { VIOL("GetInfo: NULL argument"); return EFI_INVALID_PARAMETER; }
    if (memcmp(Type, &gEfiFileInfoGuid, sizeof(EFI_GUID))) return EFI_UNSUPPORTED;
    const char *slash = strrchr(f->path, '/');
    const char *nm = f->is_root ? "" : (slash ? slash + 1 : "");
    CHAR16 wname[512];
    size_t wl = utf8_to_u16(nm, wname);
    UINTN want = SIZE_OF_EFI_FILE_INFO + (wl + 1) * sizeof(CHAR16);
    if (*Size < want) { *Size = want; return EFI_BUFFER_TOO_SMALL; }
    struct stat st;
    if (stat(f->path, &st)) return EFI_DEVICE_ERROR;
    EFI_FILE_INFO *info = Buf;
    memset(info, 0, want);
    info->Size = want;
    info->FileSize = S_ISDIR(st.st_mode) ? 0 : (UINT64)st.st_size;
    info->PhysicalSize = info->FileSize;
    info->Attribute = S_ISDIR(st.st_mode) ? EFI_FILE_DIRECTORY : 0;
    memcpy(info->FileName, wname, (wl + 1) * sizeof(CHAR16));
    *Size = want;
    return EFI_SUCCESS;
}

static char g_empty_root[4100];
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *g_sfs2_obj_p(void);

static EFI_STATUS EFIAPI sfs_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root) {
    if (!Root) { VIOL("OpenVolume(NULL)"); return EFI_INVALID_PARAMETER; }
    bool second = This == g_sfs2_obj_p();
    struct hf *f = hf_new(second ? g_empty_root : g_root, true);
    if (!f) return EFI_DEVICE_ERROR;
    *Root = &f->p;
    return EFI_SUCCESS;
}
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL g_sfs = { EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION, sfs_open_volume };
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL g_sfs2 = { EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION, sfs_open_volume };
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *g_sfs2_obj_p(void) { return &g_sfs2; }

/* ---- graphics ---------------------------------------------------------------- */
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION g_modes[2];
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE g_gop_mode;
static uint32_t *g_fb;
static size_t g_fb_cap;
static long g_blts, g_idle_blts;   /* idle: redraws since the last input event */

static EFI_STATUS EFIAPI gop_query_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 n, UINTN *sz, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info) {
    if (n >= 2 || !sz || !info) return EFI_INVALID_PARAMETER;
    *info = track_alloc(sizeof(**info));
    if (!*info) return EFI_OUT_OF_RESOURCES;
    **info = g_modes[n];
    *sz = sizeof(**info);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 n) {
    if (n >= 2 || (getenv("SETMODE_FAIL") && n == 0)) return EFI_DEVICE_ERROR;
    g_gop_mode.Mode = n;
    g_gop_mode.Info = &g_modes[n];
    memset(g_fb, 0, g_fb_cap);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf,
                                 EFI_GRAPHICS_OUTPUT_BLT_OPERATION op, UINTN sx, UINTN sy, UINTN dx, UINTN dy,
                                 UINTN w, UINTN h, UINTN delta) {
    g_blts++;
    if (++g_idle_blts > 200) { VIOL("redrew the screen %ld times with no input event: busy loop", g_idle_blts); g_done_code = 4; longjmp(g_done, 3); }
    UINTN W = g_gop_mode.Info->HorizontalResolution, H = g_gop_mode.Info->VerticalResolution;
    if (op != EfiBltBufferToVideo) { UNEXPECTED("Blt (op != BufferToVideo)"); return EFI_UNSUPPORTED; }
    if (!buf) { VIOL("Blt: NULL buffer"); return EFI_INVALID_PARAMETER; }
    if (dx + w > W || dy + h > H) {
        VIOL("Blt outside the screen: dst %zux%zu at (%zu,%zu) on a %zux%zu mode", (size_t)w, (size_t)h, (size_t)dx, (size_t)dy, (size_t)W, (size_t)H);
        return EFI_INVALID_PARAMETER;
    }
    if (delta == 0) delta = (sx + w) * 4;
    if (delta % 4 || delta < (sx + w) * 4) { VIOL("Blt: bad Delta %zu for source x %zu + width %zu", (size_t)delta, (size_t)sx, (size_t)w); return EFI_INVALID_PARAMETER; }
    for (UINTN j = 0; j < h; j++) /* reads the caller's buffer: ASan flags a buffer that is too small */
        memcpy(g_fb + (dy + j) * W + dx, (uint8_t *)buf + (sy + j) * delta + sx * 4, w * 4);
    return EFI_SUCCESS;
}
static EFI_GRAPHICS_OUTPUT_PROTOCOL g_gop = { gop_query_mode, gop_set_mode, gop_blt, &g_gop_mode };

/* ---- console ---------------------------------------------------------------- */
static EFI_SIMPLE_TEXT_OUTPUT_MODE g_con_mode = { 3, 2, EFI_LIGHTGRAY, 0, 0, TRUE };
static bool g_badcon;
static long g_outstr;

static EFI_STATUS EFIAPI con_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, BOOLEAN e) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI con_output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, CHAR16 *s) {
    if (!s) { VIOL("OutputString(NULL)"); return EFI_INVALID_PARAMETER; }
    g_outstr += (long)u16len(s); /* ASan: must be NUL-terminated */
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI con_test(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, CHAR16 *s) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI con_query(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, UINTN m, UINTN *c, UINTN *r) {
    if (m >= 3) return EFI_UNSUPPORTED;
    *c = 80 + m * 40; *r = 25 + m * 10;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI con_setmode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, UINTN m) {
    if (m >= 3) return EFI_UNSUPPORTED;
    g_con_mode.Mode = (INT32)m;
    g_badcon = false;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI con_setattr(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, UINTN a) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI con_clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t) { return g_badcon ? EFI_INVALID_PARAMETER : EFI_SUCCESS; }
static EFI_STATUS EFIAPI con_setcur(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, UINTN c, UINTN r) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI con_enablecur(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, BOOLEAN v) { g_con_mode.CursorVisible = v; return EFI_SUCCESS; }
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL g_conout = { con_reset, con_output, con_test, con_query, con_setmode, con_setattr, con_clear, con_setcur, con_enablecur, &g_con_mode };

/* ---- events and input ------------------------------------------------------------- */
struct ev { int kind; bool armed, closed; };    /* 0 = key, 1 = timer */
static struct ev g_ev_key = { 0 };
static int g_open_events;
static long g_timer_ticks_armed;

static const char *g_script;                    /* unconsumed part of KEYS */
static char *g_script_buf;

static const char *skip_ws(const char *s) { while (*s == ' ') s++; return s; }
static const char *tok_end(const char *s) { while (*s && *s != ' ') s++; return s; }

static bool tok_is(const char *s, const char *e, const char *name) { return strlen(name) == (size_t)(e - s) && !strncmp(s, name, e - s); }

static bool decode_key(const char *s, const char *e, EFI_INPUT_KEY *k) {
    k->ScanCode = 0; k->UnicodeChar = 0;
    if (tok_is(s, e, "UP")) k->ScanCode = SCAN_UP;
    else if (tok_is(s, e, "DOWN")) k->ScanCode = SCAN_DOWN;
    else if (tok_is(s, e, "LEFT")) k->ScanCode = SCAN_LEFT;
    else if (tok_is(s, e, "RIGHT")) k->ScanCode = SCAN_RIGHT;
    else if (tok_is(s, e, "PGUP")) k->ScanCode = SCAN_PAGE_UP;
    else if (tok_is(s, e, "PGDN")) k->ScanCode = SCAN_PAGE_DOWN;
    else if (tok_is(s, e, "HOME")) k->ScanCode = SCAN_HOME;
    else if (tok_is(s, e, "END")) k->ScanCode = SCAN_END;
    else if (tok_is(s, e, "ESC")) k->ScanCode = SCAN_ESC;
    else if (tok_is(s, e, "F5")) k->ScanCode = SCAN_F5;
    else if (tok_is(s, e, "ENTER")) k->UnicodeChar = CHAR_CARRIAGE_RETURN;
    else if (tok_is(s, e, "TAB")) k->UnicodeChar = CHAR_TAB;
    else if (tok_is(s, e, "SPACE")) k->UnicodeChar = ' ';
    else if (e - s == 1) k->UnicodeChar = (CHAR16)(unsigned char)*s;
    else { fprintf(stderr, "bad script token '%.*s'\n", (int)(e - s), s); exit(2); }
    return true;
}

static EFI_STATUS EFIAPI in_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *t, BOOLEAN e) {
    return EFI_SUCCESS;   /* script tokens are future key presses: nothing queued to drop */
}

static long g_empty_reads;

static EFI_STATUS EFIAPI in_read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *t, EFI_INPUT_KEY *key) {
    if (!key) { VIOL("ReadKeyStroke(NULL)"); return EFI_INVALID_PARAMETER; }
    const char *s = skip_ws(g_script), *e = tok_end(s);
    if (s == e && ++g_empty_reads > 1000) { g_exhausted = true; g_done_code = 3; longjmp(g_done, 1); }
    if (s == e || tok_is(s, e, "T") || tok_is(s, e, "|")) return EFI_NOT_READY;
    g_empty_reads = 0;
    g_idle_blts = 0;
    decode_key(s, e, key);
    g_script = e;
    return EFI_SUCCESS;
}
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL g_conin = { in_reset, in_read, (EFI_EVENT)&g_ev_key };

static jmp_buf g_done;
static int g_done_code;
static bool g_exhausted;

static long g_waits;
static uint64_t g_stall_us;
static EFI_STATUS EFIAPI fw_stall(UINTN us) { g_stall_us += us; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI fw_wait_for_event_inner(UINTN n, EFI_EVENT *ev, UINTN *idx);
static EFI_STATUS EFIAPI fw_wait_for_event(UINTN n, EFI_EVENT *ev, UINTN *idx) {
    EFI_STATUS st = fw_wait_for_event_inner(n, ev, idx);
    if (!EFI_ERROR(st)) g_idle_blts = 0;
    return st;
}
static EFI_STATUS EFIAPI fw_wait_for_event_inner(UINTN n, EFI_EVENT *ev, UINTN *idx) {
    /* A loop that keeps asking without ever waiting or stalling burns a CPU core. */
    if (++g_waits > 50000 && g_stall_us < (uint64_t)g_waits * 1000) { VIOL("WaitForEvent called %ld times: busy loop", g_waits); g_done_code = 4; longjmp(g_done, 3); }
    if (getenv("WAIT_FAIL")) return EFI_DEVICE_ERROR;   /* e.g. a console without a usable WaitForKey */
    if (n == 0 || !ev || !idx) { VIOL("WaitForEvent: bad arguments (count %zu)", (size_t)n); return EFI_INVALID_PARAMETER; }
    for (UINTN i = 0; i < n; i++) {
        struct ev *e = (struct ev *)ev[i];
        if (!e) { VIOL("WaitForEvent: NULL event at index %zu", (size_t)i); return EFI_INVALID_PARAMETER; }
        if (e->closed) { VIOL("WaitForEvent: event %zu was already closed", (size_t)i); return EFI_INVALID_PARAMETER; }
    }
    const char *s = skip_ws(g_script), *e = tok_end(s);
    if (s == e) { g_exhausted = true; g_done_code = 3; longjmp(g_done, 1); }
    if (tok_is(s, e, "|")) {
        g_script = e;
        for (UINTN i = 0; i < n; i++)
            if (ev[i] == (EFI_EVENT)&g_ev_key) { *idx = i; return EFI_SUCCESS; }
        return fw_wait_for_event_inner(n, ev, idx);
    }
    if (tok_is(s, e, "T")) {
        for (UINTN i = 0; i < n; i++) {
            struct ev *x = (struct ev *)ev[i];
            if (x->kind == 1 && x->armed) { g_script = e; *idx = i; return EFI_SUCCESS; }
        }
        fprintf(stderr, "note: script has a tick but no timer is armed; dropping it\n");
        g_script = e;
        return fw_wait_for_event_inner(n, ev, idx);
    }
    for (UINTN i = 0; i < n; i++)
        if (ev[i] == (EFI_EVENT)&g_ev_key) { *idx = i; return EFI_SUCCESS; }
    VIOL("WaitForEvent: waiting without WaitForKey while the script has a key pending");
    g_script = e;
    return EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI fw_create_event(UINT32 type, EFI_TPL tpl, EFI_EVENT_NOTIFY fn, VOID *ctx, EFI_EVENT *out) {
    if (!out) { VIOL("CreateEvent(NULL)"); return EFI_INVALID_PARAMETER; }
    if (type != EVT_TIMER) { VIOL("CreateEvent: unexpected type 0x%x", type); return EFI_INVALID_PARAMETER; }
    if (fn) { VIOL("CreateEvent: notify function not expected"); }
    static struct ev pool[256];
    static int used;
    if (used == 256) { VIOL("harness: out of event slots"); return EFI_OUT_OF_RESOURCES; }
    struct ev *e = &pool[used++];  /* never reused: use-after-close is diagnosed, not a crash */
    e->kind = 1;
    g_open_events++;
    *out = (EFI_EVENT)e;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fw_set_timer(EFI_EVENT ev, EFI_TIMER_DELAY t, UINT64 trig) {
    struct ev *e = (struct ev *)ev;
    if (!e || e->closed || e->kind != 1) { VIOL("SetTimer on a NULL, closed or non-timer event"); return EFI_INVALID_PARAMETER; }
    if (getenv("SETTIMER_FAIL") && t == TimerPeriodic) return EFI_DEVICE_ERROR;
    e->armed = t != TimerCancel;
    if (t == TimerPeriodic && trig != 10000000) VIOL("SetTimer: unexpected period %llu", (unsigned long long)trig);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fw_close_event(EFI_EVENT ev) {
    struct ev *e = (struct ev *)ev;
    if (!e || e->closed) { VIOL("CloseEvent on a NULL or already closed event"); return EFI_INVALID_PARAMETER; }
    e->closed = true; e->armed = false;
    g_open_events--;
    return EFI_SUCCESS;
}

/* ---- images ---------------------------------------------------------------------- */
struct image {
    struct hnd h;
    char name[512];            /* lower-case file name of the image */
    char rel[1024];            /* path from the device path, UTF-8 */
    uint8_t *buf; size_t n;
    EFI_DEVICE_PATH_PROTOCOL *dp;
    bool started, gone;
    EFI_LOADED_IMAGE_PROTOCOL li;
};
static int g_loaded;
static long g_starts;

static EFI_STATUS EFIAPI fw_load_image(BOOLEAN policy, EFI_HANDLE parent, EFI_DEVICE_PATH_PROTOCOL *dp, VOID *src, UINTN n, EFI_HANDLE *out) {
    if (!out) { VIOL("LoadImage: NULL ImageHandle"); return EFI_INVALID_PARAMETER; }
    if (parent != (EFI_HANDLE)&h_image) VIOL("LoadImage: parent is not the calling image");
    if (policy) VIOL("LoadImage: BootPolicy TRUE with a SourceBuffer");
    if (!src || !n) { VIOL("LoadImage: no source buffer (partybox always supplies one)"); return EFI_INVALID_PARAMETER; }
    if (!dp) { VIOL("NOTE LoadImage without a device path: the child would start with no FilePath"); return EFI_INVALID_PARAMETER; }

    /* Walk the device path: well-formed nodes, a terminating END node, and the
     * last real node is a NUL-terminated file path. */
    size_t off = 0, total = 0;
    const EFI_DEVICE_PATH_PROTOCOL *last = NULL, *node = dp;
    for (int guard = 0; guard < 64; guard++) {
        size_t len = node->Length[0] | (node->Length[1] << 8);
        if (len < 4) { VIOL("LoadImage: device path node with length %zu", len); return EFI_INVALID_PARAMETER; }
        total += len;
        if (node->Type == END_DEVICE_PATH_TYPE && node->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE) break;
        last = node;
        node = (const EFI_DEVICE_PATH_PROTOCOL *)((const uint8_t *)node + len);
        off += len;
    }
    (void)off;
    if (!last || last->Type != MEDIA_DEVICE_PATH || last->SubType != MEDIA_FILEPATH_DP) {
        VIOL("LoadImage: device path does not end in a file path node");
        return EFI_INVALID_PARAMETER;
    }
    size_t flen = (last->Length[0] | (last->Length[1] << 8)) - 4;
    const CHAR16 *pn = (const CHAR16 *)((const uint8_t *)last + 4);
    if (flen < 2 || flen % 2 || pn[flen / 2 - 1] != 0) { VIOL("LoadImage: file path node not NUL-terminated"); return EFI_INVALID_PARAMETER; }

    static struct image *keep[8192];
    static int nkeep;
    struct image *im = calloc(1, sizeof(*im));
    if (nkeep < 8192) keep[nkeep++] = im;   /* never freed: use-after-unload is diagnosed, not a crash */
    im->h.kind = H_CHILD;
    u16_to_utf8(pn, im->rel, sizeof im->rel);
    const char *base = strrchr(im->rel, '\\');
    base = base ? base + 1 : im->rel;
    for (size_t i = 0; base[i] && i < sizeof im->name - 1; i++) im->name[i] = (char)tolower((unsigned char)base[i]);

    /* The firmware must get exactly the bytes of the file it was asked to load. */
    char host[4096];
    if (resolve(g_root, im->rel, host, sizeof host)) VIOL("LoadImage: file path %s does not exist", im->rel);
    else {
        FILE *fp = fopen(host, "rb");
        struct stat st;
        if (fp && !stat(host, &st)) {
            if ((size_t)st.st_size != n) VIOL("LoadImage: size %zu differs from file size %lld", (size_t)n, (long long)st.st_size);
            else {
                uint8_t *chk = malloc(n);
                if (fread(chk, 1, n, fp) != n || memcmp(chk, src, n)) VIOL("LoadImage: source buffer differs from %s", im->rel);
                free(chk);
            }
        }
        if (fp) fclose(fp);
    }

    im->buf = malloc(n); memcpy(im->buf, src, n); im->n = n;     /* firmware copies the image */
    im->dp = malloc(total); memcpy(im->dp, dp, total);            /* ... and the device path (as edk2 does) */
    g_loaded++;
    *out = (EFI_HANDLE)im;
    if (!strcmp(im->name, "denied.efi")) { /* image not created */
        free(im->buf); free(im->dp); free(im); g_loaded--; *out = NULL;
        return EFI_ACCESS_DENIED;
    }
    if (!strcmp(im->name, "secfail.efi")) return EFI_SECURITY_VIOLATION;  /* handle exists, must be unloaded */
    return EFI_SUCCESS;
}

static void image_release(struct image *im) {
    free(im->buf); free(im->dp);
    im->gone = true;      /* keep the struct so use-after-unload is diagnosed, not a crash */
    g_loaded--;
}

static EFI_STATUS EFIAPI fw_unload_image(EFI_HANDLE h) {
    struct image *im = (struct image *)h;
    if (!im || im->h.kind != H_CHILD || im->gone) { VIOL("UnloadImage on an invalid or already unloaded image"); return EFI_INVALID_PARAMETER; }
    if (im->started) VIOL("UnloadImage on a started image");
    image_release(im);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_start_image(EFI_HANDLE h, UINTN *exit_size, CHAR16 **exit_data) {
    struct image *im = (struct image *)h;
    if (!im || im->h.kind != H_CHILD || im->gone) { VIOL("StartImage on an invalid or unloaded image"); return EFI_INVALID_PARAMETER; }
    if (im->started) { VIOL("StartImage twice"); return EFI_INVALID_PARAMETER; }
    if (!strcmp(im->name, "secfail.efi")) VIOL("StartImage on an image LoadImage rejected with SECURITY_VIOLATION");
    im->started = true;
    g_starts++;
    EFI_STATUS st = EFI_SUCCESS;
    UINTN xs = 0;
    CHAR16 *xd = NULL;
    if (!strcmp(im->name, "exitdata.efi")) {
        static const char msg[] = "deliberate failure";
        xs = (sizeof msg) * 2;
        xd = track_alloc(xs);
        if (xd) { utf8_to_u16(msg, xd); st = EFI_ABORTED; } else xs = 0;
    } else if (!strcmp(im->name, "binexit.efi")) {   /* SUCCESS with binary ExitData: no NUL, exact size */
        xs = 8;
        xd = track_alloc(xs);
        if (xd) memset(xd, 'A', xs); else xs = 0;
    } else if (!strcmp(im->name, "binerr.efi")) {    /* error with ExitData that is *not* a string */
        xs = 8;
        xd = track_alloc(xs);
        if (xd) memset(xd, 'B', xs); else xs = 0;
        st = EFI_LOAD_ERROR;
    } else if (!strcmp(im->name, "longexit.efi")) {
        xs = 3000 * 2;
        xd = track_alloc(xs + 2);
        if (xd) { for (int i = 0; i < 3000; i++) xd[i] = 'x'; xd[3000] = 0; st = EFI_ABORTED; } else xs = 0;
    } else if (!strcmp(im->name, "crashy.efi")) {
        st = (EFI_STATUS)-2;
    } else if (!strcmp(im->name, "warn.efi")) {
        st = 5; /* EFI_WARN_DELETE_FAILURE */
    } else if (!strcmp(im->name, "modeswitch.efi")) {
        gop_set_mode(&g_gop, 1);
        g_badcon = true;
    }
    if (exit_size) *exit_size = xs;
    if (exit_data) *exit_data = xd; else if (xd) track_free(xd);
    image_release(im);   /* applications are unloaded when they return */
    return st;
}

/* ---- protocols / handles ---------------------------------------------------------- */
static EFI_LOADED_IMAGE_PROTOCOL g_li;
static int g_li_mode;

static EFI_STATUS EFIAPI fw_handle_protocol(EFI_HANDLE h, EFI_GUID *g, VOID **iface) {
    if (!h || !g || !iface) { VIOL("HandleProtocol: NULL argument"); return EFI_INVALID_PARAMETER; }
    struct hnd *x = h;
    *iface = NULL;
    if (!memcmp(g, &gEfiLoadedImageProtocolGuid, 16)) {
        if (x->kind == H_IMAGE) { *iface = &g_li; return EFI_SUCCESS; }
        if (x->kind == H_CHILD) { struct image *im = (struct image *)x; if (im->gone) VIOL("HandleProtocol on an unloaded image"); *iface = &im->li; return EFI_SUCCESS; }
    } else if (!memcmp(g, &gEfiSimpleFileSystemProtocolGuid, 16)) {
        if (x->kind == H_FS) { *iface = &g_sfs; return EFI_SUCCESS; }
        if (x->kind == H_FS2) { *iface = &g_sfs2; return EFI_SUCCESS; }
    } else if (!memcmp(g, &gEfiDevicePathProtocolGuid, 16)) {
        if (x->kind == H_FS) { *iface = g_fs_dp; return EFI_SUCCESS; }
        if (x->kind == H_FS2) { *iface = g_fs2_dp; return EFI_SUCCESS; }
        if (x->kind == H_GOP) { *iface = g_gop_dp; return EFI_SUCCESS; }
    } else if (!memcmp(g, &gEfiGraphicsOutputProtocolGuid, 16)) {
        if (x->kind == H_GOP && !getenv("NOGOP")) { *iface = &g_gop; return EFI_SUCCESS; }
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI fw_locate_handle_buffer(EFI_LOCATE_SEARCH_TYPE t, EFI_GUID *g, VOID *key, UINTN *n, EFI_HANDLE **buf) {
    if (t != ByProtocol || !g || !n || !buf) { VIOL("LocateHandleBuffer: bad arguments"); return EFI_INVALID_PARAMETER; }
    EFI_HANDLE list[2]; UINTN cnt = 0;
    if (!memcmp(g, &gEfiSimpleFileSystemProtocolGuid, 16)) { list[cnt++] = &h_fs; list[cnt++] = &h_fs2; }
    else if (!memcmp(g, &gEfiGraphicsOutputProtocolGuid, 16) && !getenv("NOGOP")) list[cnt++] = &h_gop;
    if (!cnt) return EFI_NOT_FOUND;
    EFI_HANDLE *out = track_alloc(cnt * sizeof(EFI_HANDLE));
    if (!out) return EFI_OUT_OF_RESOURCES;
    memcpy(out, list, cnt * sizeof(EFI_HANDLE));
    *n = cnt; *buf = out;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_locate_protocol(EFI_GUID *g, VOID *reg, VOID **iface) {
    if (!g || !iface) { VIOL("LocateProtocol: NULL argument"); return EFI_INVALID_PARAMETER; }
    if (!memcmp(g, &gEfiGraphicsOutputProtocolGuid, 16) && !getenv("NOGOP")) { *iface = &g_gop; return EFI_SUCCESS; }
    *iface = NULL;
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI fw_watchdog(UINTN timeout, UINT64 code, UINTN size, CHAR16 *data) { return EFI_SUCCESS; }

/* ---- variables / runtime ---------------------------------------------------------- */
static uint64_t g_os_ind;
static bool g_os_ind_set;

static EFI_STATUS EFIAPI rt_get_variable(CHAR16 *name, EFI_GUID *g, UINT32 *attr, UINTN *size, VOID *data) {
    if (!name || !g || !size || (!data && *size)) { VIOL("GetVariable: bad arguments"); return EFI_INVALID_PARAMETER; }
    if (memcmp(g, &gEfiGlobalVariableGuid, 16)) return EFI_NOT_FOUND;
    char n8[64];
    u16_to_utf8(name, n8, sizeof n8);
    uint8_t val[8] = { 0 };
    UINTN len;
    if (!strcmp(n8, "OsIndicationsSupported")) { val[0] = 1; len = 8; }
    else if (!strcmp(n8, "OsIndications")) { if (!g_os_ind_set) return EFI_NOT_FOUND; memcpy(val, &g_os_ind, 8); len = 8; }
    else if (!strcmp(n8, "SecureBoot")) { val[0] = getenv("SB") ? 1 : 0; len = 1; }
    else return EFI_NOT_FOUND;
    if (*size < len) { *size = len; return EFI_BUFFER_TOO_SMALL; }
    memcpy(data, val, len);
    *size = len;
    if (attr) *attr = 7;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI rt_set_variable(CHAR16 *name, EFI_GUID *g, UINT32 attr, UINTN size, VOID *data) {
    char n8[64];
    u16_to_utf8(name, n8, sizeof n8);
    if (!strcmp(n8, "OsIndications") && size == 8) {
        memcpy(&g_os_ind, data, 8); g_os_ind_set = true;
        if ((attr & 7) != 7) VIOL("SetVariable(OsIndications): attributes 0x%x, expected NV|BS|RT", attr);
        return EFI_SUCCESS;
    }
    VIOL("SetVariable(%s): unexpected", n8);
    return EFI_INVALID_PARAMETER;
}

static void EFIAPI rt_reset(EFI_RESET_TYPE type, EFI_STATUS st, UINTN size, VOID *data) {
    fprintf(stderr, "RESET type=%d osind=%llx\n", (int)type, (unsigned long long)g_os_ind);
    g_done_code = 10 + (int)type;
    longjmp(g_done, 2);
}

/* ---- main ---------------------------------------------------------------- */
static EFI_BOOT_SERVICES g_bs;
static EFI_RUNTIME_SERVICES g_rt;
static EFI_SYSTEM_TABLE g_st;

static void report_leaks(void) {
    for (struct alloc *a = g_live; a; a = a->next) {
        fprintf(stderr, "LEAK: %zu bytes at %p allocated at:\n", a->n, a->p);
        char **sym = backtrace_symbols(a->bt, a->nbt);
        for (int i = 1; i < a->nbt && i < 7; i++) fprintf(stderr, "    %s\n", sym[i]);
        free(sym);
        g_viol++;
    }
}

static void dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t W = g_gop_mode.Info->HorizontalResolution, H = g_gop_mode.Info->VerticalResolution;
    fprintf(f, "P6\n%u %u\n255\n", W, H);
    for (size_t i = 0; i < (size_t)W * H; i++) { uint8_t p[3] = { g_fb[i] >> 16, g_fb[i] >> 8, g_fb[i] }; fwrite(p, 1, 3, f); }
    fclose(f);
}

int main(void) {
    const char *esp = getenv("ESP");
    if (!esp) { fprintf(stderr, "ESP not set\n"); return 2; }
    if (!realpath(esp, g_root)) { perror("ESP"); return 2; }
    snprintf(g_empty_root, sizeof g_empty_root, "%s.empty", g_root);
    mkdir(g_empty_root, 0755);
    uint32_t W = getenv("W") ? (uint32_t)atoi(getenv("W")) : 1024, H = getenv("H") ? (uint32_t)atoi(getenv("H")) : 768;
    g_oom_at = getenv("OOM_AT") ? atol(getenv("OOM_AT")) : 0;
    g_badcon = getenv("BADCON") != NULL;
    g_li_mode = getenv("LI") ? atoi(getenv("LI")) : 0;
    g_script_buf = strdup(getenv("KEYS") ? getenv("KEYS") : "ESC");
    g_script = g_script_buf;

    g_modes[0] = (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){ 0, W, H, PixelBlueGreenRedReserved8BitPerColor, { 0 }, W };
    g_modes[1] = (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){ 0, W > 400 ? W / 2 : W, H > 300 ? H / 2 : H, PixelBlueGreenRedReserved8BitPerColor, { 0 }, W > 400 ? W / 2 : W };
    g_gop_mode.MaxMode = 2; g_gop_mode.Mode = 0; g_gop_mode.Info = &g_modes[0]; g_gop_mode.SizeOfInfo = sizeof(g_modes[0]);
    g_fb_cap = (size_t)W * H * 4;
    g_fb = calloc(1, g_fb_cap);

    build_hw_dp(g_fs_dp, true); build_hw_dp(g_fs2_dp, true); build_hw_dp(g_gop_dp, false);

    /* partybox's own LoadedImage: file path node "\EFI\partybox\partybox.efi" */
    {
        uint8_t *p = g_img_fp;
        static const char *path = "\\EFI\\partybox\\partybox.efi";
        size_t pl = (strlen(path) + 1) * 2, nl = 4 + pl;
        put_node(&p, MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP, nl);
        CHAR16 w[64]; utf8_to_u16(path, w);
        memcpy(p + 4, w, pl);
        p += nl;
        put_node(&p, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4);
    }
    g_li.Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
    g_li.ParentHandle = NULL;
    g_li.DeviceHandle = g_li_mode == 1 ? NULL : (EFI_HANDLE)&h_fs;
    g_li.FilePath = g_li_mode == 2 ? NULL : (EFI_DEVICE_PATH_PROTOCOL *)g_img_fp;
    if (getenv("LOADOPT")) {
        size_t n = strlen(getenv("LOADOPT"));
        CHAR16 *lo = malloc((n + 1) * 2);                    /* exact size: ASan sees reads past the end */
        size_t wl = utf8_to_u16(getenv("LOADOPT"), lo);
        g_li.LoadOptions = lo;
        g_li.LoadOptionsSize = (UINT32)(getenv("LOADOPT_ODD") ? wl * 2 - 1 : wl * 2);
    }

    g_bs.AllocatePool = fw_alloc_pool; g_bs.FreePool = fw_free_pool;
    g_bs.HandleProtocol = fw_handle_protocol; g_bs.LocateHandleBuffer = fw_locate_handle_buffer; g_bs.LocateProtocol = fw_locate_protocol;
    g_bs.LoadImage = fw_load_image; g_bs.StartImage = fw_start_image; g_bs.UnloadImage = fw_unload_image;
    g_bs.WaitForEvent = fw_wait_for_event; g_bs.CreateEvent = fw_create_event; g_bs.SetTimer = fw_set_timer; g_bs.CloseEvent = fw_close_event;
    g_bs.SetWatchdogTimer = fw_watchdog; g_bs.Stall = fw_stall;
    g_rt.GetVariable = rt_get_variable; g_rt.SetVariable = rt_set_variable; g_rt.ResetSystem = rt_reset;

    g_st.Hdr.Revision = 0x0002003C;
    g_st.FirmwareVendor = (CHAR16 *)L"FakeFW";
    g_st.ConIn = &g_conin; g_st.ConOut = &g_conout;
    g_st.BootServices = &g_bs; g_st.RuntimeServices = &g_rt;

    EFI_STATUS st = 0;
    int jr = setjmp(g_done);
    bool returned = false;
    if (!jr) { st = efi_main((EFI_HANDLE)&h_image, &g_st); returned = true; }

    if (getenv("DUMP")) dump_ppm(getenv("DUMP"));
    if (g_exhausted && !getenv("EXPECT_EXHAUST")) VIOL("script exhausted while partybox was waiting for input");
    if (returned) {
        if (st != EFI_SUCCESS) VIOL("efi_main returned 0x%llx", (unsigned long long)st);
        report_leaks();
        if (g_open_files) VIOL("%d EFI_FILE_PROTOCOL handle(s) left open", g_open_files);
        if (g_open_events) VIOL("%d event(s) left open", g_open_events);
        if (g_loaded) VIOL("%d loaded image(s) never unloaded", g_loaded);
    }
    fprintf(stderr, "%s: allocs=%ld peak_live=%ld blts=%ld starts=%ld oom_hit=%d violations=%d%s\n", g_viol ? "FAIL" : "ok",
            g_allocs, g_peak, g_blts, g_starts, g_oom_hit, g_viol, returned ? "" : " (ended by reset/script end; no leak check)");
    return g_viol ? 1 : 0;
}

#include "lib.h"

EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;
EFI_GUID gEfiGlobalVariableGuid = EFI_GLOBAL_VARIABLE;

void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp < sp) {
        while (n--) *dp++ = *sp++;
    } else {
        while (n--) dp[n] = sp[n];
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

void *xalloc(size_t n) {
    void *p = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, n ? n : 1, &p))) return NULL;
    memset(p, 0, n);
    return p;
}

void xfree(void *p) {
    if (p) gBS->FreePool(p);
}

size_t wlen(const CHAR16 *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

CHAR16 *wdup(const CHAR16 *s) {
    size_t n = wlen(s);
    CHAR16 *d = xalloc((n + 1) * sizeof(CHAR16));
    if (d) memcpy(d, s, n * sizeof(CHAR16));
    return d;
}

CHAR16 *wcat3(const CHAR16 *a, const CHAR16 *b, const CHAR16 *c) {
    size_t la = wlen(a), lb = wlen(b), lc = wlen(c);
    CHAR16 *d = xalloc((la + lb + lc + 1) * sizeof(CHAR16));
    if (!d) return NULL;
    memcpy(d, a, la * 2);
    memcpy(d + la, b, lb * 2);
    memcpy(d + la + lb, c, lc * 2);
    return d;
}

CHAR16 wlower(CHAR16 c) {
    return (c >= 'A' && c <= 'Z') ? (CHAR16)(c + 32) : c;
}

int wicmp(const CHAR16 *a, const CHAR16 *b) {
    for (;; a++, b++) {
        CHAR16 x = wlower(*a), y = wlower(*b);
        if (x != y || !x) return (int)x - (int)y;
    }
}

bool wends_with_i(const CHAR16 *s, const CHAR16 *suffix) {
    size_t ls = wlen(s), lx = wlen(suffix);
    return ls >= lx && wicmp(s + ls - lx, suffix) == 0;
}

void wcopy(CHAR16 *dst, size_t cap, const CHAR16 *src) {
    if (!cap) return;
    dst[0] = 0;
    wappend(dst, cap, src);
}

void wappend(CHAR16 *dst, size_t cap, const CHAR16 *src) {
    size_t n = wlen(dst);
    while (src && *src && n + 1 < cap) dst[n++] = *src++;
    if (n < cap) dst[n] = 0;
}

void wappend_u64(CHAR16 *dst, size_t cap, uint64_t v) {
    CHAR16 buf[24];
    int i = 23;
    buf[i] = 0;
    do { buf[--i] = (CHAR16)('0' + v % 10); v /= 10; } while (v);
    wappend(dst, cap, buf + i);
}

void wappend_hex(CHAR16 *dst, size_t cap, uint64_t v) {
    CHAR16 buf[24];
    int i = 23;
    buf[i] = 0;
    do { buf[--i] = (CHAR16)"0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
    buf[--i] = 'x';
    buf[--i] = '0';
    wappend(dst, cap, buf + i);
}

void wformat_size(CHAR16 *dst, size_t cap, uint64_t bytes) {
    dst[0] = 0;
    if (bytes < 1024) {
        wappend_u64(dst, cap, bytes);
        wappend(dst, cap, L" B");
        return;
    }
    static const CHAR16 *units[] = { L" KiB", L" MiB", L" GiB" };
    int u = 0;
    uint64_t scaled = bytes * 10 / 1024; /* tenths */
    while (scaled >= 10240 && u < 2) { scaled /= 1024; u++; }
    wappend_u64(dst, cap, scaled / 10);
    if (scaled < 100) { /* one decimal for small values */
        wappend(dst, cap, L".");
        wappend_u64(dst, cap, scaled % 10);
    }
    wappend(dst, cap, units[u]);
}

const CHAR16 *status_str(EFI_STATUS s) {
    switch (s) {
    case EFI_SUCCESS: return L"Success";
    case EFI_LOAD_ERROR: return L"Load Error";
    case EFI_INVALID_PARAMETER: return L"Invalid Parameter";
    case EFI_UNSUPPORTED: return L"Unsupported";
    case EFI_BAD_BUFFER_SIZE: return L"Bad Buffer Size";
    case EFI_BUFFER_TOO_SMALL: return L"Buffer Too Small";
    case EFI_NOT_READY: return L"Not Ready";
    case EFI_DEVICE_ERROR: return L"Device Error";
    case EFI_WRITE_PROTECTED: return L"Write Protected";
    case EFI_OUT_OF_RESOURCES: return L"Out of Resources";
    case EFI_VOLUME_CORRUPTED: return L"Volume Corrupted";
    case EFI_NO_MEDIA: return L"No Media";
    case EFI_NOT_FOUND: return L"Not Found";
    case EFI_ACCESS_DENIED: return L"Access Denied";
    case EFI_TIMEOUT: return L"Timeout";
    case EFI_ABORTED: return L"Aborted";
    case EFI_SECURITY_VIOLATION: return L"Security Violation";
    case EFI_INCOMPATIBLE_VERSION: return L"Incompatible Version";
    default: return NULL;
    }
}

#ifdef PB_DEBUG
void dbg(const char *s) {
    for (; *s; s++) __asm__ volatile("outb %0, %1" : : "a"(*s), "Nd"((uint16_t)0x402));
}
#endif

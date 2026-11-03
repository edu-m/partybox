#pragma once
#include "efi.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);

void *xalloc(size_t n);          /* zeroed pool allocation, NULL on failure */
void xfree(void *p);

size_t wlen(const CHAR16 *s);
CHAR16 *wdup(const CHAR16 *s);
CHAR16 *wcat3(const CHAR16 *a, const CHAR16 *b, const CHAR16 *c);
CHAR16 wlower(CHAR16 c);
int wicmp(const CHAR16 *a, const CHAR16 *b);
bool wends_with_i(const CHAR16 *s, const CHAR16 *suffix);
void wcopy(CHAR16 *dst, size_t cap, const CHAR16 *src);
void wappend(CHAR16 *dst, size_t cap, const CHAR16 *src);
void wappend_u64(CHAR16 *dst, size_t cap, uint64_t v);
void wappend_hex(CHAR16 *dst, size_t cap, uint64_t v);
void wformat_size(CHAR16 *dst, size_t cap, uint64_t bytes);

const CHAR16 *status_str(EFI_STATUS s);

#ifdef PB_DEBUG
void dbg(const char *s);   /* QEMU isa-debugcon on port 0x402 */
#else
#define dbg(s) ((void)0)
#endif

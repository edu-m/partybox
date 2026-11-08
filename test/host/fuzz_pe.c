/* libFuzzer target: partybox's PE classification and Authenticode check.
 * The input is the "file"; the fake EFI_FILE_PROTOCOL serves it from an exact-size heap
 * buffer so any read past what the parser was entitled to is an ASan report. */
#include <stdlib.h>
#include "../../src/scan.c"

EFI_HANDLE gImageHandle;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;

struct mf { EFI_FILE_PROTOCOL p; const uint8_t *d; size_t n; uint64_t pos; };

static EFI_STATUS EFIAPI mf_setpos(EFI_FILE_PROTOCOL *This, UINT64 pos) { ((struct mf *)This)->pos = pos; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI mf_read(EFI_FILE_PROTOCOL *This, UINTN *size, VOID *buf) {
    struct mf *f = (struct mf *)This;
    size_t avail = f->pos < f->n ? f->n - (size_t)f->pos : 0, k = *size < avail ? *size : avail;
    memcpy(buf, f->d + f->pos, k);
    f->pos += k;
    *size = k;
    return EFI_SUCCESS;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    uint8_t *copy = malloc(n ? n : 1);
    memcpy(copy, data, n);
    struct mf f = { .d = copy, .n = n };
    f.p.SetPosition = mf_setpos;
    f.p.Read = mf_read;
    struct pe_info pi;
    bool is_pe = pe_parse(&f.p, n, &pi);
    (void)pe_problem(is_pe, &pi);
    if (is_pe) (void)pe_signed(&f.p, n, &pi);
    free(copy);
    return 0;
}

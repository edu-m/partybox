/* libFuzzer target: partybox.conf parsing and the load-options "timeout=" parser.
 * First input byte picks the target. */
#include <stdlib.h>
#include "../../src/main.c"

static EFI_LOADED_IMAGE_PROTOCOL g_li;
static EFI_STATUS EFIAPI fake_handle_protocol(EFI_HANDLE h, EFI_GUID *g, VOID **iface) { *iface = &g_li; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI fake_alloc(EFI_MEMORY_TYPE t, UINTN n, VOID **p) { *p = malloc(n ? n : 1); return *p ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES; }
static EFI_STATUS EFIAPI fake_free(VOID *p) { free(p); return EFI_SUCCESS; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    static EFI_BOOT_SERVICES bs;
    bs.HandleProtocol = fake_handle_protocol;
    bs.AllocatePool = fake_alloc;
    bs.FreePool = fake_free;
    gBS = &bs;
    if (!n) return 0;
    uint8_t mode = data[0] % 2;
    data++; n--;
    uint8_t *copy = malloc(n ? n : 1);
    memcpy(copy, data, n);

    if (mode == 0) {
        struct conf c;
        memset(&c, 0, sizeof c);
        c.countdown = 5;
        conf_parse(&c, copy, n);
        if (wlen(c.selection) >= sizeof(c.selection) / sizeof(CHAR16)) __builtin_trap();
    } else if (mode == 1) {
        g_li.LoadOptions = copy;               /* exact-size: reads past the end are reported */
        g_li.LoadOptionsSize = (UINT32)n;
        (void)parse_timeout(5);
        g_li.LoadOptions = NULL;
    }
    free(copy);
    return 0;
}

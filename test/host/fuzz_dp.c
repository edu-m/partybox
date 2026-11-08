/* libFuzzer target: the device-path walker that works out which directory partybox.efi came from.
 * The input is the bytes of LoadedImage->FilePath; an END node is appended, as firmware would. */
#include <stdlib.h>
#include "../../src/scan.c"

EFI_HANDLE gImageHandle;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;

static EFI_STATUS EFIAPI fake_alloc(EFI_MEMORY_TYPE t, UINTN n, VOID **p) { *p = malloc(n ? n : 1); return *p ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES; }
static EFI_STATUS EFIAPI fake_free(VOID *p) { free(p); return EFI_SUCCESS; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    static EFI_BOOT_SERVICES bs;
    bs.AllocatePool = fake_alloc;
    bs.FreePool = fake_free;
    gBS = &bs;
    uint8_t *dp = malloc(n + 4);
    memcpy(dp, data, n);
    dp[n] = END_DEVICE_PATH_TYPE; dp[n + 1] = END_ENTIRE_DEVICE_PATH_SUBTYPE; dp[n + 2] = 4; dp[n + 3] = 0;

    /* Trust boundary: image_dir() believes node lengths and the END node, as every UEFI
     * consumer of a device path does, so only structurally valid paths are in scope here.
     * (A path with bad lengths or no END node makes it read past the buffer.) */
    for (size_t pos = 0;;) {
        if (pos + 4 > n + 4) { free(dp); return 0; }
        size_t len = dp[pos + 2] | (dp[pos + 3] << 8);
        if (len < 4 || pos + len > n + 4) { free(dp); return 0; }
        if (dp[pos] == END_DEVICE_PATH_TYPE && dp[pos + 1] == END_ENTIRE_DEVICE_PATH_SUBTYPE) break;
        pos += len;
    }
    EFI_LOADED_IMAGE_PROTOCOL li = { 0 };
    li.FilePath = (EFI_DEVICE_PATH_PROTOCOL *)dp;
    CHAR16 *d = image_dir(&li);
    if (d && wlen(d) >= PATH_MAX_CH) __builtin_trap();
    xfree(d);
    free(dp);
    return 0;
}

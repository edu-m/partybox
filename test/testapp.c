/* Test payloads for partybox. One source, behaviour picked at compile time:
 *   -DMODE_HELLO       print where we were loaded from, wait for a key, return success
 *   -DMODE_EXITDATA    return EFI_ABORTED with an exit-data string
 *   -DMODE_MODESWITCH  switch to another GOP mode, wait for a key, return
 */
#include "lib.h"

EFI_HANDLE gImageHandle;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;

static void puts16(const CHAR16 *s) { gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s); }

__attribute__((unused)) static void wait_key(void) {
    UINTN idx;
    EFI_INPUT_KEY k;
    puts16(L"\r\nPress any key to return to partybox...\r\n");
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &idx);
    gST->ConIn->ReadKeyStroke(gST->ConIn, &k);
}

__attribute__((unused)) static void print_image_path(void) {
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (void **)&li);
    puts16(L"DeviceHandle set: ");
    puts16(li && li->DeviceHandle ? L"yes\r\n" : L"no\r\n");
    puts16(L"FilePath: ");
    for (EFI_DEVICE_PATH_PROTOCOL *n = li ? li->FilePath : NULL; n && n->Type != END_DEVICE_PATH_TYPE;
         n = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)n + (n->Length[0] | (n->Length[1] << 8)))) {
        if (n->Type == MEDIA_DEVICE_PATH && n->SubType == MEDIA_FILEPATH_DP)
            puts16(((FILEPATH_DEVICE_PATH *)n)->PathName);
    }
    puts16(L"\r\n");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    gImageHandle = image;
    gST = st;
    gBS = st->BootServices;
    gRT = st->RuntimeServices;

#if defined(MODE_HELLO)
    puts16(L"Hello from a partybox test app!\r\n\r\n");
    print_image_path();
    wait_key();
    return EFI_SUCCESS;
#elif defined(MODE_EXITDATA)
    static const CHAR16 text[] = L"deliberate failure from exitdata.efi";
    CHAR16 *data = NULL;
    gBS->AllocatePool(EfiBootServicesData, sizeof(text), (void **)&data);
    memcpy(data, text, sizeof(text));
    gBS->Exit(image, EFI_ABORTED, sizeof(text), data);
    return EFI_ABORTED; /* not reached */
#elif defined(MODE_MODESWITCH)
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (EFI_ERROR(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (void **)&gop)))
        return EFI_UNSUPPORTED;
    UINT32 cur = gop->Mode->Mode, max = gop->Mode->MaxMode;
    UINT32 cw = gop->Mode->Info->HorizontalResolution;
    /* pick the first mode with a different width */
    for (UINT32 m = 0; m < max; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN sz;
        if (m == cur || EFI_ERROR(gop->QueryMode(gop, m, &sz, &info))) continue;
        if (info->HorizontalResolution != cw) { gop->SetMode(gop, m); break; }
    }
    CHAR16 buf[64] = L"Switched GOP mode to ";
    wappend_u64(buf, 64, gop->Mode->Info->HorizontalResolution);
    wappend(buf, 64, L"x");
    wappend_u64(buf, 64, gop->Mode->Info->VerticalResolution);
    puts16(buf);
    wait_key();
    return EFI_SUCCESS;
#else
#error "pick a MODE_*"
#endif
}

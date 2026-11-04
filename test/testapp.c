/* Test payloads for partybox. One source, behaviour picked at compile time:
 *   -DMODE_HELLO       print where we were loaded from, wait for a key, return success
 *   -DMODE_EXITDATA    return EFI_ABORTED with an exit-data string
 *   -DMODE_MODESWITCH  switch to another GOP mode, wait for a key, return
 *   -DMODE_LOWHOG      occupy RAM below 4 GiB so later allocations land above it,
 *                      like on PCs with lots of RAM (OVMF otherwise prefers low memory)
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
#elif defined(MODE_LOWHOG)
    /* Pages an application allocates stay allocated after it exits, so
     * everything started afterwards has to be served from above 4 GiB. A
     * small reserve is handed back for firmware buffers that must stay low. */
    CHAR16 buf[96];
    void *probe = NULL;
    gBS->AllocatePool(EfiLoaderData, 16 << 20, &probe);
    wcopy(buf, 96, L"16 MiB pool before: ");
    wappend_hex(buf, 96, (UINTN)probe);
    puts16(buf);
    puts16(L"\r\n");
    gBS->FreePool(probe);

    EFI_PHYSICAL_ADDRESS reserve = 0xFFFFFFFF;
    UINTN reserve_pages = (4 << 20) / 4096;
    bool have_reserve = !EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, reserve_pages, &reserve));
    uint64_t taken = 0;
    for (UINTN chunk = 4096; chunk >= 1; chunk /= 2) { /* 16 MiB chunks down to single pages */
        for (;;) {
            EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFF;
            if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, chunk, &a))) break;
            taken += chunk;
        }
    }
    if (have_reserve) gBS->FreePages(reserve, reserve_pages);

    probe = NULL;
    gBS->AllocatePool(EfiLoaderData, 16 << 20, &probe);
    wcopy(buf, 96, L"occupied below 4 GiB: ");
    wappend_u64(buf, 96, taken * 4096 >> 20);
    wappend(buf, 96, L" MiB\r\n16 MiB pool after:  ");
    wappend_hex(buf, 96, (UINTN)probe);
    puts16(buf);
    puts16((UINTN)probe >> 32 ? L"  (above 4 GiB)\r\n" : L"  (still below 4 GiB)\r\n");
    gBS->FreePool(probe);
    wait_key();
    return EFI_SUCCESS;
#else
#error "pick a MODE_*"
#endif
}

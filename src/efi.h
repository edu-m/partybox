/* Thin wrapper around the edk2 MdePkg headers (no edk2 libraries are used). */
#pragma once

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern EFI_HANDLE gImageHandle;
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_RUNTIME_SERVICES *gRT;

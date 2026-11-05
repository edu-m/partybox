#pragma once
#include "efi.h"

struct app {
    CHAR16 *rel;      /* path relative to the PARTYBOX dir, e.g. L"tools\\memtest.efi" */
    CHAR16 *label;    /* display name without a ".efi" extension, e.g. L"memtest" */
    CHAR16 *group;    /* sub-directory name or NULL */
    uint64_t size;
    bool is_signed;   /* PE carries an Authenticode signature (not checked against db) */
    const CHAR16 *problem; /* NULL if launchable, else why not (static string) */
};

struct scan {
    EFI_HANDLE fs_handle;     /* filesystem the PARTYBOX dir lives on */
    CHAR16 *dir;              /* absolute dir path, e.g. L"\\EFI\\partybox\\PARTYBOX"
                                 (when !found: where it was expected) */
    bool found;
    bool fallback;            /* found by searching, not next to the image */
    struct app *apps;
    int count, cap;
};

/* Work out where partybox.efi lives and enumerate <dir>\PARTYBOX. */
void scan_run(struct scan *s);
void scan_free(struct scan *s);

/* Full path to an app on s->fs_handle. Caller frees. */
CHAR16 *scan_app_path(const struct scan *s, const struct app *a);

/* The volume and directory partybox.efi was loaded from (dir like
 * L"\\EFI\\partybox", L"" for the root). Caller frees *dir. */
bool scan_image_location(EFI_HANDLE *fs, CHAR16 **dir);

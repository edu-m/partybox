# partybox

A tiny graphical EFI launcher meant to be chainloaded from another bootloader
(Limine, systemd-boot, the firmware boot menu, the UEFI shell…). On every start
it scans the `PARTYBOX` folder **next to its own .efi** and lists every EFI
application it finds (by content, not by name, so EFI-stub kernels like
`vmlinuz-linux` count too), so you can drop test programs into a folder and pick one after a
reboot without ever touching your main bootloader's config.

```
ESP
└── EFI
    └── partybox
        ├── partybox.efi
        └── PARTYBOX
            ├── memtest.efi
            ├── shell.efi
            └── mytool/            ← one level of sub-folders is scanned too,
                ├── mytool.efi        so an app can ship with its own files
                └── data.bin
```

## Features

- GOP graphics with antialiased text and a HiDPI layout (2× sizes from 2000×1400 up).
  Falls back to a plain text menu on serial-only / no-GOP systems.
- Recognises EFI applications from their PE headers: an image counts if it's
  an EFI application (Subsystem 10) built for this CPU (or EBC), whatever it's
  called. Files named `*.efi` that don't qualify are still listed, greyed out
  with the reason ("built for AArch64", "EFI driver, not an application", "not
  an EFI image"). Any other file (initrds, configs…) is ignored, as are
  dot-files such as macOS `._foo.efi` junk. Sorted, with file sizes.
- Launched apps get a proper `DeviceHandle`/`FilePath`, so they can locate
  their own files (and a nested partybox works).
- When an app returns, partybox comes back, restores its video mode if the app
  changed it, shows the exit status (plus any exit-data string), and rescans.
- **Autoboot**: on start, the selected entry (the first app) boots after a
  countdown (5 s by default). Any key cancels it. It only runs once, not again
  after an app returns.
- **Secure Boot awareness**: with Secure Boot on, apps without an Authenticode
  signature get an amber **!**, and launching one asks for confirmation first
  (Cancel is the default). See [Secure Boot](#secure-boot) for what this can and
  can't tell you.
- **Reboot**, **Firmware Settings** (sets `OsIndications` → `BOOT_TO_FW_UI` and
  resets; shown as "not supported" if the firmware doesn't advertise it),
  **Shut Down**, and **Exit** (returns to whatever started partybox).
- Firmware vendor, UEFI revision, resolution and Secure Boot state are shown
  top right.
- Disables the UEFI watchdog so the menu can sit forever.

Keys: `↑`/`↓` (or `j`/`k`), `PgUp`/`PgDn`, `Home`/`End`, `Enter` to launch,
`R`/`F5` to rescan, `Esc` to exit. In the warning dialog: `←`/`→`/`Tab`, `Enter`,
`Y`/`N`, `Esc`.

## Options

partybox reads its load options, so with Limine you pass them via `cmdline:`
(from the UEFI shell, as arguments):

| Option | |
| --- | --- |
| `timeout=N` | autoboot countdown in seconds; `timeout=0` disables autoboot (default 5) |

If the folder can't be found next to the image (some loaders don't report
where an image came from), partybox looks for the same path on every volume,
then for `\PARTYBOX` at the root of any volume.

## Building

Requires `clang`, `lld` (`lld-link`) and the edk2 `MdePkg` headers. Only the
headers are used — no edk2 build system, no gnu-efi.

edk2 is pinned as a submodule. Only `MdePkg/Include` is needed, so a sparse,
shallow checkout is enough:

```sh
git clone <this repo> partybox && cd partybox
git submodule update --init --depth 1 --filter=blob:none edk2   # or a full checkout
make                      # build/partybox.efi
make EDK2=/path/to/edk2   # or point at an edk2 tree elsewhere
```

## Versions and releases

The version lives in [`VERSION`](VERSION) (semver, e.g. `0.2.0` or
`0.3.0-rc.1`). The Makefile bakes it into the binary, and it's shown in the
header.

GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml))
builds every push and pull request. Every push to `main` also publishes a
release tagged `v<VERSION>` with `partybox.efi` and `SHA256SUMS`, marked as a
pre-release if the version has a `-suffix`. If that tag already exists, the
release step is skipped: bump `VERSION` in the commit you want to release.

## Installing with Limine

Copy `build/partybox.efi` to e.g. `/EFI/partybox/partybox.efi` on the ESP,
create `/EFI/partybox/PARTYBOX/`, and add an entry to your `limine.conf`:

```
/partybox
    protocol: efi
    path: boot():/EFI/partybox/partybox.efi
    # cmdline: timeout=10     # optional, see Options
    # resolution: 2560x1440   # optional, partybox keeps whatever mode it gets
```

Note: when a chainloaded app *returns*, Limine does not go back to its menu; it
exits as well, so partybox's **Exit** usually lands you in the firmware boot
manager. Use **Reboot** to get back to Limine.

## Secure Boot

The **!** marker means "this file has no Authenticode signature": partybox
reads the PE security directory and checks for a PKCS#7 certificate. That
check is cheap and doesn't load or measure the image, but it can't tell
whether the firmware *trusts* the signer (that would need db/dbx and a full
crypto stack). So:

- unsigned → **!** and a confirmation prompt; the firmware will almost
  certainly refuse it (edk2 reports "Access Denied", others "Security
  Violation"), unless its hash is enrolled in db;
- signed by a key that isn't in db → no **!**, but `LoadImage` still refuses it
  and partybox shows the error.

The marker and prompt only appear while Secure Boot is enabled. partybox itself
has to be signed (or its hash enrolled) to start at all under Secure Boot.

`make run-sb` boots a QEMU VM with throwaway test keys enrolled and Secure Boot
enforced. Because Limine isn't signed, partybox boots directly as
`BOOTX64.EFI` there. The `PARTYBOX` folder has one signed and two unsigned apps.
`tools/sbtest.py` is a minimal Authenticode signer for that test only; use
`sbsign`/`sbctl` with your real keys.

## Trying it in QEMU

```sh
make run     # Limine → partybox → three test apps, using OVMF
```

`make esp` builds `build/esp.img` with Limine (`/usr/share/limine/BOOTX64.EFI`),
`examples/limine.conf`, partybox, and the test payloads from `test/testapp.c`:
`hello` (prints its load path), `exitdata` (fails with an exit-data message),
`modeswitch` (changes the GOP mode, to test that partybox restores its own),
and `tools/nested-hello` (sub-folder app). Override `OVMF_CODE`, `OVMF_VARS`
or `LIMINE` if your distro puts them elsewhere.

## Layout

| File | |
| --- | --- |
| `src/main.c` | menu model, rendering, input, launching, system actions |
| `src/scan.c` | finding our own directory, enumerating `PARTYBOX`, signature check |
| `src/gfx.c` | GOP back buffer, rounded rects, alpha-blended text |
| `src/lib.c` | freestanding libc bits, UTF-16 string helpers, GUIDs |
| `src/font_data.h` | generated glyphs (`make font`, needs Pillow) |
| `tools/sbtest.py` | test-only key generator and Authenticode signer for `make run-sb` |

The embedded glyphs are rasterized from Noto Sans (SIL Open Font License 1.1),
with a few symbols from DejaVu Sans.

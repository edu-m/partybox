# partybox

A tiny graphical EFI launcher meant to be chainloaded from another bootloader
(Limine, systemd-boot, the firmware boot menu, the UEFI shell…). On every start
it scans the `PARTYBOX` folder **next to its own .efi** and lists every EFI
application it finds (by content, not by name, so EFI-stub kernels like
`vmlinuz-linux` count too), so you can drop test programs into a folder and pick one after a
reboot without ever touching your main bootloader's config.

> [!WARNING]
> **This is a hobby project, made for fun.** It exists to run UEFI apps and
> games, nothing more.
>
> - **It is not meant to be your primary bootloader.** Chainload it from a real
>   one and keep a way to boot your system that doesn't depend on partybox.
> - **Use it at your own risk.** I'm
>   not responsible for anything that happens to your machine, your data or
>   your boot setup.
>   **While you *can* use it as your primary bootloader**, it's not designed
>   to be used that way.
> - **Memory safety is not guaranteed.** It is written in C and has bugs.
> - **UEFI compliance should be up to spec, but that is not guaranteed
>   either.**

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
- Before launching an app (and on exit), partybox checks that the firmware's
  text console still matches the screen. If a loader's `resolution:` (or an
  app) shrank the display behind the console's back, the console's
  `ClearScreen` fails and text apps such as the UEFI Shell quit on startup
  with "Invalid Parameter". partybox then has the console re-apply its own
  graphics mode for the app, and restores partybox's mode afterwards.
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

Settings come from an optional `partybox.conf` in the same directory as
`partybox.efi` (see [`examples/partybox.conf`](examples/partybox.conf)):

| Key | |
| --- | --- |
| `selection=PATH` | app highlighted at start and autobooted, as its path inside `PARTYBOX` (e.g. `doom\PlayDoom.efi`; `.efi` optional) |
| `countdown=N` | autoboot countdown in seconds, 0 to 65535; `countdown=0` disables autoboot (default 5) |

One `key=value` per line. Keys are case-insensitive, blanks around keys and
values don't matter, and the file may be ASCII/UTF-8 or UTF-16LE with a BOM.
Every line that isn't a valid entry (comments, unknown keys, bad values) is
ignored, and a later valid line overrides an earlier one. A `selection` that
matches no app is reported in red under the menu, and the first app stays
selected.

partybox also reads its load options, so with Limine you can pass them via
`cmdline:` (from the UEFI shell, as arguments). They override the file:

| Option | |
| --- | --- |
| `timeout=N` | autoboot countdown in seconds; `timeout=0` disables autoboot |

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
create `/EFI/partybox/PARTYBOX/`, and, for example if using Limine as primary bootloader, add an entry to your `limine.conf`:

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

## Showcase apps

`make` also builds **`build/blackjack.efi`** ([`apps/blackjack`](apps/blackjack)), a
blackjack table drawn with GOP, to show what a bare-metal UEFI app can do.
Drop it in `PARTYBOX` and launch it from the menu. It's shipped with each
release next to `partybox.efi`.

- 6-deck shoe, dealer stands on all 17s and peeks for blackjack, blackjack
  pays 3:2, double on the first two cards (no split or insurance).
- Start with 1,000 chips; `↑`/`↓` (`PgUp`/`PgDn`) to bet 10–500, `Enter` to
  deal, `H`/`S`/`D` to hit, stand or double, `Esc` to go back to partybox.
- Deliberately conservative about firmware: GOP `Blt` for output, `ConIn`
  for input, `Stall` for pacing, nothing else. It shares partybox's
  font/drawing code.

### DOOM

[Cacodemon345/uefidoom](https://github.com/Cacodemon345/uefidoom) is pinned as
the `apps/uefidoom` submodule. `make doom` builds it on Linux into
`build/doom.efi`:

```sh
git submodule update --init edk2 apps/uefidoom   # edk2 needs a full checkout here
make doom
```

[`apps/uefidoom-build/build.sh`](apps/uefidoom-build/build.sh) compiles
edk2's BaseTools if needed, fetches edk2-libc and AudioPkg at pinned commits,
applies the [patches](apps/uefidoom-build/patches) to a copy of the sources
(GCC portability, keeping the current video mode with a frame scaled to fit,
playing the IWADs' v1.9 demos, real key events with WASD and a saner mouse,
real-time pacing with the WAD in RAM and direct framebuffer drawing, a
defensive keyboard hand-back, PC speaker and HD Audio sound), and builds with
edk2's `GCC` toolchain using [`DoomBuild.dsc`](apps/uefidoom-build/DoomBuild.dsc).
It then builds `build/AudioDxe.efi`, the HD Audio driver the game uses for
digital sound, from the same AudioPkg checkout plus
[`patches-audiodxe/`](apps/uefidoom-build/patches-audiodxe). Everything lands
in `build/doomws`; the submodules stay pristine.

Use a source build rather than old prebuilt binaries. Those predate upstream's
"Fix hang during initialization", which stops 64-bit pointers being truncated
to 32 bits. The bug is invisible in QEMU but freezes real PCs with RAM above
4 GiB during `R_Init` (a page fault with interrupts off: even Num Lock stops
responding). Run the `lowhog` test app first to reproduce that situation in
QEMU (see below).

**Controls.** UEFI's keyboard interface only reports keystrokes with typematic
repeat, never key releases, and only the last key pressed repeats. So
`doom.efi` takes USB HID boot keyboards and the PS/2 controller over from the
firmware while it runs, which gives it real key-down/key-up events: held keys
respond immediately and several keys work at once. The firmware drivers are
reattached when it quits, including on errors. Any other keyboard still works
through the firmware, with a key counting as held until its repeats stop.

- W/A/S/D or the arrows to move (A/D strafe; `,`/`.` too), mouse to turn,
  left button or Space to fire, E to use, Shift to run, 1–7 for weapons.
- Mouse turning uses raw counts (as Chocolate Doom does); vertical mouse
  movement doesn't walk.
- Switches, added to the `doom.efi` line of your `startup.nsh`:
  `-mousescale <percent>` adjusts mouse speed on top of the in-game
  sensitivity (firmwares differ wildly in how they report mouse movement:
  values from 50 to 3500 have been needed); `-vertmouse` re-enables walking
  with the mouse; `-conin` skips the keyboard takeover in case some firmware
  objects to it; `-fps` shows frames/s, ms per frame, the drawing path (`fb`
  direct framebuffer or `blt` GOP) and the mouse resolution/peak movement the
  firmware reports, in the HUD message line; `-blt` forces GOP `Blt` drawing.

**Sound.** Effects play through one of two paths, chosen at startup:

- *HD Audio.* If an `AudioDxe` driver is loaded, `doom.efi` mixes effects in
  software (16 voices with vanilla's volume, stereo separation and pitch) into
  a 44.1 kHz 16-bit stereo stream to the codec's line-out or speaker port,
  about 90 ms behind the game. Firmware never ships an HD Audio driver, so load
  the one `make doom` builds from the shell before the game. `make esp` puts it
  at `doom\drivers\AudioDxe.efi` (a subfolder, so partybox doesn't list it as
  an app), and [`examples/doom-startup.nsh`](examples/doom-startup.nsh) runs
  `load drivers\AudioDxe.efi` when it's there. The driver comes from the
  AudioPkg fork pinned in `build.sh`, with two fixes: it never got codec
  responses from QEMU's `intel-hda` (RIRB interrupt count 0 stalls the
  command ring), and its end-of-stream callback used the wrong calling
  convention and crashed. It supports the usual Realtek/Intel-style codecs;
  check the driver's output at startup if nothing is found. USB audio is out
  of reach: UEFI USB stacks have no isochronous transfers.
- *PC speaker.* Otherwise the IWADs' PC-speaker versions of every effect (the
  `DP*` lumps) are fed to the 8254 timer directly, so this works on any board
  with a beeper wired up, with no firmware support at all. One voice, as in
  1993.

`-pcspeaker` forces the speaker even with a driver present, `-nopcspeaker`
turns it off, `-nosfx` turns both off, `-sndout N` picks another codec
output (the startup text lists them: rear line-out, front headphones...).

If a machine stays silent, the startup text says why: it lists the PCI audio
devices (vendor:device and class; an Intel audio DSP shows up as class 04.01
and needs the firmware's audio set to HD Audio/legacy mode), how many codecs
AudioDxe bound, every output port and the path chosen. `-sndinfo` pauses on
that text until a key is pressed, and the chosen path is also shown in the
HUD message line when the first level starts (`Sound: HD Audio out 0 Line`
or `Sound: PC speaker (no HD Audio codec)`). A PC speaker only sounds on
boards that actually have a beeper fitted. There is no music (that would need an
OPL emulator). To hear the HD Audio path in QEMU:
`make run QEMUFLAGS="-audiodev pa,id=snd -device intel-hda -device hda-output,audiodev=snd"`
(use `-audiodev wav,id=snd,path=out.wav` to record instead).

**Performance.** The game runs in real time (tics are paced by a cycle-counter
clock, and a slow frame no longer slows the game), each WAD is read into
memory once so nothing touches the firmware's file I/O during play, and frames
go straight into the framebuffer when its format allows. On a typical machine
that gives a steady 35 fps, the engine's tic rate.

`doom.efi` still has to be started from a UEFI Shell, since it finds its WADs
through the shell's current directory. Put it in a folder with the IWADs and
a shell that `cd`s there and runs it (for example a `PlayDoom.efi` shell with
[`examples/doom-startup.nsh`](examples/doom-startup.nsh) as its
`startup.nsh`). With a `doom/` folder like that next to the Makefile,
`make esp` swaps in `build/doom.efi` and adds `drivers\AudioDxe.efi`. DOOM
and the WADs aren't part of this repo.

## Trying it in QEMU

```sh
make run     # Limine → partybox → test apps and blackjack, using OVMF
make run QEMUFLAGS="-enable-kvm -cpu host"   # faster, if KVM is available
```

`make esp` builds `build/esp.img` (`ESP_MB`, default 128) with Limine
(`/usr/share/limine/BOOTX64.EFI`), `examples/limine.conf`, partybox,
`blackjack.efi`, and the test payloads from `test/testapp.c`: `hello` (prints
its load path), `exitdata` (fails with an exit-data message), `modeswitch`
(changes the GOP mode, to test that partybox restores its own),
`tools/nested-hello` (sub-folder app), and `lowhog`. `lowhog` occupies all
RAM below 4 GiB and exits, so apps launched afterwards get memory above 4 GiB,
as on real PCs (OVMF otherwise prefers low memory). Run it before an app that
"works in QEMU but hangs on hardware", with e.g.
`make run QEMU_MEM=8G QEMUFLAGS="-enable-kvm -cpu host"`. Folders listed in `ESP_EXTRA` (default:
`doom/` if present, git-ignored) are copied into `PARTYBOX` as-is. Override
`OVMF_CODE`, `OVMF_VARS` or `LIMINE` if your distro puts them elsewhere.

## Layout

| File | |
| --- | --- |
| `src/main.c` | menu model, rendering, input, launching, system actions |
| `src/scan.c` | finding our own directory, enumerating `PARTYBOX`, signature check |
| `src/gfx.c` | GOP back buffer, rounded rects, alpha-blended text |
| `src/lib.c` | freestanding libc bits, UTF-16 string helpers, GUIDs |
| `src/font_data.h` | generated glyphs (`make font`, needs Pillow) |
| `apps/blackjack/` | the blackjack showcase app and its generated card glyphs |
| `apps/uefidoom/` | DOOM port (submodule, third-party, GPL-2.0) |
| `apps/uefidoom-build/` | Linux build of it: script, `.dsc`, patches |
| `tools/genfont.py` | glyph generator: `ui` set for partybox, `cards` set for blackjack |
| `tools/sbtest.py` | test-only key generator and Authenticode signer for `make run-sb` |

The embedded glyphs are rasterized from Noto Sans (SIL Open Font License 1.1),
with a few symbols from DejaVu Sans.

## License

partybox itself is released under the [MIT License](LICENSE). Third-party parts
keep their own licenses: the embedded glyphs are rasterized from Noto Sans and
DejaVu Sans (SIL Open Font License 1.1), and the DOOM port in `apps/uefidoom`
(a submodule, with the patches in `apps/uefidoom-build`) is GPL-2.0.

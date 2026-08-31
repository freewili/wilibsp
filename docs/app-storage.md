# App files on the SD card

For an app maintained outside this monorepo, first follow
[`app-project-setup.md`](app-project-setup.md). The root agent pointer and
pinned `wilibsp/` checkout are part of the app contract, not optional
contributor convenience.

FreeWili loadable apps belong in `/apps/` as `.uf2` files. Install one from a
connected development machine with:

```bash
fw install-app build/apps/my_app/my_app.uf2
```

Organize larger collections with a relative subfolder under `/apps`:

```bash
fw install-app my_app.uf2 --folder beta/radio
```

This installs `/apps/beta/radio/my_app.uf2`. The command creates missing
directories and rejects absolute paths, backslashes, empty components, and
`.`/`..` components before it touches the device.

The command finds MAIN with fwFinder, asks MAIN to hand the SD card to the USB
reader, waits for the drive to mount, copies the UF2 into `/apps/`, safely
unmounts it, and returns the card to MAIN. Use `--device SERIAL` when more than
one FreeWili is connected. `--port COM44` (or the corresponding POSIX device)
is an explicit fallback for legacy USB identities fwFinder cannot recognize.
The host needs the `pyfwfinder` and `pyserial`
Python packages; the command reports either missing dependency directly.

Before touching the device, `fw install-app` parses every UF2 block and refuses
anything targeting QSPI flash. Loadable DISPLAY apps must target SRAM
(`0x20000000..0x20070000`) or PSRAM (`0x11000000..0x11800000`) consistently.
This prevents an app from replacing the stock DISPLAY firmware. DISPLAY's
recovery loader itself is immutable OTP code, not a flash-resident region;
firmware replacement is an explicit maintenance workflow, not app installation.

`fw flash` enforces the same rule on the ELF it programs over the debug probe,
so the two paths onto the device agree. A `fw2_display_app()` target is
`no_flash` and loads into SRAM, so `fw flash` remains the normal debug loop for
it. What `fw flash` now refuses is an image *stored* in flash — most often
`pico_set_binary_type(copy_to_ram)`, which runs from SRAM but is stored at flash
base, so programming it replaces the DISPLAY firmware without ever saying so.
Deliberate firmware replacement needs `fw flash --replace-display-firmware`.

## Publishing apps

The loadable `.uf2` is part of the FreeWili app contract. App repositories
must attach the validated UF2 as a downloadable release artifact for every
published app release. A source tag or a short-lived CI artifact alone is not
enough: testers and users must be able to download that exact release UF2
without reproducing the embedded toolchain locally.

If the app's source repository is public, the app must also provide an About
screen that shows the app version and a link to that repository. The About
screen may be intentionally tucked away so it does not distract from the
feature UI; the conventional gesture is holding the PAGE button for five
seconds. Another discoverable gesture or menu entry is acceptable, but the
version and repository link must be readable on the device without RTT or a
development host.

## PSRAM-resident app startup

An app whose executable image lives in PSRAM needs a small **SRAM bootstrap**.
Do not call this BOOTRAM: RP2350 BOOTRAM is special memory used by the boot ROM
and is not the application's general-purpose startup region.

The DISPLAY app loader initializes QMI CS1 and fills the PSRAM window before it
jumps to the app. From that point onward, code fetched from PSRAM depends on
that QMI configuration remaining valid. A normal cold-boot CRT may reset boot
ROM state, peripherals, clocks, or QMI-related state; running those steps from
PSRAM can invalidate the bus carrying the next instruction and leave the
display black.

For a PSRAM-resident app:

- Keep the vector table at the beginning of the PSRAM image. Its initial stack
  pointer must point into SRAM.
- Put the C/C++ runtime entry and any clock/QMI-sensitive boot routines in
  SRAM. A minimal reset handler may begin in PSRAM only long enough to copy
  this bootstrap into SRAM and enter it.
- Treat PSRAM as already initialized by the loader. Do not run the SDK's
  cold-boot PSRAM setup again while executing from that same PSRAM window.
- Skip or replace cold-boot reset and clock initializers that would disturb the
  inherited loader state. Initialize ordinary application peripherals after
  the SRAM bootstrap takes control.
- Perform every clock transition and PSRAM timing update from SRAM. Adjust the
  QMI timing before raising the system clock so PSRAM never exceeds its bus
  limit during the transition.

These requirements apply to apps **executing from PSRAM**, not to ordinary BSP
apps linked and loaded directly into SRAM with the `no_flash` binary type.

See `apps/hello_psram_exec` for a minimal freestanding implementation: custom
linker script, PSRAM reset stub, SRAM bootstrap, PSRAM-only UF2 generator, and
post-link placement checks. It deliberately avoids the normal cold-boot CRT.

Before publishing a PSRAM app, make the build verify all of the following:

1. The first vector-table words contain an SRAM stack pointer and a PSRAM reset
   address.
2. The runtime entry and clock/QMI-sensitive startup symbols resolve inside
   SRAM (`0x20000000..0x20070000`), not PSRAM.
3. Every UF2 payload block targets PSRAM (`0x11000000..0x11800000`) and none
   targets QSPI flash. `fw install-app` enforces the UF2 portion before copying.
4. On hardware, the loader reports success and the app reaches an observable
   runtime milestone such as display output, USB enumeration, or diagnostics.

## App-owned data

Apps must keep app-owned persistent data under `/appdata/<app-name>/`; for
example, Meshtastic uses `/appdata/meshtastic/`. This includes preferences,
saves, logs, generated maps, caches, and similar files. Create `/appdata/` and
the app-specific directory before the first write. Do not put app-owned files
at the SD root or directly in `/appdata/`.

User-selected exports and deliberately shared or interoperable files may live
elsewhere. When an app uses such a path, make that intent clear in its UI or
documentation. Do not hide user-authored files inside `/appdata/` merely to
satisfy the app-owned-data rule.

## Two-processor apps (DISPLAY + ESP32)

An app whose custom code runs on both the DISPLAY CPU and the ESP32-C5 ships
two artifacts, and they load by different mechanisms:

| Artifact | Lands at | Loaded by |
| --- | --- | --- |
| DISPLAY app `.uf2` | `/apps/<app>.uf2` | the DISPLAY app loader (on-device, or `fw run-app`) |
| ESP32 firmware | `/appdata/<app>/esp32/` | MAIN's ESP32 flasher, reading `flasher_args.json` |

Install both in one SD handoff:

```bash
fw install-bundle build/apps/my_app/my_app.uf2 ../my-esp-fw/build.esp32c5
```

It verifies the UF2 exactly as `fw install-app` does, validates the ESP32
manifest, copies both, returns the card to MAIN, then asks MAIN to flash the
ESP32 and polls until it finishes. `--no-flash-esp` stages the image without
flashing and prints the device-console line to run later. `--name` overrides the
`/appdata` folder, which defaults to the UF2 stem. `--folder` places the UF2
under `/apps/<folder>/` the same way `fw install-app` does.

**Only the manifest and the binaries it references are copied** — an `idf.py`
build folder is around 250 MB, of which the device needs about 1.5 MB.

MAIN's manifest parser is strict, and `fw install-bundle` enforces the same
limits on the PC so a bad bundle fails before the card has been handed around:
`flasher_args.json` must be at most 4 KB and list at most 6 partitions, and
every file it references must exist.

### Flashing the ESP32 is not free

It reboots the radio. MAIN drives BOOT/EN, syncs at 115200, upgrades to 460800,
writes each partition, then resets the ESP32 back into its application — tens of
seconds, and Wi-Fi and BLE are down throughout. It also needs **power zone 5**.
Do not reflash on every app launch. Have the DISPLAY app ask the ESP32 for its
version (`BNOSE_CMD_GET_INFO` returns one) and reflash only on a mismatch.

### Version skew is the failure mode to design for

The two halves are installed by separate mechanisms, so they *will* drift: a
user copies a new UF2 by hand, or an ESP32 flash fails halfway. Give the app a
version check on startup rather than assuming the pair matches. Note that both
`applink_send()` and MAIN's relay are fire-and-forget — a DISPLAY app talking to
the wrong ESP32 build gets silence, not an error.

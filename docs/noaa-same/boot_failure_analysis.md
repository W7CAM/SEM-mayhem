# NOAA/SAME Boot Failure Analysis

Version: v0.1  
Date: 2026-10-12  
Branch: `feature/noaa-same`  
Baseline: `b0d9973ce1dae7c7d06508a47325987cd94d1e49` (`next`)  
Analyzed NOAA branch head: `65307e5a5945e52be8efea956e3e438a5dc60b21`

## Scope

This note analyzes the reported boot failure after installing a NOAA/SAME build on a standard HackRF One R10 with PortaPack H4M. The target is explicitly the standard HackRF One/PortaPack H4M path, not HackRF Pro/PRALINE.

Reported behavior:

- Custom firmware image built successfully.
- Device showed black screen with RX LED flashing.
- Removing the SD card did not restore normal boot.
- Hardware DFU remained available.
- Recovery through LPC43xx DFU, temporary HackRF USB firmware in RAM, `hackrf_info` showing `RunningFromRAM` on R10, and flashing known-good Mayhem with `hackrf_spiflash` succeeded.
- Known-good Mayhem and Looking Glass subsequently worked.

That recovery strongly suggests the hardware is functional and the incident was caused by firmware image, target, layout, or installation state.

## Build Configuration Checked

Both the baseline and NOAA branch were built with the existing Mayhem default HackRF configuration in Docker using the ARM Cortex-M toolchain file:

- Baseline worktree: `/private/tmp/sem-mayhem-baseline`
- Baseline build dir: `/private/tmp/sem-mayhem-baseline/build-baseline`
- NOAA build dir: `build-hackrf-h4m`
- Toolchain file in both CMake caches: `/havoc/firmware/toolchain-arm-cortex-m.cmake`
- No PRALINE build target was selected for the corrected NOAA build.

The current NOAA build completed as a standard HackRF/PortaPack package and generated:

- `build-hackrf-h4m/firmware/portapack-mayhem-firmware.bin`
- `build-hackrf-h4m/firmware/portapack-mayhem_OCI.ppfw.tar`
- `build-hackrf-h4m/firmware/baseband/PNOS.bin`
- `build-hackrf-h4m/firmware/application/noaa_same.ppma`

The OCI package contains both `FIRMWARE/portapack-mayhem_dev.bin` and `APPS/noaa_same.ppma`.

## Artifact Comparison

| Artifact | Baseline size | NOAA branch size | Delta |
| --- | ---: | ---: | ---: |
| Raw firmware `.bin` | 1,048,576 | 1,048,576 | 0 |
| OCI package `.tar` | 3,010,560 | 3,041,280 | +30,720 |
| `application.bin` | 688,372 | 688,836 | +464 |
| `baseband.img` | 315,680 | 315,680 | 0 |
| Baseband `*.bin` count | 63 | 64 | +1 |
| OCI app count | 101 | 102 | +1 |
| `PNOS.bin` | N/A | 20,020 | +20,020 |
| `noaa_same.ppma` | N/A | 28,204 | +28,204 |

The NOAA branch build also reported the external app region check passing. The remaining flash ROM margin decreased only from 44,520 bytes to 44,056 bytes.

The NOAA baseband ELF size from `arm-none-eabi-size`:

```text
text=19723 data=288 bss=11356 dec=31367
```

The generated `noaa_same.ppma` bundles the `PNOS` M4 image tag and the app-specific baseband payload. The export path appends the baseband image to the external app and checks the combined app plus M4 image against the standard HackRF `m4_code` region before writing the `.ppma`.

## Source Delta Relevant To Boot

The NOAA milestone changed these areas:

- External application registration and UI:
  - `firmware/application/external/noaa_same/main.cpp`
  - `firmware/application/external/noaa_same/ui_noaa_same.cpp`
  - `firmware/application/external/noaa_same/ui_noaa_same.hpp`
  - `firmware/application/external/external.cmake`
  - `firmware/application/external/external_tier*.ld`
- Baseband processor:
  - `firmware/baseband/proc_noaa_same.cpp`
  - `firmware/baseband/proc_noaa_same.hpp`
  - `firmware/baseband/CMakeLists.txt`
- Shared plumbing:
  - `firmware/application/baseband_api.cpp`
  - `firmware/application/baseband_api.hpp`
  - `firmware/common/message.hpp`
  - `firmware/common/spi_image.hpp`
- Documentation:
  - `docs/noaa-same/architecture.md`

The only new SPI image tag is `image_tag_noaa_same{'P','N','O','S'}` in `firmware/common/spi_image.hpp`.

## Boot-Path Findings

The NOAA DSP code is not on the early boot path. The application startup path eventually calls the UI event loop, and only after leaving PortaPack mode does it load the HackRF USB image:

- `firmware/application/main.cpp:203` calls `portapack::init()`.
- `firmware/application/main.cpp:217` enters `event_loop()`.
- `firmware/application/main.cpp:231` calls `m4_init(image_tag_hackrf, m4_code_hackrf, true)` after the event loop exits.

The NOAA baseband image is requested only when the external NOAA app view is constructed:

- `firmware/application/external/noaa_same/main.cpp:25` defines the external app metadata.
- `firmware/application/external/noaa_same/main.cpp:70` declares the bundled M4 tag `PNOS`.
- `firmware/application/external/noaa_same/ui_noaa_same.cpp:57` calls `baseband::run_image(image_tag_noaa_same)`.

External app discovery reads only the `.ppma` header while browsing menus:

- `firmware/application/ui_external_items_menu_loader.cpp:220` iterates SD-card `.ppma` files.
- `firmware/application/ui_external_items_menu_loader.cpp:228` reads `application_information_t`.
- `firmware/application/ui_external_items_menu_loader.cpp:252` defers full app loading until selection.
- `firmware/application/ui_external_items_menu_loader.cpp:317` starts the actual external app load path.

Therefore, with the SD card removed, the NOAA external app package should not be involved in menu discovery or app launch. A black screen with SD removed is more consistent with the flashed internal firmware image or installation state than with the `noaa_same.ppma` file alone.

## Plausible Root Causes

### 1. Wrong firmware artifact or wrong target image installed

This is the highest-risk operational explanation. A PRALINE/HackRF Pro image, a corrupted file, or a package artifact flashed as if it were the raw firmware image can produce a non-booting PortaPack while still leaving DFU recovery available. The corrected NOAA build inspected here is a standard HackRF build, but the reported failure occurred before this analysis and cannot be proven to have used this exact artifact.

Diagnostic: compare SHA-256 and size of the flashed file against `build-hackrf-h4m/firmware/portapack-mayhem-firmware.bin` before any retry.

### 2. Raw firmware image issue in current branch

The current branch raw firmware image is the expected 1 MiB size, fits in flash, and differs only modestly in the M0 application binary. The added NOAA app code is removed from `application.bin` by the external app packaging rule, leaving only metadata/plumbing in the internal firmware.

This remains testable only on hardware: flash the corrected current raw `.bin` with SD removed and verify splash/menu boot before copying any NOAA `.ppma` file.

### 3. External app packaging or header issue

If `APPS/noaa_same.ppma` is present, the menu loader will read its header during RX menu construction. The package layout appears conventional and the app count increases by exactly one. A malformed header could affect menu loading, but it should not explain a black screen when the SD card is removed.

Diagnostic: boot corrected raw firmware with no SD card, then boot with SD card installed but without `APPS/noaa_same.ppma`, then boot with `APPS/noaa_same.ppma`.

### 4. PNOS baseband image table issue

The branch adds one baseband target with `DeclareTargets(PNOS noaa_same)` in `firmware/baseband/CMakeLists.txt:741`. Startup does not execute `PNOS`; the NOAA view requests it later. The packed `baseband.img` size is unchanged from baseline and the raw firmware remains within flash limits.

This is lower probability than a wrong artifact/install issue, but it can be isolated by building a diagnostic variant with the NOAA external app disabled while keeping `PNOS`, and another with `PNOS` removed while keeping only the app metadata.

### 5. NOAA runtime DSP crash

The NOAA baseband processor and UI can crash the device if launched and faulty, but they should not run before selecting the app. This explanation does not fit an immediate black-screen boot failure with the SD card removed.

## Recommended Hardware Validation Matrix

Use only standard HackRF One/H4M artifacts. Do not switch to PRALINE to obtain a passing boot.

1. Record SHA-256 and size of:
   - Known-good Mayhem raw firmware.
   - Corrected NOAA raw firmware: `build-hackrf-h4m/firmware/portapack-mayhem-firmware.bin`.
   - Corrected NOAA OCI package: `build-hackrf-h4m/firmware/portapack-mayhem_OCI.ppfw.tar`.
2. Flash only the corrected NOAA raw `.bin` with `hackrf_spiflash`.
3. Boot with SD card removed.
   - Expected: PortaPack splash/menu.
   - If this fails, the issue is in raw firmware, target, image layout, or flash/install path.
4. Boot with SD card inserted but without `APPS/noaa_same.ppma`.
   - Expected: existing apps continue to function.
5. Copy `APPS/noaa_same.ppma` from the OCI package to the SD card and boot again.
   - Expected: RX menu remains usable and NOAA SAME appears.
6. Launch NOAA SAME.
   - Expected: app opens, audio remains muted by default, and PNOS starts only at this point.
7. Exercise Rescan, preset load/save, and manual decoder assignment.
   - Expected: no automatic audio enablement and no reset of unrelated apps.

Additional isolation builds, if step 3 or 5 fails:

- Build with `noaa_same` removed from `external.cmake` but `PNOS` still declared.
- Build with `PNOS` removed from `baseband/CMakeLists.txt` but external app metadata retained only if safe.
- Build with both the external NOAA app and `PNOS` removed, leaving only shared message/API additions.

## Recovery Procedure Confirmation

The reported recovery path is appropriate for a bad flash image on standard HackRF One hardware:

1. Enter LPC43xx DFU mode.
2. Load temporary HackRF USB firmware into RAM.
3. Confirm `hackrf_info` reports R10 and `RunningFromRAM`.
4. Use `hackrf_spiflash -w <known-good-portapack-mayhem-firmware.bin>`.
5. Reboot and verify known-good Mayhem.

This should remain the fallback procedure for each diagnostic flash attempt.

## Conclusions

- The corrected NOAA branch build inspected here uses the standard HackRF One/H4M build path, not PRALINE.
- The current artifacts include both the new `PNOS` baseband image and `APPS/noaa_same.ppma`.
- Static source inspection does not identify a NOAA code path that should execute before the app is selected.
- The black-screen symptom with SD removed points first to wrong artifact, wrong target, corrupted flash image, or install procedure, not to the NOAA external app file alone.
- Real-time behavior and boot behavior of the corrected NOAA image have not been validated on physical H4M hardware in this workspace.

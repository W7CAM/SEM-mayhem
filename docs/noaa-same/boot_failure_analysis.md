# NOAA/SAME Boot Failure Analysis

Version: v0.2<br>
Date: 2026-10-12<br>
Branch: `feature/noaa-same`<br>
Baseline: `b0d9973ce1dae7c7d06508a47325987cd94d1e49` (`next`)<br>
Failed-image source head: `0ab910c9ea9cfac66fa5b6821c220f5630d3b839`<br>
Candidate change: remove NOAA external-app dynamic startup initializer

## Test B Addendum

Test B flashed the custom raw firmware image with the SD card removed. The device still failed to boot. This eliminates the SD-card package and external `.ppma` loading as necessary causes.

The failed SHA-256 was matched to the local raw build artifact before rebuilding:

```text
7fed03eccd461c2f37831002650da159bc9b5649ec8c103db92a1bff320b24cd  build-hackrf-h4m/firmware/portapack-mayhem-firmware.bin
```

After applying the diagnostic candidate change described below, the rebuilt raw firmware SHA-256 is:

```text
59d0c416459be5a0843b28b62bf2c1b00f0080d1f6ff415e8d9b31342e79d3d8  build-hackrf-h4m/firmware/portapack-mayhem-firmware.bin
```

This new image is the single highest-value next hardware experiment. It keeps the NOAA feature, PNOS baseband registration, and external app registration present, but removes the startup-time dynamic initializer that points into the synthetic external-app address range. Passing or failing this image cleanly separates the suspected boot-time constructor fault from the remaining PNOS/shared-plumbing hypotheses.

Do not treat this as a verified fix until the image boots on the HackRF One R10 + PortaPack H4M.

## Ranked Root Cause After Test B

1. Boot-time C++ initializer points into external app link space

   The failed image contains an extra startup initializer entry at application offset `0x434`:

   ```text
   failed application.bin @ 0x430:
   49040000 191a14ae 55460000 d1460000 ...
   ```

   Interpreted little-endian, `191a14ae` is `0xAE141A19`, a Thumb function pointer into `ram_external_app_noaa_same`. That address range is a synthetic linker region used for external app packaging; it is not valid executable memory at normal M0 boot.

   The map identifies the target as:

   ```text
   ae141a18 t _GLOBAL__sub_I__ZN2ui12external_app9noaa_same12NoaaSameViewC2ERNS_14NavigationViewE
   ```

   The source cause was global dynamic initialization in `firmware/application/external/noaa_same/ui_noaa_same.cpp`:

   ```cpp
   const std::filesystem::path noaa_dir{u"/NOAA"};
   const std::filesystem::path presets_path{u"/NOAA/PRESETS.TXT"};
   ```

   The linker correctly placed the NOAA external app code and data in `ram_external_app_noaa_same`, but the `.init_array` entry was left in the internal application startup path. On boot, the C++ runtime would call through a pointer to `0xAE141A19`, causing an early fault before UI startup.

2. PNOS/baseband image registration

   PNOS remains a lower-probability cause. The failed and candidate images both include `PNOS.bin`, and `baseband.img` remains 315,680 bytes. Startup does not request PNOS; it is only loaded when the NOAA view calls `baseband::run_image(image_tag_noaa_same)`.

   This should still be isolated if the candidate image fails to boot.

3. Shared-message ABI effects

   The added message IDs and classes are not the leading cause. `Message::MAX_SIZE` remains 512 bytes, `MessageQueue` stores byte copies into fixed queue buffers, and `SharedMemory` stores queues, pointers, and a separate 512-byte `bb_data` union. Adding `NoaaSameConfigureMessage` and `NoaaSameStatusMessage` does not resize the queue buffers or shared memory layout by itself.

4. General flash/header/layout corruption

   The raw image size remains 1,048,576 bytes. The initial vector table is structurally sane in baseline, failed, and candidate images:

   ```text
   initial SP: 0x20000400
   failed reset vector:    0x00008085
   baseline reset vector:  0x0000807d
   candidate reset vector: 0x00008085
   ```

   The reset-vector shift is consistent with ordinary code layout movement and is not suspicious by itself.

## Boot Layout Evidence

The raw firmware generator concatenates `application.bin`, `baseband.img`, padding, and a checksum into a 1 MiB SPI image. It rejects images that exceed flash capacity and reports remaining flash space.

Current bootloader/runtime expectations from source:

- `firmware/tools/make_spi_image.py` reads application and baseband binaries, pads them, and writes the raw image plus checksum.
- `firmware/common/spi_image.hpp` defines the application region starting at flash offset `0x00000`.
- `firmware/common/spi_image.hpp` defines the baseband image region as starting at `_textend`, after the internal application image.
- `firmware/application/core_control.cpp` scans the SPI image chunk list from `spi_flash::images.base()` and loads an M4 image by tag.
- `firmware/application/main.cpp` enters the PortaPack event loop before loading `image_tag_hackrf` after exit.

The boot failure occurs before any SD-card `.ppma` load is necessary, and the binary evidence now shows an internal application startup-table entry pointing at the NOAA external-app region.

## Candidate Diagnostic Change

The diagnostic candidate changes the two global `std::filesystem::path` objects into functions returning path temporaries:

```cpp
std::filesystem::path noaa_dir() {
    return {u"/NOAA"};
}

std::filesystem::path presets_path() {
    return {u"/NOAA/PRESETS.TXT"};
}
```

After this change, the startup table at `application.bin` offset `0x430` no longer contains a NOAA external-app pointer:

```text
candidate application.bin @ 0x430:
49040000 55460000 d1460000 4d470000 05480000 00000000 ...
```

The map still contains NOAA code under `ram_external_app_noaa_same`, as expected, but `.init_array` contains only the normal internal/libstdc++ startup entries:

```text
.init_array 0x00000430 /tmp/application.elf...ltrans0.ltrans.o
.init_array 0x00000434 libstdc++_nano.a(cxx11-locale-inst.o)
.init_array 0x00000438 libstdc++_nano.a(cxx11-wlocale-inst.o)
.init_array 0x0000043c libstdc++_nano.a(locale-inst.o)
.init_array 0x00000440 libstdc++_nano.a(wlocale-inst.o)
```

Current candidate artifact sizes:

| Artifact | Size |
| --- | ---: |
| `portapack-mayhem-firmware.bin` | 1,048,576 |
| `application.bin` | 688,836 |
| `baseband.img` | 315,680 |
| `noaa_same.ppma` | 28,132 |

The package still contains both `FIRMWARE/portapack-mayhem_dev.bin` and `APPS/noaa_same.ppma`.

## Minimal Diagnostic Test Matrix

Test first:

1. `candidate-noaa-no-global-path-init`
   - Artifact: `build-hackrf-h4m/firmware/portapack-mayhem-firmware.bin`
   - SHA-256: `59d0c416459be5a0843b28b62bf2c1b00f0080d1f6ff415e8d9b31342e79d3d8`
   - SD card: removed
   - Expected diagnostic value: highest. If this boots, the `0xAE141A19` startup initializer was the root cause. If it fails, continue below.

If the candidate fails:

2. `shared-plumbing-only`
   - Keep `message.hpp` additions and `baseband_api` plumbing.
   - Remove NOAA external app registration and PNOS declaration.
   - Purpose: isolate message/API changes from image/app registration.

3. `shared-plus-pnos`
   - Keep shared plumbing and `DeclareTargets(PNOS noaa_same)`.
   - Remove NOAA external app registration.
   - Purpose: test whether adding the baseband image changes packed image layout enough to affect boot.

4. `shared-plus-external-metadata-no-pnos`
   - Keep NOAA external app metadata but remove bundled PNOS dependency and any dynamic initialization.
   - Purpose: test external-app registration/linker layout without adding the PNOS baseband image.

5. `full-noaa-with-runtime-paths`
   - Current candidate source: full NOAA feature retained, no global filesystem path construction.
   - Purpose: confirm the app can appear and launch after raw boot is proven.

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

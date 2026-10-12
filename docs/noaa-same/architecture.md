# NOAA Weather Radio + SAME decoder architecture assessment v0.6

Baseline: Mayhem `next` at `b0d9973ce1dae7c7d06508a47325987cd94d1e49` (`FM Radio: Restore saved bandwidth when loading a favorite (#3363)`).

Target: standalone HackRF PortaPack H4M operation under Mayhem firmware.

v0.6 status: first functional monitoring milestone implemented. The milestone adds seven-channel NOAA power measurement, a hard limit of three FM decoder slots, live user-driven reassignment, preset persistence, and a muted-by-default UI. Complete SAME decoding remains intentionally unimplemented.

## Goals and constraints

- Monitor the seven primary NOAA Weather Radio channels: 162.400, 162.425, 162.450, 162.475, 162.500, 162.525, and 162.550 MHz.
- Decode SAME from every RSSI-qualified channel without requiring a host computer.
- Keep Mayhem's existing applications and radio behavior intact.
- Default to silent operation. Weather audio, attention tones, beeps, and audible alerting must be explicit user opt-ins.
- Decode and log all valid SAME messages regardless of the user's geographic filters.
- Apply geographic filters only when deciding whether to notify audibly or auto-open weather audio.
- Retain alert history on the SD card.

## Relevant existing framework

### Application model

Mayhem splits user-interface code on the M0/application side from streaming DSP code on the M4/baseband side. The application selects a baseband image with `baseband::run_image(...)`, configures radio parameters through `ReceiverModel`, and receives decoded data and statistics from `shared_memory.application_queue`.

The existing receiver model already exposes the expected controls for a weather receiver:

- target frequency, sample rate, RF bandwidth, LNA/VGA/RF amp, antenna bias, and volume through `ReceiverModel`;
- NFM mode and multiple NFM bandwidth presets through `ReceiverModel::Mode::NarrowbandFMAudio`;
- NFM configuration mapped to `NBFMConfigureMessage` in `baseband::NBFMConfig::apply`;
- audio rate selection from baseband configuration.

This is the right integration point for a new receive application, but the existing single-channel NFM image should not be modified into the NOAA monitor because the monitor has different channelization and logging requirements.

### Baseband DSP

The reusable single-channel NFM flow is in `firmware/baseband/proc_nfm_audio.cpp`:

1. 3.072 Msps complex input.
2. first decimator to 768 ksps;
3. real halfband audio decimator to 384 ksps;
4. frequency-translating decimator to 48 ksps;
5. channel FIR/decimator to 24 ksps;
6. FM demodulation;
7. optional CTCSS detection;
8. `AudioOutput` for squelch, filtering, audio DMA, and audio statistics.

The important reusable pieces are:

- `dsp::decimate::FIRC8xR16x24FS4Decim4`;
- `dsp::decimate::FIRC16xR16x16Decim2`;
- `dsp::FrequencyTranslatingDecimator32By8`;
- `dsp::decimate::FIRAndDecimateComplex`;
- `dsp::demodulate::FM`;
- NFM FIR tap sets such as `taps_16k0_decim_0`, `taps_16k0_decim_1`, and `taps_16k0_channel`;
- `ChannelStatsCollector`/`ChannelStatisticsMessage` for channel activity;
- `RSSIThread` for front-end RSSI display.

The existing `AFSKRxProcessor` is useful as a reference for low-rate FSK slicing and message delivery, but it is not directly suitable for SAME:

- it assumes start/stop framing in its RS232-like mode;
- its continuous-trigger mode is labeled untested;
- it uses a delay discriminator tied to its selected baud and audio path rather than a protocol-specific SAME synchronizer;
- it is single-channel and would duplicate the RF decimation work if reused seven times.

The existing `same_tx` external app is useful for protocol constants and message formatting references. It uses mark/space tones near 2083 Hz and 1563 Hz, repeats the header three times, emits `0xAB` preamble bytes, and bit-reverses characters for transmission. It is a transmit helper, not a receiver or parser.

### Audio routing

`AudioOutput` already provides audio DMA feeding, squelch, high-pass/de-emphasis filtering, audio statistics, and optional streaming. That is suitable for a selected live weather-audio channel, but the NOAA/SAME monitor should keep decoding independent of audible audio.

Recommended policy:

- The baseband processor always demodulates candidate channel audio needed for SAME.
- Only one selected channel is ever routed to `AudioOutput` for live listening.
- The selected channel defaults to muted.
- SAME alert tones/beeps are generated only when the user enables audible notifications.
- Geographic filters control audible notification and optional auto-monitor behavior, not logging or decode.

### SD-card storage

The SD-card conventions already exist:

- `LogFile::append()` creates parent directories via `ensure_directory`;
- apps use dedicated root folders such as `/ADSB`, `/GPS`, `/BLESPAM`, `/SAMPLES`, `/WAV`;
- `next_filename_matching_pattern(...)` is available when rotating or generating ordinal filenames is needed.

Recommended folder layout:

- `/NOAA/SAME_LOG.CSV` for append-only decoded SAME headers and EOMs;
- `/NOAA/HISTORY.TXT` or `/NOAA/HISTORY.CSV` for compact user-facing alert history;
- `/NOAA/CONFIG.INI` only if the standard app-settings manager is insufficient;
- optional `/NOAA/AUDIO/` only for a future explicit recording feature.

History should be append-only at first, with an in-memory recent list for the UI. Implement retention controls later by age/count truncation at startup or manual cleanup, not in the baseband real-time path.

## SAME signal requirements

The current NWS SAME directive defines:

- data rate: 520.83 bits per second;
- logic zero: 1562.5 Hz;
- logic one: 2083.3 Hz;
- bit period: 1.92 ms;
- bytes are eight bits, ASCII payload, no start, stop, or parity bits;
- synchronization is obtained from a preamble at the start of each header or EOM burst.

Reference: NOAA/NWS Instruction 10-1712, Appendix A (`weather.gov/media/directives/010_pdfs/pd01017012curr.pdf`).

The receiver must therefore not use an async serial decoder requiring start/stop bits. It needs a SAME-specific AFSK demodulator with preamble acquisition, bit clock tracking, byte packing, burst de-duplication, and header/EOM grammar validation.

## Feasibility of all-channel monitoring

### RF bandwidth

The seven NOAA channels span 150 kHz from 162.400 to 162.550 MHz. Centering the HackRF at 162.475 MHz puts the channels at offsets:

- -75 kHz, -50 kHz, -25 kHz, 0, +25 kHz, +50 kHz, +75 kHz.

This is feasible in one RF tuning pass. A sample rate around 768 ksps or 3.072 Msps with a shared first decimator leaves enough digital bandwidth for all seven channels. The existing NFM app's 3.072 Msps baseband path and NFM tap sets are a practical baseline.

### DSP architecture

Recommended architecture:

1. Tune once to 162.475 MHz.
2. Set RF/baseband bandwidth wide enough for at least 150 kHz plus transition margin.
3. Run one common front-end decimator.
4. Split into seven channel states, each with:
   - fixed frequency translation for its channel offset;
   - narrow FM channel filter/decimator;
   - FM demodulator to 24 ksps or 12 ksps audio;
   - RSSI or channel-power estimate and hysteresis;
   - SAME AFSK decoder;
   - 1050 Hz tone detector if audible legacy tone indication is desired.
5. Route only the user's selected weather-audio channel to `AudioOutput`.
6. Send decoded SAME events to the application side with channel frequency, RSSI/power, timestamp, raw header text, parsed fields, and validity flags.

Avoid running seven independent copies of the full `NarrowbandFMAudio` chain. Instead, share the expensive wideband decimation and instantiate only the per-channel translating/channel/audio stages seven times.

### CPU and memory risk

All-channel monitoring is architecturally feasible, but it is the main technical risk.

Risk factors:

- Seven `FrequencyTranslatingDecimator32By8` plus seven channel FIRs and seven FM demodulators may exceed the M4's real-time budget if implemented naively.
- Existing baseband images are generally single-purpose and single-channel; there is no drop-in seven-channel NFM monitor.
- The baseband message queue should carry decoded events, not per-sample or per-bit traffic.
- Per-channel spectrum UI should be avoided initially because spectrum capture is extra work.

Mitigations:

- Share the first decimation stage.
- Decode SAME only for channels above channel-power/RSSI threshold with hysteresis and hang time.
- Keep the SAME decoder integer/fixed-point where practical.
- Prefer Goertzel or quadrature tone-energy detectors at 1562.5 and 2083.3 Hz over heavier audio DSP.
- Disable CTCSS and spectrum capture in the NOAA processor.
- Start with compile-time seven-channel support but include a runtime diagnostic mode that can limit active channels to 1, 3, or 7 for profiling.
- Measure baseband deadline margin on hardware before enabling 7-channel decode by default.

## v0.6 implementation

The v0.6 milestone introduces:

- `firmware/baseband/proc_noaa_same.cpp` and `.hpp`: a new baseband processor tagged `PNOS`;
- `firmware/application/external/noaa_same/`: a new external RX app named `NOAA SAME`;
- `NoaaSameConfigureMessage`: application-to-baseband decoder slot assignment as up to three NOAA frequencies in Hz, audio enable, audio slot, and immediate override flag;
- `NoaaSameStatusMessage`: baseband-to-application seven-channel power, relative power, qualifier state, decoder slot status, update counters, and diagnostic counters;
- `/NOAA/PRESETS.TXT`: simple SD-card preset persistence for three named presets, `P1` through `P3`.

The baseband is centered on 162.475 MHz. The seven monitored channels are fixed offsets from that center:

- -75 kHz: 162.400 MHz;
- -50 kHz: 162.425 MHz;
- -25 kHz: 162.450 MHz;
- 0 kHz: 162.475 MHz;
- +25 kHz: 162.500 MHz;
- +50 kHz: 162.525 MHz;
- +75 kHz: 162.550 MHz.

The DSP chain shares the first two decimation stages, then runs seven per-channel translators and channel filters for measurement. FM demodulation is limited to the three configured decoder slots. Decoder assignment crosses the app/baseband boundary as frequencies, which lets a future GPS-assisted selector submit a top-three frequency list without depending on UI channel indexes. The baseband maps supported NOAA frequencies to its fixed channelizers. Each slot has independent demodulator state and is reset only when that slot's assigned channel changes. A SAME state-machine hook is present at each decoder slot, but the actual SAME decoder is deferred.

The application supports three assignment paths:

- Manual: each of three decoder slot fields can be changed independently.
- Presets: `P1`, `P2`, and `P3` load/save up to three NOAA channel indexes on the SD card, then submit the corresponding frequencies to the shared decoder-assignment interface.
- Recommendation: the UI collects approximately two seconds of status updates, ranks channels by measured power above the observed floor, and displays a pending top-three recommendation. The user must press `Apply`; no continuous automatic reassignment is performed.

Audio policy in v0.6:

- weather audio is off on startup;
- audible alerts are not implemented;
- channel reassignment never enables audio;
- the `Audio` checkbox is the only path that starts audio output;
- disabling audio stops audio output and keeps decoder assignments intact.

The UI exposes the minimum hardware-test diagnostics: seven channel rows, three decoder slot states, active decoder count, a CPU/utilization display derived from the existing M4 performance counter, and a deadline-miss field reserved for measured baseband deadline accounting.

## Current validation state

Local configure checks performed:

- Root CMake configure for `-DBOARD=PRALINE` was attempted. The external app tier updater ran and regenerated/validated the tier linker scripts.
- The configure could not proceed to compilation because `arm-none-eabi-gcc` and `arm-none-eabi-g++` are not installed in this environment.
- A direct `firmware` subdirectory CMake attempt also failed before project compilation because the host AppleClang/macOS SDK installation cannot link a trivial test program.

Not yet validated:

- H4M firmware compilation with the official ARM embedded toolchain.
- Baseband real-time behavior for one, two, and three active FM demodulators.
- Whether seven measurement channels plus three demodulators meet the M4 deadline with margin.
- Actual RF sensitivity, channel qualification thresholds, and noise-floor ranking behavior.
- Audio output behavior on physical H4M hardware.
- SD-card preset read/write behavior on the target filesystem.

Do not treat v0.6 as real-time validated until tested on physical H4M hardware.

## Future implementation plan

### Phase 1: New receive-only baseband image

Add a new baseband image, tentatively `proc_noaa_same.cpp`, rather than modifying `proc_nfm_audio.cpp`.

The processor should:

- use the existing baseband thread and shared-memory message framework;
- tune/configure through a dedicated `NOAASameConfigureMessage`;
- perform shared front-end decimation;
- run seven channel states at fixed offsets;
- compute per-channel power every 100 ms or similar;
- feed SAME decoders only while the channel is qualified;
- emit decoded header/EOM messages to the app side.

Keep live audio optional and single-channel. The baseband can demodulate all channels for SAME, but only copy one selected channel into `AudioOutput`.

### Phase 2: SAME decoder library

Create a small protocol component usable from the baseband processor:

- mark/space detector for 1562.5/2083.3 Hz;
- bit clock recovery for 520.83 bps;
- preamble acquisition on repeated `0xAB`;
- eight-bit byte packing without start/stop/parity;
- header grammar parser for `ZCZC-ORG-EEE-PSSCCC+TTTT-JJJHHMM-LLLLLLLL-`;
- EOM parser for `NNNN`;
- confidence and duplicate suppression across the three repeated header bursts.

The parser should preserve raw text even when parsing fails, so the application can log malformed or partial captures for diagnostics.

### Phase 3: Application UI and storage

Add a standalone receive app, likely under `firmware/application/external/noaa_same/`.

Recommended UI elements:

- seven-channel status list with frequency, power/RSSI, carrier state, and last SAME state;
- selected monitor channel;
- explicit toggles for live audio and audible alerts, both default off;
- FIPS/geographic filter editor;
- alert history view;
- SD logging status/error indicator.

Application-side logging should happen after decoded messages arrive from baseband. Do not write SD-card files from the M4 real-time path.

### Phase 4: Feasibility validation

Before broad UI polish, validate on H4M hardware:

- baseband processor deadline margin with 1, 3, and 7 active channels;
- SAME decode with recorded or generated SAME headers injected over RF or replayed through a test harness;
- weak-signal behavior and duplicate suppression;
- SD logging under repeated alert traffic;
- silent defaults after fresh settings and after app restart;
- no regressions to existing NFM/audio apps.

## Components suitable for reuse

- `ReceiverModel` radio control and persisted receiver settings.
- NFM FIR tap definitions and decimator/demodulator classes.
- `AudioOutput` for optional selected-channel weather audio.
- `RSSIThread`, `ChannelStatsCollector`, and UI RSSI widgets for activity display.
- `LogFile`, `File`, `ensure_directory`, and `rtc_time` for SD history.
- `RecentEntries` patterns from apps such as ADS-B/BLE for alert history.
- SAME TX constants and basic message formatting concepts from `same_tx`, with receiver-specific corrections for exact symbol timing and no start/stop framing.
- `baseband::run_image`, message definitions, and shared-memory queues for M0/M4 integration.

## Feasibility conclusion

Standalone NOAA Weather Radio plus SAME decoding is feasible on H4M, and all seven primary NOAA channels can be observed from one 162.475 MHz tuning because they occupy only 150 kHz of spectrum.

The feasibility concern is not RF coverage; it is M4 real-time DSP load from seven parallel NFM demodulators plus seven SAME decoders. The recommended design is a purpose-built shared-channelizer baseband image with lightweight per-channel SAME detection, RSSI qualification, and a single optional audio output. A naive implementation using seven full existing NFM chains is not recommended.

The first implementation milestone should be a silent, logging-only monitor that proves seven-channel decoding and SD history retention. Audible alerting and weather audio should remain opt-in features layered on top of decoded/logged events.

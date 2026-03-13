# AmpStageStudio

Circuit-inspired guitar amp modeling sandbox with swappable preamp, power tube, and pedal stages.

`AmpStageStudio` is an experimental guitar amp simulator built around explicit DSP building blocks instead of neural captures. The project is aimed at quickly auditioning amp families, mixing preamp and power-section combinations, comparing against NAM reference renders, and tweaking voicing live from a browser UI.

## What it does

- models named preamp sections such as Fender, Marshall, Vox, and Mesa/Boogie-style voices
- lets you pair those preamps with selectable power-tube families: `6V6`, `6L6`, `EL34`, `EL84`
- exposes amp-aware tone controls such as `bass`, `mid`, `treble`, and `presence` where the selected preamp supports them
- places front-end effects like `klon` and `tubescreamer` before the amp
- supports offline A/B rendering from WAV files
- supports live audio input with PortAudio
- supports live browser control through a lightweight local web UI

## Project status

Current project shape:
- preamp voicing is file-driven through `preamps/*.preamp`
- amp presets are convenience bundles through `amps/*.amp`
- power stage is selectable and independently overridable
- effect, preamp, and power controls can be adjusted live
- the web UI adapts control labels and tone-stack availability to the selected preamp
- NAM offline rendering is available for reference comparisons when `NeuralAmpModelerCore` is checked out locally

Current focus:
- prototype amp-specific preamp sections
- experiment with pedal/effect stages before the tube stage
- add selectable power-tube output stages (`6L6`, `EL34`, `EL84`, `6V6`)
- audition it offline with generated WAV files
- run the stage chain live from an audio interface when PortAudio is available
- build toward modular amp sections like preamp, tone stack, and power amp

## Quick start

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

Offline render:

```bash
./build/tube_stage_test \
  --input-wav "Dry Guitar.wav" \
  --preamp fender_deluxe_reverb \
  --power-tube 6V6 \
  --output-prefix dry_guitar_deluxe
```

Live run:

```bash
./build/amp_stage_live \
  --device "Scarlett" \
  --preamp fender_deluxe_reverb \
  --power-tube 6V6 \
  --control-file web/live_state.cfg
```

Web UI:

```bash
python3 web/server.py --control-file web/live_state.cfg
```

## Current files

```text
.
├── CMakeLists.txt
├── amp_profile.h
├── amps/
│   ├── fender_champ.amp
│   ├── fender_bassman_5f6a.amp
│   ├── fender_princeton.amp
│   ├── fender_deluxe_reverb.amp
│   ├── fender_twin_reverb_ab763.amp
│   ├── marshall_jcm800.amp
│   ├── marshall_jtm45.amp
│   ├── marshall_plexi_1959.amp
│   ├── mesa_boogie_mark_iic_plus.amp
│   ├── mesa_dual_rectifier.amp
│   ├── peavey_5150.amp
│   ├── soldano_slo_100.amp
│   └── vox_ac30_top_boost.amp
├── preamp.h
├── preamp_profile.h
├── preamps/
│   ├── fender_champ.preamp
│   ├── fender_bassman_5f6a.preamp
│   ├── fender_princeton.preamp
│   ├── fender_deluxe_reverb.preamp
│   ├── fender_twin_reverb_ab763.preamp
│   ├── marshall_jcm800.preamp
│   ├── marshall_jtm45.preamp
│   ├── marshall_plexi_1959.preamp
│   ├── mesa_boogie_mark_iic_plus.preamp
│   ├── mesa_dual_rectifier.preamp
│   ├── peavey_5150.preamp
│   ├── soldano_slo_100.preamp
│   └── vox_ac30_top_boost.preamp
├── power_stage.h
├── effects/
│   ├── klon_effect.h
│   └── tubescreamer_effect.h
├── tube_stage.h
└── tools/
    ├── amp_stage_live.cpp
    └── tube_stage_test.cpp
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

On Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

If PortAudio is installed, the build will also produce:

- `amp_stage_live`

If `NeuralAmpModelerCore` is checked out next to `AmpStageStudio` and `nlohmann/json`
is available, the build will also produce:

- `nam_offline_render`

## Offline audition

Generate a low-E sine test note and process it through the stage:

```bash
./build/tube_stage_test \
  --output-prefix low_e \
  --amp marshall_jtm45 \
  --frequency-hz 82.41 \
  --duration 2.0
```

That writes:

- `low_e_input.wav`
- `low_e_output.wav`

You can A/B them in any audio player or DAW.

You can also process a real guitar recording:

```bash
./build/tube_stage_test \
  --input-wav "Dry Guitar.wav" \
  --output-prefix dry_guitar \
  --preamp fender_deluxe_reverb \
  --power-tube 6V6
```

Add a Klon-inspired front-end effect before the tube stage:

```bash
./build/tube_stage_test \
  --input-wav "Dry Guitar.wav" \
  --output-prefix dry_guitar_klon_marshall \
  --effect klon \
  --effect-drive 0.55 \
  --effect-tone 0.6 \
  --effect-clean-blend 0.45 \
  --preamp marshall_jtm45 \
  --power-tube EL34
```

Or try a Tube Screamer-inspired front-end:

```bash
./build/tube_stage_test \
  --input-wav "Dry Guitar.wav" \
  --output-prefix dry_guitar_ts_fender \
  --effect tubescreamer \
  --effect-drive 0.6 \
  --effect-tone 0.55 \
  --effect-level-db -2 \
  --preamp fender_deluxe_reverb \
  --power-tube 6V6
```

You can also load a specific file directly:

```bash
./build/tube_stage_test \
  --input-wav "Dry Guitar.wav" \
  --amp-file amps/marshall_jcm800.amp \
  --output-prefix dry_guitar_jcm800
```

Current amp profiles:
- `fender_champ`
- `fender_bassman_5f6a`
- `fender_princeton`
- `fender_deluxe_reverb`
- `fender_twin_reverb_ab763`
- `marshall_jtm45`
- `marshall_jcm800`
- `marshall_plexi_1959`
- `mesa_boogie_mark_iic_plus`
- `mesa_dual_rectifier`
- `peavey_5150`
- `soldano_slo_100`
- `vox_ac30_top_boost`

Current preamp profiles:
- `fender_champ`
- `fender_bassman_5f6a`
- `fender_princeton`
- `fender_deluxe_reverb`
- `fender_twin_reverb_ab763`
- `marshall_jtm45`
- `marshall_jcm800`
- `marshall_plexi_1959`
- `mesa_boogie_mark_iic_plus`
- `mesa_dual_rectifier`
- `peavey_5150`
- `soldano_slo_100`
- `vox_ac30_top_boost`

Power-stage families:
- `6V6`
- `6L6`
- `EL34`
- `EL84`

If the file is stereo, choose the channel with `--input-channel 0` or `--input-channel 1`.

Currently supported input formats:
- 16-bit PCM WAV
- 32-bit float WAV

## NAM reference render

You can render the same dry file through a `.nam` model to create a comparison target:

```bash
./build/nam_offline_render \
  --model /path/to/model.nam \
  --input-wav "Dry Guitar.wav" \
  --output-wav dry_guitar_nam_output.wav
```

That gives you a reference file you can compare against `tube_stage_test` output while
tuning the `amps/*.amp` profile values.

## Live input

If PortAudio is available, you can run the stage chain live from an input device.

On Raspberry Pi with Pisound:

```bash
./build/amp_stage_live \
  --alsa-device plughw:2,0 \
  --effect tubescreamer \
  --preamp fender_deluxe_reverb \
  --power-tube 6V6
```

Use `--list-devices` to print PortAudio devices when not using explicit ALSA.

On macOS or another computer, you can either use one duplex device:

```bash
./build/amp_stage_live \
  --device "Scarlett" \
  --effect klon \
  --preamp marshall_jtm45 \
  --power-tube EL34
```

or split input and output devices:

```bash
./build/amp_stage_live \
  --input-device "MacBook Pro Microphone" \
  --output-device "MacBook Pro Speakers" \
  --effect none \
  --preamp fender_princeton \
  --power-tube 6V6
```

### Web control

`amp_stage_live` can poll a live control file and update the amp and effect settings
while audio is running. The included Python server edits that file from a browser UI.

Start the audio engine with a control file:

```bash
./build/amp_stage_live \
  --alsa-device plughw:2,0 \
  --amp fender_deluxe_reverb \
  --control-file web/live_state.cfg
```

Then start the UI server in a second terminal:

```bash
python3 web/server.py --control-file web/live_state.cfg
```

Open:

```text
http://localhost:8080
```

From the browser you can change:
- amp preset
- preamp profile
- power tube
- effect type
- preamp drive/level/bright/bias
- tone-stack controls such as bass/mid/treble/presence when that preamp exposes them
- power-stage drive/level/bias
- effect drive/tone/level/clean blend

## Notes

- The `amps/` files are convenience presets that combine a named preamp with a default power tube.
- The `preamps/` files are the current source of truth for stage voicing.
- `--preamp` plus `--power-tube` is the most direct way to audition custom combinations.
- `--preset marshall|fender` still works as a fallback when you do not specify a preamp or amp preset.
- The tone controls are currently circuit-inspired approximations mapped per preamp family, not exact passive tone-stack circuit solvers yet.
- `klon` is currently a Klon-inspired approximation, not an exact op-amp circuit solve.
- `tubescreamer` is currently a Tube Screamer-inspired approximation, not an exact circuit solve.
- This project is intentionally separate from `raspi-NAM`, which is focused on running
  NeuralAmpModeler models on Raspberry Pi hardware.

# AmpStageStudio

`AmpStageStudio` is a separate experiment repo for circuit-inspired guitar amp
section modeling without neural networks.

Current focus:
- prototype a single tube-style gain stage
- audition it offline with generated WAV files
- build toward modular amp sections like preamp, tone stack, and power amp

## Current files

```text
.
├── CMakeLists.txt
├── tube_stage.h
└── tools/
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

## Offline audition

Generate a low-E sine test note and process it through the stage:

```bash
./build/tube_stage_test \
  --output-prefix low_e \
  --preset marshall \
  --frequency-hz 82.41 \
  --duration 2.0
```

That writes:

- `low_e_input.wav`
- `low_e_output.wav`

You can A/B them in any audio player or DAW.

## Notes

- `marshall` and `fender` are currently voicing presets, not exact tube-circuit models.
- This project is intentionally separate from `raspi-NAM`, which is focused on running
  NeuralAmpModeler models on Raspberry Pi hardware.

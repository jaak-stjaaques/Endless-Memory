# Build and test Endless Memory

The FM‑1 package for version 0.5.1 is `build/endless-memory-0.5.1.fwsc`. The device's
installer identifies Endless Memory as `FM-1_965`.

## Build on a computer

Use Python 3 with Pillow, the JieLi toolchain, and the AC79 SDK. The toolchain
runs on x86‑64 Linux. On macOS, the build uses a Linux Docker container.

```sh
python3 -m pip install Pillow
sh tools/get_toolchain.sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
sh build.sh --release 0.5.1
```

Use `JIELI_TOOLCHAIN` and `AC79_SDK` to point at copies you already have. Without
`--release` the build is a development build, `build/endless-memory.fwsc`.

## Options

- `DINGHY_DEFS="-DEM_LOGO=n"`: the boot logo, 0 Rise, 1 Clock, 2 Fade, 3 Freeze, 4 Spiral, 5 Grid, 6 Echo, 7 Loop, 8 Word (default), 9 Monogram.
- `DINGHY_DEFS="-DEM_THEME=n"`: the screen colours, 0 Mono (default), 1 Paper, 2 Grid.

## Generated files

- `python3 tools/gen_tables.py > firmware/src/em_tables.h`: the sine, envelope, LFO and filter tables.
- `python3 tools/gen_font.py`: renders IBM Plex Mono into `firmware/gen/felucca_font.h` (needs `pip install freetype-py`).

## Test on the computer

`sh tests/run_host.sh tests/scripts/tour.txt` runs the instrument on the computer
and writes screen images and a WAV to `build/host/`. Other scripts in
`tests/scripts/` check the effects at full (`fxmax.txt`), the filters
(`filters.txt`), a loud low chord (`lowend.txt`) and the manual's screenshots
(`manual.txt`). `sh tests/run_tests.sh` runs the offline test suite after the
FM‑1 build. See the [Dinghy upstream documentation](https://github.com/zvenson/dinghy)
for the platform, and [API](API.md) and [hardware rules](RULES.md) for the
inherited low-level interfaces.

## First hardware install and recovery

The offline suite exercises the synth, the updater and the package checks. An
actual USB install and power-cycle test on a physical FM‑1 remain to be verified
for Endless Memory 0.5.1. Dinghy's USB rescue path is included.

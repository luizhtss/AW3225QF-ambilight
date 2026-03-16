# Alienware Monitor Ambilight

Screen-reactive LED control for the Alienware AW3225QF monitor via direct HID commands (V6 protocol).

Captures screen colors using DXGI desktop duplication and maps them to the monitor's front LEDs in real time — no AlienFX SDK required.

## LED Mapping

| Mask | LED |
|------|-----|
| `0x01` | Alien head logo |
| `0x02` | "32" inch number |
| `0x03` | Both front LEDs |
| `0xFF` | All LEDs |

## Building

Requires Visual Studio 2022 and CMake 3.15+. Also requires [alienfx-tools](https://github.com/T-Troll/alienfx-tools) cloned alongside this repo (for `DXGIManager`).

```
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

## Usage

AWCC / AlienFXSubAgent must be stopped for direct HID access. Use `--kill-awcc` to do it automatically.

```
# Run ambilight
ambilight.exe --kill-awcc

# Restore AWCC when done
ambilight.exe --restore-awcc
```

### Options

```
--fps N        Update rate (default: 30)
--step N       Pixel sample step (default: 8)
--smooth F     Smoothing 0.0-0.9 (default: 0.3)
--monitor N    Monitor to capture (default: 0)
--kill-awcc    Stop AWCC for direct HID access
--restore-awcc Restart AWCC and exit
--probe        Test each LED mask
```

## Tools

- **led_demo** — Interactive LED color test with named colors and demo sequence.
- **hid_debug** — HID device scanner and V6 protocol tester.

# X1000_display

X-Plane 12 plugin that mirrors the G1000 PFD and MFD displays to iPads over Wi-Fi/Ethernet, and routes physical bezel button and knob inputs back to X-Plane commands.

Designed for use with **Simionic SHB1000S** bezels and two iPads as screens, replacing the need for expensive physical G1000 hardware.

![Status](https://img.shields.io/badge/status-active-brightgreen)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-blue)
![X-Plane](https://img.shields.io/badge/X--Plane-12-orange)

---

## Features

- **Live G1000 display mirroring** — PFD and MFD captured via OpenGL draw callbacks, JPEG-encoded and streamed to Safari on iPads at ~15fps with ~30–50ms latency
- **Minimal FPS impact** — async GPU readback (PBO) + background encode thread; ~4fps hit at LFPG
- **Bezel input** — full UKP protocol map for SHB1000S PFD and MFD bezels (serial input via CP2102 USB-UART adapter)
- **Backlight control** — AP and audio mode annunciator states sent to bezel LEDs
- **Auto relay launch** — bundled Python relay starts and stops with the plugin; no manual setup
- **In-sim UI** — floating settings window (Plugins → X1000 Display → Settings) with live status, IP auto-detection, setup guide
- **Persistent settings** — saved to `X1000_display.ini` next to the plugin
- **Cross-platform** — Linux, Windows (MinGW cross-compile), macOS (universal binary)

---

## Architecture

```
X-Plane G1000 draw callback
  → glReadPixels (PBO async)
  → JPEG encode (worker thread)
  → UDP :9000/:9001
  → x1000_relay.py (WebSocket server)
  → Safari on iPad (Add to Home Screen)

SHB1000S bezel (CP2102 USB-UART)
  → Plugin UDP :15683
  → XPLMCommandOnce (g1000n1_* / g1000n2_*)

Plugin → bezel :15684
  → Backlight LED states (BL_AP, BL_HDG, ...)
```

---

## Repository Structure

```
X1000_display/
├── src/                    C++ plugin source
│   ├── Plugin.cpp          Entry points, flight loop, auto-retry
│   ├── Platform.h/.cpp     Cross-platform: GL loading, sockets, process spawn, timing
│   ├── DisplayStreamer.h/.cpp  G1000 capture, PBO, JPEG, UDP push
│   ├── SettingsManager.h/.cpp  INI persistence, IP detection
│   ├── RelayManager.h/.cpp     Relay process spawn and monitoring
│   ├── UIManager.h/.cpp        In-sim floating settings window
│   ├── ConnectionManager.h/.cpp  Bezel UDP input, backlight output
│   ├── UKPHandler.h/.cpp       UKP → X-Plane command map
│   ├── BacklightManager.h/.cpp  AP/audio datarefs → LED states
│   ├── UDPSocket.h/.cpp        Cross-platform non-blocking UDP
│   └── stb_image_write.h   Single-header JPEG encoder
├── tools/
│   └── x1000_relay.py      Python WebSocket relay (stdlib only, no dependencies)
├── compile.sh              Linux build + install
├── compile_win.sh          Windows cross-compile (MinGW-w64) + install
├── compile_mac.sh          macOS build (run on Mac) + install
├── exports.sym             Linux/Windows symbol visibility
├── exports_mac.sym         macOS symbol visibility
└── .gitignore
```

> **Not included:** `SDK/` (download from [developer.x-plane.com](https://developer.x-plane.com)) and `build/` (generated).

---

## Hardware

| Component | Model | Notes |
|---|---|---|
| PFD bezel | Simionic SHB1000S | Older model, no USB-C serial port |
| MFD bezel | Simionic SHB1000S | |
| Serial adapter | CP2102 USB-UART | Taps MSP430 BSL-TX/RX pins on PCB |
| PFD display | iPad (any) | Safari + Add to Home Screen |
| MFD display | iPad (any) | Safari + Add to Home Screen |
| Sim PC | Ubuntu 24.04 | X-Plane 12 |

---

## Requirements

- X-Plane 12 (SDK 4.1+)
- X-Plane SDK — download from [developer.x-plane.com](https://developer.x-plane.com), place in `SDK/`
- Python 3.8+ (for the relay — standard library only, no pip installs needed)
- Two iPads with Safari

### Build requirements

| Platform | Compiler |
|---|---|
| Linux | `g++` (Ubuntu 24.04+) |
| Windows | `x86_64-w64-mingw32-g++` (`sudo apt install mingw-w64`) |
| macOS | `clang++` (`xcode-select --install`) — must run on a Mac |

---

## Building and Installing

### Linux
```bash
./compile.sh install
```

### Windows (cross-compile from Linux)
```bash
sudo apt install mingw-w64
./compile_win.sh install
```

### macOS (run on a Mac)
```bash
xcode-select --install
./compile_mac.sh install
```

All three install to:
```
<X-Plane 12>/Resources/plugins/X1000_display/
├── lin_x64/X1000_display.xpl
├── win_x64/X1000_display.xpl
├── mac_x64/X1000_display.xpl
└── tools/x1000_relay.py
```

X-Plane automatically loads the correct binary for the running OS.

---

## iPad Setup

1. Start X-Plane and load a G1000 aircraft (default Cessna 172 SP)
2. Pop out the G1000 PFD and MFD windows (right-click → Pop Out)
3. The relay starts automatically — check **Plugins → X1000 Display → Settings**
4. On PFD iPad: open Safari → `http://<PC_IP>:8080/`
5. On MFD iPad: open Safari → `http://<PC_IP>:8081/`
6. Tap **Share → Add to Home Screen** on each iPad
7. Launch from the Home Screen icon (hides Safari chrome)
8. Set **Settings → Display → Auto-Lock → Never** on each iPad

The PC IP is auto-detected and shown in the plugin's settings window.

---

## Bezel Input (SHB1000S)

> ⚠️ **Work in progress** — CP2102 USB-UART adapters required (tap MSP430 BSL-TX/RX pins).

The UKP protocol map is fully implemented for PFD (`g1000n1_*`) and MFD (`g1000n2_*`) commands. Once the serial adapters arrive and the baud rate/protocol is confirmed, this section will be updated.

**Protocol:** `ServerAv|UKP=N` (even N = press/CW, odd N = release/CCW) over UDP to plugin port 15683.

---

## Performance

Tested on Ubuntu 24.04, X-Plane 12, default Cessna 172 G1000:

| Airport | Without plugin | With plugin | Delta |
|---|---|---|---|
| EKVG (light) | 66 fps | 58 fps | −8 fps |
| LFPG (heavy) | 30 fps | 26 fps | −4 fps |

Stream: 1024×768, JPEG quality 85, 15 fps → ~145 KB/frame, ~2 MB/s per display.

---

## Related Project

**X1000Viewer** — companion project (in the same repository) for [...].

---

## License

MIT License — see `LICENSE` file.

---

## Acknowledgements

- [stb_image_write](https://github.com/nothings/stb) — single-header JPEG encoder by Sean Barrett
- [X-Plane SDK](https://developer.x-plane.com) — Laminar Research
- Simionic — SHB1000S bezel hardware and UKP protocol documentation

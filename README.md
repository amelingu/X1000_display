# X1000_display

X-Plane 12 plugin that mirrors the G1000 PFD and MFD displays to two iPads over a local network, and routes Simionic SHB1000S bezel button and knob inputs back to X-Plane.

Designed for use with **Simionic SHB1000S** bezels and two iPads as screens, replacing the need for expensive physical G1000 hardware.

![Status](https://img.shields.io/badge/status-active-brightgreen)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-blue)
![X-Plane](https://img.shields.io/badge/X--Plane-12-orange)

---

## Features

- **Live G1000 display mirroring** — PFD and MFD captured via OpenGL draw callbacks, JPEG-encoded and streamed to Safari on iPads at ~15fps with ~30–50ms latency
- **Minimal FPS impact** — async GPU readback (PBO) + background encode thread; ~4fps hit at LFPG
- **Bezel input** — full UKP protocol command map for SHB1000S PFD and MFD bezels (see Status below)
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
  → tools/x1000_relay.py (WebSocket server)   ← managed by plugin
  → Safari on iPad  OR  X1000Viewer iOS app

tools/x1000_bezel.py (standalone Python script, bleak BLE)
  → connects to SHB1000S bezel(s) via Bluetooth LE
  → receives UKP byte notifications from characteristic
  → sends UKP packets → Plugin UDP :15683
  → ConnectionManager → XPLMCommandOnce (g1000n1_* / g1000n2_*)

Plugin → bezel :15684
  → Backlight LED states (BL_AP, BL_HDG, ...)
```

**Key design decision:** bezel serial input is handled by a separate Python process (`x1000_bezel.py`) rather than inside the plugin. This provides crash isolation — a serial port error cannot destabilise X-Plane. The plugin's `ConnectionManager` already listens on UDP :15683 and dispatches UKP commands; no C++ changes are needed when the bezel script is implemented.

---

## Repository Structure

```
X1000_display/
├── src/                          C++ plugin source
│   ├── Plugin.cpp                Entry points, flight loop, auto-retry
│   ├── Platform.h/.cpp           Cross-platform: GL, sockets, process spawn, timing
│   ├── DisplayStreamer.h/.cpp    G1000 capture, PBO, JPEG, UDP push
│   ├── SettingsManager.h/.cpp    INI persistence, IP detection
│   ├── RelayManager.h/.cpp       Relay process spawn and monitoring
│   ├── UIManager.h/.cpp          In-sim floating settings window
│   ├── ConnectionManager.h/.cpp  Bezel UDP input, backlight output
│   ├── UKPHandler.h/.cpp         UKP → X-Plane command map (PFD + MFD)
│   ├── BacklightManager.h/.cpp   AP/audio datarefs → LED states
│   ├── UDPSocket.h/.cpp          Cross-platform non-blocking UDP
│   └── stb_image_write.h         Single-header JPEG encoder
├── tools/
│   ├── x1000_relay.py            Python WebSocket relay (stdlib only)
│   └── x1000_bezel.py            TODO: serial → UKP → plugin (pyserial)
├── X1000Viewer/                  Native iOS app (Swift)
│   ├── X1000Viewer.swift         Receives relay stream, true full screen
│   └── Xcode.txt                 Xcode project setup notes
├── docs/
│   └── bezel SHB1000S.txt        SHB1000S hardware reference notes
├── SDK/                          ← download and place here (not in repo)
│   ├── CHeaders/
│   │   ├── XPLM/                 Core SDK headers
│   │   └── Widgets/              Widget SDK headers
│   └── Libraries/
│       ├── Lin/                  Linux .so stubs
│       ├── Win/                  Windows .lib stubs
│       └── Mac/                  macOS .framework stubs
├── compile.sh                    Linux build + install
├── compile_win.sh                Windows cross-compile (MinGW-w64) + install
├── compile_mac.sh                macOS build (run on Mac) + install
├── exports.sym                   Linux/Windows symbol visibility
├── exports_mac.sym               macOS symbol visibility
├── LICENSE                       MIT
└── .gitignore
```

> **`SDK/` is not included in the repository** — download it from [developer.x-plane.com](https://developer.x-plane.com) (free registration required) and place it at `SDK/` as shown above before building.

---

## Status

| Feature | Status |
|---|---|
| G1000 PFD/MFD display mirroring | ✅ Working |
| WebSocket relay → Safari | ✅ Working |
| In-sim settings UI | ✅ Working |
| Persistent settings | ✅ Working |
| Linux build | ✅ Working |
| Windows build | ✅ Compiles (untested at runtime) |
| macOS build | ⏳ Script ready, needs a Mac |
| Bezel UKP command map | ✅ Complete (PFD + MFD) |
| Bezel BLE input (x1000_bezel.py) | ✅ Working — awaiting BLE dongle for sim PC |
| Backlight LED output | ✅ Protocol complete, pending verification |
| X1000Viewer iOS app | 🔧 In progress |

---

## Hardware

| Component | Model | Notes |
|---|---|---|
| PFD bezel | Simionic SHB1000S | Older model, no USB-C serial port |
| MFD bezel | Simionic SHB1000S | |
| Serial adapter | CP2102 USB-UART | Taps MSP430 BSL-TX/RX pins on PCB |
| PFD display | iPad (any) | Safari or X1000Viewer |
| MFD display | iPad (any) | Safari or X1000Viewer |
| Sim PC | Ubuntu 24.04 | X-Plane 12 |

---

## Requirements

- X-Plane 12 (SDK 4.1+)
- X-Plane SDK — download from [developer.x-plane.com](https://developer.x-plane.com), place at `SDK/`
- Python 3.8+ with `bleak` (`pip install bleak --break-system-packages`)
- Two iPads with Safari (or X1000Viewer)

### Build requirements

| Platform | Toolchain |
|---|---|
| Linux | `g++` (Ubuntu 24.04+) |
| Windows | `x86_64-w64-mingw32-g++` — `sudo apt install mingw-w64` |
| macOS | `clang++` — `xcode-select --install` (must run on a Mac) |

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
For a better experience (hidden status bar, true full screen, screen always on) use the **X1000Viewer** iOS app instead of Safari.

---

## Bezel Input

The SHB1000S bezel communicates over **Bluetooth LE** — button and knob events are sent as single-byte UKP notifications on a vendor-specific GATT characteristic. No serial interface is required.

**BLE details:**
- Service UUID: `c8ad063d-cc77-4d98-997f-dc796450b209`
- Characteristic UUID: `f62a9f56-f29e-48a8-a317-47ee37a58999`
- Each notification = one or more UKP bytes
- Even byte = button press or knob CW turn
- Odd byte = button release or knob CCW turn

**`tools/x1000_bezel.py`** connects to the bezel(s) via BLE, subscribes to notifications, and forwards each UKP value as `ServerAv|UKP=N` UDP packets to the plugin on port 15683. The plugin's existing `ConnectionManager` and `UKPHandler` dispatch them to X-Plane commands with no C++ changes needed.

```bash
pip install bleak --break-system-packages

# Auto-scan and connect
python3 tools/x1000_bezel.py

# Explicit MAC addresses
python3 tools/x1000_bezel.py --pfd 00:07:80:A6:F5:0A --plugin-ip 127.0.0.1

# Two bezels
python3 tools/x1000_bezel.py --pfd XX:XX:XX:XX:XX:XX --mfd YY:YY:YY:YY:YY:YY
```

The script is kept as a separate process from the plugin for crash isolation. A USB Bluetooth 5.0 dongle is required on the sim PC (any CSR or Realtek based dongle works on Ubuntu — e.g. TP-Link UB500).

---

## X1000Viewer

Native iOS app (Swift) that receives the relay stream and displays the G1000 PFD or MFD in true full screen.

| Feature | Safari (web) | X1000Viewer (native) |
|---|---|---|
| Status bar | Always visible on iPadOS 16+ | Hidden completely |
| Full screen | Home Screen icon workaround | True full screen |
| Screen always on | Manual Auto-Lock setting | Automatic |
| Orientation | Manual | Forced landscape |
| Latency | ~30–50ms | ~30–50ms (no meaningful difference) |
| Reconnection | 1-second JS timeout | Instant |

---

## Performance

Tested on Ubuntu 24.04, X-Plane 12, default Cessna 172 G1000:

| Airport | Without plugin | With plugin | Delta |
|---|---|---|---|
| EKVG (light scenery) | 66 fps | 58 fps | −8 fps |
| LFPG (heavy scenery) | 30 fps | 26 fps | −4 fps |

Stream: 1024×768, JPEG quality 85, 15 fps → ~145 KB/frame, ~2 MB/s per display.

---

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgements

- [stb_image_write](https://github.com/nothings/stb) — single-header JPEG encoder by Sean Barrett
- [X-Plane SDK](https://developer.x-plane.com) — Laminar Research
- Simionic — SHB1000S bezel hardware and UKP protocol

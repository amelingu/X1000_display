# X1000_display

X-Plane 12 plugin that mirrors the G1000 PFD and MFD displays to two iPads over a local network, and routes Simionic SHB1000S bezel button and knob inputs back to X-Plane.

Designed for use with **Simionic SHB1000S** bezels and two iPads as screens, providing a full G1000 simulation experience without expensive physical hardware.

![Status](https://img.shields.io/badge/status-active-brightgreen)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-blue)
![X-Plane](https://img.shields.io/badge/X--Plane-12-orange)

---

## Features

### Display
- **Live G1000 PFD/MFD mirroring** — captured via OpenGL draw callbacks, JPEG-encoded and streamed to Safari on iPads at ~15fps with ~30–50ms latency
- **Brightness sync** — iPad image brightness tracks `sim/cockpit2/electrical/display_screen_brightness` in real time
- **Minimal FPS impact** — async GPU readback (PBO) + background encode thread; ~4fps hit at LFPG

### Bezel Input (SHB1000S via Bluetooth LE)
- **Full PFD button/knob map** — all G1000 PFD functions (`g1000n1_*`)
- **Full MFD button/knob map** — all G1000 MFD functions (`g1000n3_*`)
- **Audio panel** — COM1/2 monitor and MIC select, NAV1/2, ADF, DME, MKR/MUTE, HI SENS, SPKR, display backup switch
- **Fast spin detection** — heading/course knobs fire 10 commands per click when spinning fast, with direction noise filtering
- **Hold-repeat** — NOSE UP/DOWN and FMS cursor keys repeat while held (0.5s delay, then 5Hz)
- **Cursor acceleration** — FMS cursor keys accelerate from 5/sec to 20/sec the longer they are held
- **CLR on release** — CLR fires on button release, not press, to prevent accidental activations
- **MFD CLR long press** — forces MFD to full-screen NAV page (when display backup is off)
- **Display backup switch** — stable ON/OFF switch controlling `G1000_display_reversion`
- **Auto BLE reconnect** — bezels reconnect automatically between X-Plane sessions

### Plugin Infrastructure
- **Auto relay launch** — display relay and bezel BLE bridge start/stop with the plugin
- **In-sim UI** — floating settings window (Plugins → X1000 Display → Settings) with live status, bezel MAC configuration, setup guide
- **Persistent settings** — saved to `X1000_display.ini` next to the plugin
- **Auto-retry display init** — waits for G1000 avionics to bind after aircraft load
- **Cross-platform** — Linux, Windows (MinGW cross-compile), macOS (universal binary)

---

## Architecture

```
X-Plane G1000 draw callback
  → glReadPixels (PBO async)
  → JPEG encode × brightness (worker thread)
  → UDP :9000/:9001
  → tools/x1000_relay.py (WebSocket server)   ← auto-launched by plugin
  → Safari on iPad  OR  X1000Viewer iOS app

tools/x1000_bezel.py (bleak BLE, standalone process)   ← auto-launched by plugin
  → connects to SHB1000S bezel(s) via Bluetooth LE
  → receives UKP byte notifications
  → sends UKP packets → Plugin UDP :15683 (PFD) / :15685 (MFD)
  → ConnectionManager → UKPHandler → XPLMCommandOnce
  → AudioPanelManager → XPLMCommandOnce / XPLMSetDatai

Plugin → bezel :15684   (backlight protocol — currently ignored by SHB1000S firmware)
```

**Key design decisions:**
- Bezel input is a separate Python process (`x1000_bezel.py`) for crash isolation
- Display relay is pure Python stdlib — no pip dependencies
- PBO double-buffering eliminates GPU readback stall
- JPEG encode happens on a background thread — render thread cost is negligible

---

## Repository Structure

```
X1000_display/
├── src/                          C++ plugin source
│   ├── Plugin.cpp                Entry points, flight loop, auto-retry
│   ├── Platform.h/.cpp           Cross-platform: GL, sockets, process spawn, timing
│   ├── DisplayStreamer.h/.cpp    G1000 capture, PBO, JPEG, brightness, UDP push
│   ├── SettingsManager.h/.cpp    INI persistence, IP detection
│   ├── RelayManager.h/.cpp       Relay process spawn and monitoring
│   ├── UIManager.h/.cpp          In-sim floating settings window
│   ├── ConnectionManager.h/.cpp  Bezel UDP input, backlight output
│   ├── UKPHandler.h/.cpp         UKP → X-Plane command map (PFD + MFD + audio panel)
│   ├── AudioPanelManager.h/.cpp  Audio panel button → X-Plane commands/datarefs
│   ├── BacklightManager.h/.cpp   AP/audio datarefs → bezel LED states
│   ├── UDPSocket.h/.cpp          Cross-platform non-blocking UDP
│   └── stb_image_write.h         Single-header JPEG encoder
├── tools/
│   ├── x1000_relay.py            Python WebSocket relay (stdlib only, no pip needed)
│   └── x1000_bezel.py            BLE bezel bridge (requires: pip install bleak)
├── X1000Viewer/                  Native iOS app (Swift) — in progress
├── docs/
│   └── bezel SHB1000S.txt        SHB1000S hardware reference notes
├── SDK/                          ← download from developer.x-plane.com (not in repo)
│   ├── CHeaders/XPLM/
│   ├── CHeaders/Widgets/
│   └── Libraries/Lin|Win|Mac/
├── compile.sh                    Linux build + install
├── compile_win.sh                Windows cross-compile (MinGW-w64) + install
├── compile_mac.sh                macOS build (run on Mac) + install
├── exports.sym                   Linux/Windows symbol visibility
├── exports_mac.sym               macOS symbol visibility
├── LICENSE                       MIT
└── .gitignore
```

---

## Status

| Feature | Status |
|---|---|
| G1000 PFD/MFD display mirroring | ✅ Working |
| Brightness sync from X-Plane | ✅ Working |
| WebSocket relay → Safari | ✅ Working |
| In-sim settings UI | ✅ Working |
| Persistent settings | ✅ Working |
| Linux build | ✅ Working |
| Windows build | ✅ Compiles (untested at runtime) |
| macOS build | ⏳ Script ready, needs a Mac |
| PFD bezel input — all buttons/knobs | ✅ Working |
| MFD bezel input — all buttons/knobs | ✅ Working |
| Audio panel — COM/NAV/ADF/DME/MKR | ✅ Working |
| Fast spin knob filter + 10x batch | ✅ Working |
| Hold-repeat (NOSE UP/DOWN, cursor) | ✅ Working |
| Cursor key acceleration | ✅ Working |
| Display backup switch | ✅ Working |
| Bezel auto-reconnect | ✅ Working |
| Bezel/audio panel LED control | 🔧 Protocol unknown — needs Windows sniff |
| X1000Viewer iOS app | 🔧 In progress |

---

## Hardware

| Component | Model | Notes |
|---|---|---|
| PFD bezel | Simionic SHB1000S | Bluetooth LE, audio panel attached |
| MFD bezel | Simionic SHB1000S | Bluetooth LE |
| BLE adapter | Any USB BT5.0 dongle | e.g. TP-Link UB500 — built-in BT works too |
| PFD display | iPad (any) | Safari or X1000Viewer |
| MFD display | iPad (any) | Safari or X1000Viewer |
| Sim PC | Ubuntu 24.04 | X-Plane 12 |

**BLE MAC addresses** (configure in plugin settings or ini):
- PFD bezel: `00:07:80:A6:E1:71`
- MFD bezel: `00:07:80:A6:F5:0A`

---

## Requirements

- X-Plane 12 (SDK 4.1+)
- X-Plane SDK — download from [developer.x-plane.com](https://developer.x-plane.com), place at `SDK/`
- Python 3.8+
  - Display relay: no pip installs needed (stdlib only)
  - Bezel bridge: `pip install bleak --break-system-packages`

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
└── tools/
    ├── x1000_relay.py
    └── x1000_bezel.py
```

---

## iPad Setup

1. Start X-Plane and load a G1000 aircraft (default Cessna 172 SP)
2. Pop out the G1000 PFD and MFD windows (right-click → Pop Out)
3. The relay starts automatically — check **Plugins → X1000 Display → Settings**
4. On PFD iPad: open Safari → `http://<PC_IP>:8080/`
5. On MFD iPad: open Safari → `http://<PC_IP>:8081/`
6. Tap **Share → Add to Home Screen** on each iPad
7. Launch from the Home Screen icon
8. Set **Settings → Display → Auto-Lock → Never** on each iPad

The PC IP is auto-detected and shown in the plugin's settings window.
For a better experience (hidden status bar, true full screen) use the **X1000Viewer** iOS app.

---

## Bezel Setup

Configure MAC addresses in the plugin settings window or directly in the ini file:

```ini
[bezel]
pfd_bezel_mac=00:07:80:A6:E1:71
mfd_bezel_mac=00:07:80:A6:F5:0A
bezel_pfd_port=15683
bezel_mfd_port=15685
```

The bezel bridge starts automatically when the plugin loads. Both bezels reconnect automatically between X-Plane sessions.

Install the bleak dependency once:
```bash
pip install bleak --break-system-packages
```

---

## Bezel LED Control

The SHB1000S bezels have general adjustable backlights (no individual button LEDs on PFD/MFD bezels). The audio panel has individual button LEDs for each function.

The plugin sends `ClientAv|BL_*=` backlight packets via UDP, but the SHB1000S firmware currently ignores these — the bezel manages its own LED state internally. Reverse-engineering the LED control protocol requires sniffing the Simionic iOS app on Windows, which is planned for a future session.

---

## Audio Panel Button Mapping

| Button | X-Plane Command |
|---|---|
| COM1/MIC | `sim/audio_panel/transmit_audio_com1` |
| COM2/MIC | `sim/audio_panel/transmit_audio_com2` |
| COM1 monitor | `sim/audio_panel/monitor_audio_com1` |
| COM2 monitor | `sim/audio_panel/monitor_audio_com2` |
| NAV1 | `sim/audio_panel/monitor_audio_nav1` |
| NAV2 | `sim/audio_panel/monitor_audio_nav2` |
| ADF | `sim/audio_panel/monitor_audio_adf1` |
| DME | `sim/audio_panel/monitor_audio_dme` |
| MKR/MUTE | `sim/audio_panel/monitor_audio_mkr` |
| SPKR | `sim/audio_panel/toggle_speaker` |
| Display backup | `sim/GPS/G1000_display_reversion` + `G1000_reversionary_mode` dataref |
| COM1/2 toggle | `sim/GPS/g1000n1_com12` |

Buttons not implemented in default C172: COM3, TEL, PA, AUX, MAN SQ, PLAY, PILOT, COPLT (no X-Plane commands exist for these in the default aircraft).

---

## Performance

Tested on Ubuntu 24.04, X-Plane 12, default Cessna 172 G1000:

| Airport | Without plugin | With plugin | Delta |
|---|---|---|---|
| EKVG (light scenery) | 66 fps | 58 fps | −8 fps |
| LFPG (heavy scenery) | 30 fps | 26 fps | −4 fps |

Stream: 1024×768, JPEG quality 85, 15 fps → ~145 KB/frame, ~2 MB/s per display.

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

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgements

- [stb_image_write](https://github.com/nothings/stb) — single-header JPEG encoder by Sean Barrett
- [X-Plane SDK](https://developer.x-plane.com) — Laminar Research
- [bleak](https://github.com/hbldh/bleak) — Bluetooth LE Python library
- Simionic — SHB1000S bezel hardware

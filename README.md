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
- **Smart init** — plugin detects whether the loaded aircraft has G1000 avionics and whether the screens are popped out; no performance impact when flying non-G1000 aircraft

### Bezel Input (SHB1000S via Bluetooth LE)
- **Full PFD button/knob map** — all G1000 PFD functions (`g1000n1_*`)
- **Full MFD button/knob map** — all G1000 MFD functions (`g1000n3_*`)
- **Audio panel** — COM1/2 monitor and MIC select, NAV1/2, ADF, DME, MKR/MUTE, HI SENS, SPKR, display backup switch
- **Fast spin detection** — heading/course knobs fire 10 commands per click when spinning fast, with direction noise filtering
- **Hold-repeat** — NOSE UP/DOWN and FMS cursor keys repeat while held (0.5s delay, then 5Hz)
- **Cursor acceleration** — FMS cursor keys accelerate from 5/sec to 20/sec the longer they are held
- **COM flip-flop long press** — short press swaps active/standby; long press (≥500ms) sets emergency frequency 121.500MHz (via `XPLMCommandBegin`/`Continue`/`End`)
- **CLR short/long press** — short press clears entry; long press returns to NAV page (via `XPLMCommandBegin`/`Continue`/`End`)
- **Display backup switch** — stable ON/OFF switch controlling `G1000_display_reversion`
- **Auto BLE reconnect** — bezels reconnect automatically between X-Plane sessions

### Bezel LED Control
- **Backlight brightness** — PFD and MFD bezel backlights (and audio panel backlight) track X-Plane `instrument_brightness_ratio` in real time
- **Audio panel button LEDs** — COM1/2 monitor, COM1/2 MIC, NAV1/2, ADF, DME, MKR/MUTE, SPKR LEDs reflect actual X-Plane audio selection state
- **Protocol** — fully reverse-engineered via nRF52840 Wireshark capture (July–August 2026): each LED has an ON byte and an OFF byte (`OFF = ON + 0x17`); brightness scale is inverted (`0x00` = max bright, `0x40` = off)

### Plugin Infrastructure
- **Auto relay launch** — display relay and bezel BLE bridge start/stop with the plugin
- **In-sim UI** — floating settings window (Plugins → X1000 Display → Settings) with live status, bezel MAC configuration, BLE scan with auto PFD detection
- **Bezel scan** — click Scan BLE, press 3 buttons on PFD bezel → both bezels auto-assigned and connected without restart
- **Persistent settings** — saved to `X1000_display.ini` at plugin root (shared across platforms)
- **Cross-platform** — Linux ✅, Windows 10 ✅, macOS ⏳

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
  INPUT:  BLE notifications → UKP bytes → UDP :15683 (PFD) / :15685 (MFD)
          → ConnectionManager → UKPHandler → XPLMCommandBegin/Continue/End
          → AudioPanelManager → XPLMCommandOnce / XPLMSetDatai
  OUTPUT: UDP :15684 ← plugin binary LED state packet
          → BLE write (single byte per LED: ON byte or OFF byte)
```

**BLE LED protocol** (fully decoded via nRF52840 Wireshark capture, August 2026):

| Byte value | Effect |
|---|---|
| `0x00` | Reset — all LEDs off, max brightness (avoid — use `0x01` minimum) |
| `0x01–0x40` | Backlight brightness — **inverted**: `0x01`=max bright, `0x40`=off |
| ON byte (`0x43`–`0x59`) | Turn ON specific LED |
| OFF byte (`ON + 0x17`) | Turn OFF specific LED |

**Plugin → bezel script LED packet** (binary UDP on port 15684):
```
Byte 0:     brightness (inverted: 0x01=max bright, 0x40=off)
Bytes 1..N: pre-computed BLE bytes — ON byte or OFF byte for each tracked LED
```

**Key design decisions:**
- Bezel input/output handled by a separate Python process (`x1000_bezel.py`) for crash isolation
- Display relay uses Python stdlib only — no pip dependencies
- PBO double-buffering eliminates GPU readback stall
- JPEG encode happens on a background thread — render thread cost is negligible
- Single ini file at plugin root shared by all platform binaries
- Plugin detects non-G1000 aircraft instantly (no blocking loop) and stops retrying

---

## Repository Structure

```
X1000_display/
├── src/                          C++ plugin source
│   ├── Plugin.cpp                Entry points, flight loop, G1000 detection, auto-retry
│   ├── Platform.h/.cpp           Cross-platform: GL, sockets, process spawn, timing
│   ├── DisplayStreamer.h/.cpp    G1000 capture, PBO, JPEG, brightness, UDP push
│   ├── SettingsManager.h/.cpp    INI persistence, IP detection
│   ├── RelayManager.h/.cpp       Relay process spawn and monitoring
│   ├── UIManager.h/.cpp          In-sim floating settings window + BLE scan UI
│   ├── ConnectionManager.h/.cpp  Bezel UDP input, backlight output
│   ├── UKPHandler.h/.cpp         UKP → X-Plane command map (PFD + MFD + audio panel)
│   ├── AudioPanelManager.h/.cpp  Audio panel button → X-Plane commands/datarefs
│   ├── BacklightManager.h/.cpp   Audio/brightness datarefs → binary LED state → UDP
│   ├── UDPSocket.h/.cpp          Cross-platform non-blocking UDP
│   └── stb_image_write.h         Single-header JPEG encoder
├── tools/
│   ├── x1000_relay.py            Python WebSocket relay (stdlib only, no pip needed)
│   └── x1000_bezel.py            BLE bezel bridge — input + LED output (requires bleak)
├── docs/
│   ├── led_sandbox.py            Standalone LED control test script (requires bleak)
│   └── bezel SHB1000S.txt        SHB1000S hardware reference notes
├── X1000Viewer/                  Native iOS app (Swift) — in progress
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
| Persistent settings (shared ini) | ✅ Working |
| Bezel scan with auto PFD detection | ✅ Working |
| Linux build | ✅ Working |
| Windows 10 build and runtime | ✅ Working |
| macOS build | ⏳ Script ready, needs a Mac |
| PFD bezel input — all buttons/knobs | ✅ Working |
| MFD bezel input — all buttons/knobs | ✅ Working |
| Audio panel — COM/NAV/ADF/DME/MKR | ✅ Working |
| COM flip-flop short/long press | ✅ Working |
| CLR short/long press | ✅ Working |
| Fast spin knob filter + 10x batch | ✅ Working |
| Hold-repeat (NOSE UP/DOWN, cursor) | ✅ Working |
| Cursor key acceleration | ✅ Working |
| Display backup switch | ✅ Working |
| Bezel auto-reconnect between sessions | ✅ Working |
| Bezel/audio panel backlight control | ✅ Working |
| Audio panel button LED control | ✅ Working |
| Non-G1000 aircraft: no performance impact | ✅ Working |
| X1000Viewer iOS app | 🔧 In progress |

---

## Hardware

| Component | Model | Notes |
|---|---|---|
| PFD bezel | Simionic SHB1000S or later (SH1000M etc.) | Bluetooth LE, audio panel attached via EXT. PORT |
| MFD bezel | Simionic SHB1000S or later | Bluetooth LE |
| BLE adapter | Any USB BT5.0 dongle | e.g. TP-Link UB500 — built-in BT works too |
| PFD display | iPad (any) | Safari or X1000Viewer |
| MFD display | iPad (any) | Safari or X1000Viewer |
| Sim PC | Ubuntu 24.04 or Windows 10 | X-Plane 12 |

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
| Windows | `x86_64-w64-mingw32-g++` — `sudo apt install mingw-w64` (cross-compile from Linux) |
| macOS | `clang++` — `xcode-select --install` (must run on a Mac) |

---

## Building and Installing

### Prerequisites (all platforms)

**1. Clone the repository**
```bash
git clone https://github.com/amelingu/X1000_display.git
cd X1000_display
```

**2. Download the X-Plane SDK**
- Register (free) at [developer.x-plane.com](https://developer.x-plane.com)
- Download the SDK (version 4.1+)
- Extract so the structure is:
```
X1000_display/
└── SDK/
    ├── CHeaders/
    │   ├── XPLM/
    │   └── Widgets/
    └── Libraries/
        ├── Lin/
        ├── Win/
        └── Mac/
```

**3. Install Python dependencies** (for relay and bezel scripts)
```bash
pip install bleak --break-system-packages   # Linux/Mac
pip install bleak                            # Windows
```

---

### Linux (native build)

**Requirements:**
```bash
sudo apt install build-essential libgl-dev
```

**Build and install:**
```bash
chmod +x compile.sh
./compile.sh install
```

X-Plane 12 is auto-detected at `~/Bureau/X-Plane 12` or `~/X-Plane 12`.
Override with: `XP12="/path/to/X-Plane 12" ./compile.sh install`

---

### Windows (cross-compile from Linux)

**Requirements:**
```bash
sudo apt install mingw-w64
```

**Build:**
```bash
chmod +x compile_win.sh
./compile_win.sh
```

**Install on Windows PC:**

Copy the following to the Windows X-Plane installation:
```
build/win_x64/X1000_display.xpl  →  <XP12>/Resources/plugins/X1000_display/win_x64/
tools/x1000_relay.py              →  <XP12>/Resources/plugins/X1000_display/tools/
tools/x1000_bezel.py              →  <XP12>/Resources/plugins/X1000_display/tools/
```

**Python on Windows:**
- Download Python 3 from [python.org](https://python.org) — check **"Add to PATH"** during install
- Then: `pip install bleak`

**First run on Windows:**
- Start X-Plane, load G1000 aircraft, pop out PFD and MFD windows
- Open **Plugins → X1000 Display → Settings**
- Click **[Scan BLE]**, press 3 buttons on the PFD bezel when prompted
- Both bezels auto-assigned — plugin connects immediately

---

### macOS (must run on a Mac)

**Requirements:**
```bash
xcode-select --install
```

**Build and install:**
```bash
chmod +x compile_mac.sh
./compile_mac.sh install
```

X-Plane 12 is auto-detected at `~/X-Plane 12`.
Override with: `XP12="/path/to/X-Plane 12" ./compile_mac.sh install`

The script produces a **universal binary** (x86_64 + arm64) in `build/mac_fat/`.

> **Note:** Cross-compiling for macOS from Linux requires osxcross and an Apple SDK — significantly more complex. Running `compile_mac.sh` on a Mac is strongly recommended.

---

### Installed layout (all platforms)

After installation, the plugin folder contains:
```
<X-Plane 12>/Resources/plugins/X1000_display/
├── lin_x64/X1000_display.xpl     ← Linux
├── win_x64/X1000_display.xpl     ← Windows
├── mac_x64/X1000_display.xpl     ← macOS
├── tools/
│   ├── x1000_relay.py            ← auto-launched by plugin
│   └── x1000_bezel.py            ← auto-launched by plugin
└── X1000_display.ini             ← auto-created on first run, shared by all platforms
```

X-Plane automatically loads the correct binary for the running OS.
All three platform binaries coexist in the same folder and share the same ini file.

---

## iPad Setup

1. Start X-Plane and load a G1000 aircraft (default Cessna 172 SP)
2. Pop out the G1000 PFD and MFD windows (right-click → Pop Out)
3. The relay starts automatically — check **Plugins → X1000 Display → Settings** for the PC IP address
4. On PFD iPad: open Safari → `http://<PC_IP>:8080/`
5. On MFD iPad: open Safari → `http://<PC_IP>:8081/`
6. Tap **Share → Add to Home Screen** on each iPad
7. Launch from the Home Screen icon
8. Set **Settings → Display → Auto-Lock → Never** on each iPad

The PC IP is auto-detected and shown in the plugin's settings window.
For a better experience (hidden status bar, true full screen) use the **X1000Viewer** iOS app.

**To test the stream on the X-Plane PC itself** (no iPad needed):
- PFD: `http://127.0.0.1:8080/`
- MFD: `http://127.0.0.1:8081/`

---

## Bezel Setup

### First time (no ini file)

1. Open **Plugins → X1000 Display → Settings**
2. Click **[Scan BLE]** — the plugin scans for nearby bezels for 15 seconds
3. When prompted, press 3 buttons on the **PFD bezel**
4. Both bezels are automatically identified and assigned
5. The bezel bridge restarts automatically — no further action needed

### Manual configuration

Edit `X1000_display.ini` at the plugin root:

```ini
[bezel]
pfd_bezel_mac=00:07:80:A6:E1:71
mfd_bezel_mac=00:07:80:A6:F5:0A
bezel_pfd_port=15683
bezel_mfd_port=15685
```

Install bleak once:
```bash
pip install bleak --break-system-packages
```

### Running alongside the Simionic plugin

Running X1000_display and the Simionic plugin simultaneously is not supported — both plugins use UDP ports 15683/15685 for bezel input, causing a bind conflict. The Bluetooth adapter is also shared and may conflict.

If you need to run both plugins, change the bezel ports in `X1000_display.ini`:

```ini
[bezel]
bezel_pfd_port=15693
bezel_mfd_port=15695
```

Note: the Simionic plugin must be disabled or unloaded before starting X1000_display for BLE to work correctly.

---

## Bezel LED Control

### Protocol (reverse-engineered via nRF52840 Wireshark capture, August 2026)

The SHB1000S uses a single BLE characteristic (`f62a9f56-f29e-48a8-a317-47ee37a58999`) for both button input (notifications) and LED output (writes). Each LED has two control bytes:

| Byte value | Effect |
|---|---|
| `0x01–0x40` | Backlight brightness — **inverted scale**: `0x01`=max bright, `0x40`=off |
| ON byte | Turn ON the specific LED (see table below) |
| OFF byte = ON byte + `0x17` | Turn OFF the specific LED |

Brightness controls PFD bezel, MFD bezel, and audio panel simultaneously. It must be written **last** in any sequence — the bezel resets brightness on each write.

### Audio panel LED bytes

| Button | ON byte | OFF byte |
|---|---|---|
| COM1/MIC | `0x43` (67) | `0x5a` (90) |
| COM2/MIC | `0x44` (68) | `0x5b` (91) |
| COM1 monitor | `0x47` (71) | `0x5e` (94) |
| COM2 monitor | `0x48` (72) | `0x5f` (95) |
| NAV1 | `0x52` (82) | `0x69` (105) |
| NAV2 | `0x53` (83) | `0x6a` (106) |
| ADF | `0x50` (80) | `0x67` (103) |
| DME | `0x4f` (79) | `0x66` (102) |
| MKR/MUTE | `0x4d` (77) | `0x64` (100) |
| SPKR | `0x4c` (76) | `0x63` (99) |

### LED sandbox

`docs/led_sandbox.py` is a standalone test script that connects directly to the PFD bezel (bypassing the plugin) and tests brightness and LED control against live X-Plane datarefs. Useful for debugging without recompiling.

```bash
pkill -f x1000_bezel.py
bluetoothctl disconnect 00:07:80:A6:E1:71
sleep 2
python3 docs/led_sandbox.py
```

---

## Audio Panel Button Mapping

| Button | X-Plane Command |
|---|---|
| COM1/MIC (press) | `sim/audio_panel/transmit_audio_com1` |
| COM2/MIC (press) | `sim/audio_panel/transmit_audio_com2` |
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

## Tuning

### Heading / Course Knob Feel

All tuning parameters are defined at the **top of `src/UKPHandler.cpp`** in a dedicated section. Edit them there and recompile.

| Define | Default | Effect |
|---|---|---|
| `KNOB_NOISE_MS` | 0.01s | Direction noise filter — opposite clicks within this window are ignored |
| `KNOB_FAST_THRESHOLD` | 0.15s | Clicks faster than this trigger fast-spin mode |
| `KNOB_FAST_REPEAT` | 10 | Commands fired per click in fast-spin mode |
| `HOLD_DELAY_S` | 0.5s | Hold time before NOSE UP/DOWN and cursor keys start repeating |
| `HOLD_RATE_S` | 0.2s | Repeat interval (0.2s = 5/sec) for NOSE UP/DOWN |
| `KNOB_DEBUG_LOG` | 0 | Set to 1 to log every knob event with timing to Log.txt |

**Rules:**
- `KNOB_NOISE_MS` must be ≤ `KNOB_FAST_THRESHOLD` to avoid filtering valid fast clicks
- Bug moves too slowly during fast spin → decrease `KNOB_NOISE_MS`
- Fast-spin mode requires too much speed to trigger → increase `KNOB_FAST_THRESHOLD`

---

## Performance

Tested on Ubuntu 24.04, X-Plane 12, default Cessna 172 G1000:

| Scenario | FPS impact |
|---|---|
| EKVG (light scenery), streaming active | −8 fps (66→58) |
| LFPG (heavy scenery), streaming active | −4 fps (30→26) |
| Non-G1000 aircraft loaded | ~0 fps (plugin detects and stops retrying) |
| G1000 aircraft, screens not popped out | ~0 fps (plugin waits silently, retries every 5s) |

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

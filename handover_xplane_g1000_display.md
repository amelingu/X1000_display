# Project Handover — X-Plane G1000 Display Mirroring on iPad

## Context

This project is a continuation of the Simionic G1000BridgeX Linux port.
The Simionic approach (G1000 NXi app as independent avionics) was fully
reverse-engineered and a working Linux plugin built. This new project takes
a different, cleaner approach:

**Mirror X-Plane's own G1000 display directly onto the iPads, and use
the physical bezels as hardware buttons that control X-Plane's G1000.**

---

## Hardware Setup

- PC running X-Plane 12.4.3 on Ubuntu 24.04
- 2× iPads (PFD iPad and MFD iPad)
- 2× G1000 bezels, each connected to its iPad via Bluetooth
- PC ↔ iPads: Ethernet (USB-C adapter on each iPad)
- iPad IPs: PFD = 192.168.1.15, MFD = 192.168.1.17

---

## What Was Learned from the Simionic Project

### Complete UKP Button Map (bezel → X-Plane command)

Every bezel button sends a `ServerAv|UKP=N` UDP packet to the PC on port 15683.
The PC plugin maps UKP values to `sim/GPS/g1000n1_*` X-Plane commands.

**Protocol rules:**
- Even UKP = button PRESSED → fire command
- Odd UKP = button RELEASED → ignore
- Knob rotation = SINGLE UKP per click (no release event)
- Even rotation UKP = CW click, Odd = CCW click

**Complete PFD button → UKP → X-Plane command mapping:**

| Button | UKP | Command |
|---|---|---|
| AP | 92 | sim/GPS/g1000n1_ap |
| FD | 64 | sim/GPS/g1000n1_fd |
| HDG | 100 | sim/GPS/g1000n1_hdg |
| ALT | 66 | sim/GPS/g1000n1_alt |
| NAV | 98 | sim/GPS/g1000n1_nav |
| VNAV | 60 | sim/GPS/g1000n1_vnv |
| APR | 96 | sim/GPS/g1000n1_apr |
| BC | 58 | sim/GPS/g1000n1_bc |
| VS | 94 | sim/GPS/g1000n1_vs |
| UP | 56 | sim/GPS/g1000n1_nose_up |
| FLC | 90 | sim/GPS/g1000n1_flc |
| DN | 62 | sim/GPS/g1000n1_nose_down |
| NAV vol push | 78 | sim/GPS/g1000n1_nvol |
| NAV standby flip | 74 | sim/GPS/g1000n1_nav_ff |
| NAV knob push | 84 | sim/GPS/g1000n1_nav12 |
| COM vol push | 206 | sim/GPS/g1000n1_cvol |
| COM standby flip | 202 | sim/GPS/g1000n1_com_ff |
| COM knob push | 110 | sim/GPS/g1000n1_com12 |
| HDG knob push | 86 | sim/GPS/g1000n1_hdg_sync |
| ALT knob push | 88 | (no command confirmed) |
| CRS push | 112 | sim/GPS/g1000n1_crs_sync |
| RANGE/cursor push | 32 | sim/GPS/g1000n1_pan_push |
| Cursor UP | 198 | sim/GPS/g1000n1_pan_up |
| Cursor DOWN | 200 | sim/GPS/g1000n1_pan_down |
| Cursor LEFT | 194 | sim/GPS/g1000n1_pan_left |
| Cursor RIGHT | 196 | sim/GPS/g1000n1_pan_right |
| Direct-to | 210 | sim/GPS/g1000n1_direct |
| MENU | 184 | sim/GPS/g1000n1_menu |
| FPL | 212 | sim/GPS/g1000n1_fpl |
| PROC | 182 | sim/GPS/g1000n1_proc |
| CLR | 214 | sim/GPS/g1000n1_clr |
| ENT | 180 | sim/GPS/g1000n1_ent |
| FMS inner push | 190 | sim/GPS/g1000n1_cursor |
| SK01–SK12 | 156,158,…178 | sim/GPS/g1000n1_softkey1–12 |
| DISPLAY BACKUP ON | 142 | (dataref write) |
| DISPLAY BACKUP OFF | 143 | (dataref write) |

**Complete knob rotation UKP map:**

| Knob | CW UKP | CCW UKP | CW command | CCW command |
|---|---|---|---|---|
| NAV vol | 76 | 77 | g1000n1_nvol_up | g1000n1_nvol_dn |
| NAV outer | 82 | 83 | g1000n1_nav_outer_up | g1000n1_nav_outer_down |
| NAV inner | 80 | 81 | g1000n1_nav_inner_up | g1000n1_nav_inner_down |
| HDG | 68 | 69 | g1000n1_hdg_up | g1000n1_hdg_down |
| ALT outer | 72 | 73 | g1000n1_alt_outer_up | g1000n1_alt_outer_down |
| ALT inner | 70 | 71 | g1000n1_alt_inner_up | g1000n1_alt_inner_down |
| FMS outer | 40 | 41 | g1000n1_fms_outer_up | g1000n1_fms_outer_down |
| FMS inner | 38 | 39 | g1000n1_fms_inner_up | g1000n1_fms_inner_down |
| RANGE/cursor | 192 | 193 | g1000n1_range_up | g1000n1_range_down |
| BARO | 102 | 103 | g1000n1_baro_up | g1000n1_baro_down |
| CRS | 104 | 105 | g1000n1_crs_up | g1000n1_crs_down |
| COM outer | 108 | 109 | g1000n1_com_outer_up | g1000n1_com_outer_down |
| COM inner | 106 | 107 | g1000n1_com_inner_up | g1000n1_com_inner_down |
| COM vol | 36 | 37 | g1000n1_cvol_up | g1000n1_cvol_dn |
| AUDIO VOL outer | 148 | 149 | (no command found) | |
| AUDIO VOL inner | 146 | 147 | (no command found) | |

### Communication Protocol (Simionic, for reference)

All messages: plain ASCII, pipe-separated, UDP.

**PC → iPad :15684:**
- `ClientData|LAT=...|LON=...|...` at 13 Hz (60+ flight data fields)
- `Request` at 1 Hz (poll)
- `ClientComingX` on connect (sent twice)
- `ClientOut` on disconnect
- `ClientAv|KEY=val` on avionics state change
- `Hi275` broadcast to :15782 every 3s (discovery beacon)
- `ClientRequest` broadcast to :15682 every 3s (discovery)

**iPad → PC :15683:**
- `ServerAv|UKP=N` on button/knob press (the key message for this project)
- `ServerAv|field=val|...` at 1 Hz (full G1000 state)
- `Answer|A=0` keepalive at 1 Hz
- `ServerReady` on connection
- `ServerStart` on app launch (broadcast discovery)
- `ServerData|JOYX=...|JOYY=...|TP=...|TR=...` at 20 Hz when AP engaged

### Key Findings

1. **MFD bezel buttons generate NO UKP events** in any configuration.
   All bezel input comes through the PFD iPad (192.168.1.15).
   MFD iPad (192.168.1.17) only ever sent one packet (ServerStart on crash).

2. **MFD mode detection**: ServerAv Len=26x = MFD, Len=27x = PFD.
   MFD mode is display-only.

3. **Simionic G1000 NXi app ignores unknown ClientData fields** completely.
   FMS/waypoint sync via ClientData is impossible.

4. **The sim/ GPS/g1000n1_* command namespace** maps exactly to all bezel
   functions — every button and knob has a corresponding X-Plane command.

---

## New Project: X-Plane G1000 Display on iPads

### Goal

Replace the Simionic G1000 app with a direct mirror of X-Plane's own G1000
avionics display. Use the physical bezels to control X-Plane's G1000 directly.

### Architecture

```
X-Plane G1000 PFD render ──► screen capture ──► stream ──► PFD iPad display
X-Plane G1000 MFD render ──► screen capture ──► stream ──► MFD iPad display

PFD bezel (BT→iPad) ──► UDP ──► PC plugin ──► XPLMCommandOnce(g1000n1_*) ──► X-Plane
MFD bezel (BT→iPad) ──► UDP ──► PC plugin ──► XPLMCommandOnce(g1000n1_*) ──► X-Plane
```

### Display Streaming

X-Plane 12 exposes its avionics displays via the **XPLM Avionics API**
(introduced in SDK 4.0, requires X-Plane 12+). This allows a plugin to
read the rendered G1000 textures directly.

Alternative: use X-Plane's built-in **external visuals** or a screen region
capture tool (e.g. ffmpeg capturing the G1000 window area) streamed via
WebRTC or RTSP to an iOS client app on each iPad.

### Bezel Input

The existing Simionic bezel communication protocol is already fully
documented. The new plugin needs to:
1. Listen on UDP :15683 for `ServerAv|UKP=N` from both iPads
2. Map UKP → `XPLMCommandOnce()` using the table above
3. No ClientData stream needed (the display is handled by the stream)

### What the New Plugin Needs vs the Simionic Plugin

| Feature | Simionic plugin | New plugin |
|---|---|---|
| Send ClientData | ✓ (60+ fields, 13 Hz) | ✗ not needed |
| Receive ServerAv state | ✓ (freq, AP targets) | ✗ not needed |
| Handle UKP button events | ✓ | ✓ (core feature) |
| Handle UKP knob events | ✓ | ✓ (core feature) |
| Connection handshake | ✓ (ServerStart/Ready) | ✓ (same protocol) |
| Display streaming | ✗ | ✓ (new) |

### Existing Code to Reuse

From the Simionic project (at ~/Documents/simionic_linux/src/):
- `UDPSocket.h/.cpp` — 100% reusable, no changes needed
- `UKPHandler.h/.cpp` — 100% reusable, the complete bezel map is there
- `Bridge.cpp` — reuse the receive/handshake logic, strip the ClientData sender
- `compile.sh` — reusable with updated source list

### Open Questions for the New Project

1. **Display technology**: XPLM Avionics API texture capture vs screen region
   capture vs X-Plane external visual output?

2. **Streaming protocol**: What iOS app receives the stream?
   - Custom app using WebRTC?
   - Existing screen streaming app (Moonlight, etc.)?
   - Web browser on iPad (WebRTC from plugin HTTP server)?

3. **MFD bezel**: Since MFD bezel buttons generate no UKP events through
   the Simionic protocol, does it need a different communication path?
   (The bezels connect to the iPads via Bluetooth — is there a way to
   intercept Bluetooth HID events directly on the PC?)

4. **X-Plane G1000 commands**: Are all the `sim/GPS/g1000n1_*` commands
   available for the MFD softkeys (g1000n2_* namespace)?

---

## Files from Simionic Project

All source at: ~/Documents/simionic_linux/src/
- UDPSocket.h/.cpp
- DatarefTable.h/.cpp  
- UKPHandler.h/.cpp
- Bridge.h/.cpp
- Plugin.cpp
- compile.sh (project root)

Protocol specification: ~/Documents/simionic_linux/simionic_protocol_spec.md
UKP command mapping:   ~/Documents/simionic_linux/ukp_command_mapping.md

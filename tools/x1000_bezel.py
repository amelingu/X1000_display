#!/usr/bin/env python3
# x1000_bezel.py — SHB1000S bezel BLE input/output bridge
#
# INPUT:  BLE notifications (bezel → plugin UDP :15683/:15685) — button/knob UKP values
# OUTPUT: BLE writes (plugin UDP :15684 → bezel) — LED states and backlight brightness
#
# BLE LED protocol (discovered via Wireshark capture, July 2026):
#   Handle 0x0008, single byte write:
#     0x00        = reset (all LEDs off, backlight off)
#     0x01–0x40   = backlight brightness (1=dim, 64=max)
#     0x41+       = turn ON the LED for the button with that UKP release value
#   Handle 0x0009, write 0x0200 once on connect = enable notifications
#
# LED state UDP protocol (plugin → x1000_bezel.py on port 15684):
#   Binary packet: 1 byte brightness (0-64) + N bytes of active LED UKP values
#   Example: b'\x28\x33\x6b\x6d' = brightness 40, COM1 on, NAV1 on, COM1/MIC on
#
# Requirements:
#   pip install bleak --break-system-packages
#
# Usage:
#   python3 x1000_bezel.py                          # auto-scan for bezels
#   python3 x1000_bezel.py --pfd 00:07:80:A6:E1:71 --mfd 00:07:80:A6:F5:0A
#   python3 x1000_bezel.py --scan                   # scan and identify PFD

import asyncio
import socket
import logging
import argparse
import sys
import struct

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s %(levelname)s %(message)s')
log = logging.getLogger('x1000_bezel')

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("ERROR: bleak not installed.")
    print("Install with: pip install bleak --break-system-packages")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

BEZEL_CHAR_UUID   = "f62a9f56-f29e-48a8-a317-47ee37a58999"
BEZEL_CCCD_HANDLE = 0x0009   # write 0x0200 once to enable notifications
BEZEL_LED_HANDLE  = 0x0008   # single byte: brightness or LED UKP value

DEFAULT_PLUGIN_IP   = "127.0.0.1"
DEFAULT_PFD_PORT    = 15683
DEFAULT_MFD_PORT    = 15685
DEFAULT_LED_PORT    = 15684   # plugin sends LED state here

BEZEL_NAME_FILTER = "1000"

# ---------------------------------------------------------------------------
# UDP sender (UKP → plugin)
# ---------------------------------------------------------------------------

class UDPSender:
    def __init__(self, ip: str, port: int, name: str):
        self.addr = (ip, port)
        self.name = name
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        log.info(f"{name} bezel → {ip}:{port}")

    def send_ukp(self, ukp: int):
        msg = f"ServerAv|UKP={ukp}\n".encode()
        self.sock.sendto(msg, self.addr)

    def close(self):
        self.sock.close()

# ---------------------------------------------------------------------------
# LED state receiver (plugin → bezel script → BLE)
# ---------------------------------------------------------------------------

class LEDReceiver:
    """Listens on a UDP port for LED state packets from the plugin.
    Packet format: 1 byte brightness + N bytes of active LED UKP values.
    Brightness range: 0 (off) to 64 (max).
    LED values: UKP release values for buttons that should be lit."""

    def __init__(self, port: int):
        self.port       = port
        self.brightness = 0
        self.leds       = set()   # set of active LED UKP values
        self._sock      = None

    def start(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(('0.0.0.0', self.port))
        self._sock.setblocking(False)
        log.info(f"LED receiver listening on :{self.port}")

    def poll(self) -> bool:
        """Read pending packets. Returns True if state changed."""
        if not self._sock:
            return False
        changed = False
        try:
            while True:
                data, _ = self._sock.recvfrom(256)
                if len(data) >= 1:
                    new_bright = data[0]
                    new_leds   = set(data[1:])
                    if new_bright != self.brightness or new_leds != self.leds:
                        self.brightness = new_bright
                        self.leds       = new_leds
                        changed = True
        except BlockingIOError:
            pass
        return changed

    def close(self):
        if self._sock:
            self._sock.close()

# ---------------------------------------------------------------------------
# Bezel client (BLE connection)
# ---------------------------------------------------------------------------

class BezelClient:
    def __init__(self, mac: str, name: str, sender: UDPSender):
        self.mac          = mac
        self.name         = name
        self.sender       = sender
        self.client       = BleakClient(mac)
        self._frame_count = 0
        self._last_brightness = -1
        self._last_leds       = set()

    async def connect(self):
        log.info(f"{self.name}: connecting to {self.mac}...")

        # On Windows, pre-scan so bleak can discover the device by MAC
        if sys.platform == 'win32':
            log.info(f"{self.name}: pre-scanning (Windows)...")
            try:
                await BleakScanner.discover(timeout=5.0)
            except Exception as e:
                log.warning(f"{self.name}: pre-scan failed: {e}")

        try:
            if self.client.is_connected:
                await self.client.disconnect()
                await asyncio.sleep(1.0)
            self.client = BleakClient(self.mac)
        except Exception:
            self.client = BleakClient(self.mac)

        for attempt in range(5):
            try:
                await self.client.connect()
                break
            except Exception as e:
                log.warning(f"{self.name}: connect attempt {attempt+1}/5 failed: {e}")
                if attempt < 4:
                    await asyncio.sleep(2.0)
                    self.client = BleakClient(self.mac)
                else:
                    raise

        log.info(f"{self.name}: connected")

        # Enable notifications (write 0x0200 to CCCD handle)
        try:
            await self.client.write_gatt_char(BEZEL_CCCD_HANDLE,
                                              bytearray([0x02, 0x00]),
                                              response=True)
            log.info(f"{self.name}: notifications enabled")
        except Exception as e:
            log.warning(f"{self.name}: CCCD write failed: {e}")

        # Subscribe to button notifications
        await self.client.start_notify(BEZEL_CHAR_UUID, self._on_notification)
        log.info(f"{self.name}: subscribed to button notifications")

        # Reset LED state
        await self._write_led(0x00)
        self._last_brightness = 0
        self._last_leds       = set()

    async def disconnect(self):
        if self.client.is_connected:
            await self._write_led(0x00)
            await self.client.stop_notify(BEZEL_CHAR_UUID)
            await self.client.disconnect()
            log.info(f"{self.name}: disconnected")

    async def _write_led(self, value: int):
        """Write a single byte to the LED control handle."""
        try:
            await self.client.write_gatt_char(BEZEL_LED_HANDLE,
                                              bytearray([value]),
                                              response=False)
        except Exception as e:
            log.debug(f"{self.name}: LED write failed: {e}")

    async def update_leds(self, brightness: int, leds: set):
        """Push updated LED state to the bezel.
        Writes brightness byte first, then each active LED UKP value."""
        if not self.client.is_connected:
            return

        # Write brightness (clamp to 0-64)
        b = max(0, min(64, brightness))
        if b != self._last_brightness:
            await self._write_led(b)
            self._last_brightness = b

        # Turn off LEDs that are no longer active
        for ukp in self._last_leds - leds:
            await self._write_led(0x00)   # reset then re-send active ones
            break  # one reset clears all — resend active below

        # Write active LED UKP values
        if leds != self._last_leds:
            await self._write_led(0x00)   # clear first
            for ukp in leds:
                await self._write_led(ukp)
            self._last_leds = set(leds)

    def _on_notification(self, handle, data: bytearray):
        for byte in data:
            self._frame_count += 1
            if self._frame_count == 1:
                log.info(f"{self.name}: first UKP received: {byte}")
            self.sender.send_ukp(byte)
            log.debug(f"{self.name}: UKP={byte} ({'press/CW' if byte % 2 == 0 else 'release/CCW'})")

    @property
    def is_connected(self):
        return self.client.is_connected

# ---------------------------------------------------------------------------
# Scanner
# ---------------------------------------------------------------------------

async def scan_bezels(timeout: float = 10.0) -> list:
    log.info(f"Scanning for bezels ({timeout}s)...")
    devices = await BleakScanner.discover(timeout=timeout)
    bezels = []
    for d in devices:
        name = d.name or ""
        if BEZEL_NAME_FILTER in name:
            bezels.append((d.address, name))
            log.info(f"  Found: {name} — {d.address}")
    if not bezels:
        log.info("  No bezels found")
    return bezels

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

async def main(args):
    pfd_sender = UDPSender(args.plugin_ip, args.pfd_port, "PFD")
    mfd_sender = UDPSender(args.plugin_ip, args.mfd_port, "MFD")

    # Scan mode
    if args.scan:
        log.info("Scanning for bezels and listening for button presses...")
        log.info("Turn a knob or press buttons on your PFD bezel now...")

        found_bezels = []
        active_mac   = None
        press_counts = {}

        def make_scan_notify(mac, ready_time):
            def on_notify(handle, data):
                nonlocal active_mac
                import time
                if active_mac is not None:
                    return
                if time.time() <= ready_time:
                    return
                press_counts[mac] = press_counts.get(mac, 0) + 1
                count = press_counts[mac]
                log.info(f"Button press on {mac} ({count}/3)")
                if count >= 3:
                    active_mac = mac
                    log.info(f"PFD confirmed: {mac}")
                    print(f"BEZEL_ACTIVE:{mac}", flush=True)
            return on_notify

        devices = await BleakScanner.discover(timeout=5.0)
        for d in devices:
            name = d.name or ""
            if BEZEL_NAME_FILTER in name:
                found_bezels.append((d.address, name))

        clients = []
        for mac, name in found_bezels:
            try:
                c = BleakClient(mac)
                await c.connect()
                import time
                ready_time = time.time() + 1.0
                await c.start_notify(BEZEL_CHAR_UUID, make_scan_notify(mac, ready_time))
                clients.append(c)
                log.info(f"Listening on {name} — {mac}")
            except Exception as e:
                log.warning(f"Could not connect to {mac}: {e}")

        await asyncio.sleep(10.0)

        for c in clients:
            try:
                await c.stop_notify(BEZEL_CHAR_UUID)
                await c.disconnect()
            except Exception:
                pass

        for mac, name in found_bezels:
            print(f"BEZEL_FOUND:{mac}:{name}", flush=True)
        if active_mac:
            print(f"BEZEL_PFD:{active_mac}", flush=True)
        print("BEZEL_SCAN_DONE", flush=True)
        sys.stdout.flush()
        pfd_sender.close()
        mfd_sender.close()
        return

    # Normal mode — determine MACs
    pfd_mac = args.pfd
    mfd_mac = args.mfd

    if not pfd_mac and not mfd_mac:
        log.info("No MAC specified — auto-scanning...")
        bezels = await scan_bezels()
        if not bezels:
            log.error("No bezels found.")
            return
        if len(bezels) >= 1:
            pfd_mac = bezels[0][0]
            log.info(f"Auto-assigned PFD: {bezels[0][1]} {pfd_mac}")
        if len(bezels) >= 2:
            mfd_mac = bezels[1][0]
            log.info(f"Auto-assigned MFD: {bezels[1][1]} {mfd_mac}")

    # LED state receiver
    led_rx = LEDReceiver(args.led_port)
    led_rx.start()

    # Connect bezels
    clients = []
    if pfd_mac:
        clients.append(BezelClient(pfd_mac, "PFD", pfd_sender))
    if mfd_mac:
        clients.append(BezelClient(mfd_mac, "MFD", mfd_sender))

    if not clients:
        log.error("No bezel MAC addresses available.")
        led_rx.close()
        return

    for client in clients:
        try:
            await client.connect()
        except Exception as e:
            log.error(f"{client.name}: connection failed: {e}")

    connected = [c for c in clients if c.is_connected]
    if not connected:
        log.error("No bezels connected.")
        pfd_sender.close()
        mfd_sender.close()
        led_rx.close()
        return

    log.info(f"{len(connected)} bezel(s) connected. Press Ctrl+C to stop.")

    # Main loop — poll LED state and push to bezels
    try:
        while True:
            await asyncio.sleep(0.1)   # 10Hz poll

            # Poll LED state from plugin
            led_rx.poll()

            # Push to all connected bezels
            for client in clients:
                if client.is_connected:
                    await client.update_leds(led_rx.brightness, led_rx.leds)
                else:
                    log.warning(f"{client.name}: disconnected — reconnecting...")
                    await asyncio.sleep(3.0)
                    try:
                        await client.connect()
                        log.info(f"{client.name}: reconnected")
                    except Exception as e:
                        log.error(f"{client.name}: reconnect failed: {e}")

    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
    finally:
        for client in clients:
            await client.disconnect()
        pfd_sender.close()
        mfd_sender.close()
        led_rx.close()
        log.info("Stopped.")

# ---------------------------------------------------------------------------

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='X1000 bezel BLE input/output bridge')
    parser.add_argument('--pfd',      metavar='MAC')
    parser.add_argument('--mfd',      metavar='MAC')
    parser.add_argument('--scan',     action='store_true')
    parser.add_argument('--plugin-ip',  default=DEFAULT_PLUGIN_IP)
    parser.add_argument('--pfd-port', type=int, default=DEFAULT_PFD_PORT)
    parser.add_argument('--mfd-port', type=int, default=DEFAULT_MFD_PORT)
    parser.add_argument('--led-port', type=int, default=DEFAULT_LED_PORT)
    args = parser.parse_args()

    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        pass

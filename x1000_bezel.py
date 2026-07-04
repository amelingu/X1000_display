#!/usr/bin/env python3
# x1000_bezel.py — SHB1000S bezel BLE input bridge
#
# Connects to Simionic SHB1000S bezel(s) via Bluetooth LE,
# subscribes to button/knob notifications, and forwards
# UKP values to the X1000_display plugin via UDP.
#
# The plugin's ConnectionManager listens on UDP :15683 and
# dispatches UKP commands to XPLMCommandOnce (g1000n1_* / g1000n2_*).
#
# Protocol (UKP over BLE):
#   Bezel → BLE ATT notification → single byte = UKP number
#   Even byte = button press or knob CW
#   Odd  byte = button release or knob CCW
#   Multiple bytes in one notification = rapid input
#
# Protocol (UKP over UDP to plugin):
#   "ServerAv|UKP=N\n"  where N is the UKP byte value
#
# Requirements:
#   pip install bleak --break-system-packages
#
# Usage:
#   python3 x1000_bezel.py                          # auto-scan for SHB1000
#   python3 x1000_bezel.py --pfd 00:07:80:A6:F5:0A  # explicit PFD MAC
#   python3 x1000_bezel.py --pfd XX:XX:XX:XX:XX:XX --mfd YY:YY:YY:YY:YY:YY
#   python3 x1000_bezel.py --scan                   # scan and list bezels
#   python3 x1000_bezel.py --plugin-ip 192.168.1.12 --plugin-port 15683

import asyncio
import socket
import logging
import argparse
import sys

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

# BLE characteristic UUID for bezel button notifications
BEZEL_CHAR_UUID = "f62a9f56-f29e-48a8-a317-47ee37a58999"

# Default plugin UDP address
DEFAULT_PLUGIN_IP   = "127.0.0.1"
DEFAULT_PFD_PORT    = 15683   # plugin listens for PFD UKP on this port
DEFAULT_MFD_PORT    = 15685   # plugin listens for MFD UKP on this port
                               # (15684 is reserved for backlight output)

# Device name prefix to auto-detect bezels
BEZEL_NAME_PREFIX = "SHB1000"

# ---------------------------------------------------------------------------
# UDP sender
# ---------------------------------------------------------------------------

class UDPSender:
    """One sender per bezel — each sends to a different port so the
    plugin knows which bezel (PFD/MFD) the UKP came from."""
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
# Bezel connection
# ---------------------------------------------------------------------------

class BezelClient:
    def __init__(self, mac: str, name: str, sender: UDPSender):
        self.mac    = mac
        self.name   = name
        self.sender = sender
        self.client = BleakClient(mac)
        self._frame_count = 0

    async def connect(self):
        log.info(f"{self.name}: connecting to {self.mac}...")
        await self.client.connect()
        log.info(f"{self.name}: connected")

        # Subscribe to notifications
        await self.client.start_notify(BEZEL_CHAR_UUID, self._on_notification)
        log.info(f"{self.name}: subscribed to button notifications")

    async def disconnect(self):
        if self.client.is_connected:
            await self.client.stop_notify(BEZEL_CHAR_UUID)
            await self.client.disconnect()
            log.info(f"{self.name}: disconnected")

    def _on_notification(self, handle, data: bytearray):
        """Called for each BLE notification — data is one or more UKP bytes."""
        for byte in data:
            ukp = byte
            self._frame_count += 1
            if self._frame_count == 1:
                log.info(f"{self.name}: first UKP received: {ukp}")

            self.sender.send_ukp(ukp)

            action = "press/CW" if ukp % 2 == 0 else "release/CCW"
            log.debug(f"{self.name}: UKP={ukp} ({action})")

    @property
    def is_connected(self):
        return self.client.is_connected

# ---------------------------------------------------------------------------
# Scanner
# ---------------------------------------------------------------------------

async def scan_bezels(timeout: float = 10.0) -> list:
    """Scan for SHB1000 devices and return list of (mac, name) tuples."""
    log.info(f"Scanning for bezels ({timeout}s)...")
    devices = await BleakScanner.discover(timeout=timeout)
    bezels = []
    for d in devices:
        name = d.name or ""
        if BEZEL_NAME_PREFIX in name:
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
        await scan_bezels(timeout=15.0)
        pfd_sender.close()
        mfd_sender.close()
        return

    # Determine MACs
    pfd_mac = args.pfd
    mfd_mac = args.mfd

    if not pfd_mac and not mfd_mac:
        log.info("No MAC specified — auto-scanning...")
        bezels = await scan_bezels()
        if not bezels:
            log.error("No bezels found. Use --pfd / --mfd to specify MAC addresses.")
            return
        if len(bezels) >= 1:
            pfd_mac = bezels[0][0]
            log.info(f"Auto-assigned PFD: {bezels[0][1]} {pfd_mac}")
        if len(bezels) >= 2:
            mfd_mac = bezels[1][0]
            log.info(f"Auto-assigned MFD: {bezels[1][1]} {mfd_mac}")

    # Connect clients — each bezel gets its own sender (different UDP port)
    clients = []
    if pfd_mac:
        clients.append(BezelClient(pfd_mac, "PFD", pfd_sender))
    if mfd_mac:
        clients.append(BezelClient(mfd_mac, "MFD", mfd_sender))

    if not clients:
        log.error("No bezel MAC addresses available.")
        return

    # Connect all
    for client in clients:
        try:
            await client.connect()
        except Exception as e:
            log.error(f"{client.name}: connection failed: {e}")

    connected = [c for c in clients if c.is_connected]
    if not connected:
        log.error("No bezels connected.")
        sender.close()
        return

    log.info(f"{len(connected)} bezel(s) connected. Press Ctrl+C to stop.")

    # Keep alive — reconnect if disconnected
    try:
        while True:
            await asyncio.sleep(2.0)
            for client in clients:
                if not client.is_connected:
                    log.warning(f"{client.name}: disconnected — reconnecting...")
                    try:
                        await client.connect()
                    except Exception as e:
                        log.error(f"{client.name}: reconnect failed: {e}")
    except asyncio.CancelledError:
        pass
    except KeyboardInterrupt:
        pass
    finally:
        for client in clients:
            await client.disconnect()
        pfd_sender.close()
        mfd_sender.close()
        log.info("Stopped.")

# ---------------------------------------------------------------------------

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='X1000 bezel BLE → UDP bridge for X1000_display plugin')
    parser.add_argument('--pfd',  metavar='MAC',
                        help='PFD bezel Bluetooth MAC address')
    parser.add_argument('--mfd',  metavar='MAC',
                        help='MFD bezel Bluetooth MAC address')
    parser.add_argument('--scan', action='store_true',
                        help='Scan for bezels and exit')
    parser.add_argument('--plugin-ip',   default=DEFAULT_PLUGIN_IP,
                        help=f'Plugin host IP (default: {DEFAULT_PLUGIN_IP})')
    parser.add_argument('--pfd-port', type=int, default=15683,
                        help='UDP port for PFD bezel (default: 15683)')
    parser.add_argument('--mfd-port', type=int, default=15685,
                        help='UDP port for MFD bezel (default: 15685)')
    args = parser.parse_args()

    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        pass

#!/usr/bin/env python3
# led_sandbox.py — X1000 LED control sandbox v4
#
# Mode 2: manual step through LED bytes 0x41-0xFF
# Press Enter to advance, type a label to record what lit up, q to quit.
# Outputs a CSV file: led_scan_results.csv

import asyncio
import socket
import struct
import logging
import sys
import csv
import os

logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')
log = logging.getLogger('led_sandbox')

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("pip install bleak --break-system-packages")
    sys.exit(1)

PFD_MAC         = "00:07:80:A6:E1:71"
BEZEL_CHAR_UUID = "f62a9f56-f29e-48a8-a317-47ee37a58999"
XPLANE_IP       = "127.0.0.1"
XPLANE_PORT     = 49000
LISTEN_PORT     = 49100
RREF_BRIGHT     = 1
RREF_ADF        = 2

def subscribe_dataref(sock, dataref, rref_id, freq=5):
    packet = struct.pack('<4sxii', b'RREF', freq, rref_id)
    packet += dataref.encode().ljust(400, b'\x00')
    sock.sendto(packet, (XPLANE_IP, XPLANE_PORT))

def unsubscribe_dataref(sock, dataref, rref_id):
    packet = struct.pack('<4sxii', b'RREF', 0, rref_id)
    packet += dataref.encode().ljust(400, b'\x00')
    sock.sendto(packet, (XPLANE_IP, XPLANE_PORT))

def parse_rref(data):
    results = []
    if len(data) < 5 or data[:4] != b'RREF':
        return results
    offset = 5
    while offset + 8 <= len(data):
        rref_id, value = struct.unpack_from('<if', data, offset)
        results.append((rref_id, value))
        offset += 8
    return results

async def write(client, value):
    await client.write_gatt_char(BEZEL_CHAR_UUID, bytearray([value]), response=True)

async def main():
    mode = input(
        "Mode?\n"
        "  1 = Live brightness (inverted scale)\n"
        "  2 = Manual LED byte scan with CSV output\n"
        "Choice: ").strip()

    log.info("Scanning...")
    await BleakScanner.discover(timeout=5.0)
    log.info(f"Connecting to {PFD_MAC}...")

    async with BleakClient(PFD_MAC) as client:
        log.info("Connected")

        if mode == '2':
            csv_path = os.path.expanduser("~/Documents/X1000_display/docs/led_scan_results.csv")
            results = []

            print("\n--- LED Byte Scanner ---")
            print("For each byte value, note which LED lights up on the audio panel.")
            print("Press Enter to advance to next value.")
            print("Type a label (e.g. 'ADF', 'NAV1', 'COM1') then Enter to record and advance.")
            print("Type 'q' then Enter to quit early.\n")

            # Set medium brightness (0x20 = mid range, inverted scale)
            await write(client, 0x20)
            await asyncio.sleep(0.3)

            loop = asyncio.get_event_loop()

            for v in range(0x41, 0x100):
                # Write the test byte
                await write(client, v)

                # Get user input (blocking is fine here — this is interactive)
                label = await loop.run_in_executor(
                    None,
                    lambda: input(f"0x{v:02x} ({v:3d}) → LED: "))

                label = label.strip()
                if label.lower() == 'q':
                    results.append({'byte_hex': f"0x{v:02x}", 'byte_dec': v, 'led': ''})
                    break

                results.append({
                    'byte_hex': f"0x{v:02x}",
                    'byte_dec': v,
                    'led': label
                })

                # Restore brightness between steps
                await write(client, 0x20)
                await asyncio.sleep(0.1)

            # Write CSV
            with open(csv_path, 'w', newline='') as f:
                writer = csv.DictWriter(f, fieldnames=['byte_hex', 'byte_dec', 'led'])
                writer.writeheader()
                writer.writerows(results)
            print(f"\nResults saved to: {csv_path}")

            # Show non-empty entries
            print("\nLabelled entries:")
            for r in results:
                if r['led']:
                    print(f"  {r['byte_hex']} ({r['byte_dec']:3d}) → {r['led']}")

            await write(client, 0x00)
            return

        # --- Mode 1: Live brightness ---
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('0.0.0.0', LISTEN_PORT))
        sock.setblocking(False)
        subscribe_dataref(sock,
            "sim/cockpit2/electrical/panel_brightness_ratio[3]", RREF_BRIGHT)
        subscribe_dataref(sock,
            "sim/cockpit2/radios/actuators/audio_selection_adf1", RREF_ADF)
        log.info("Live mode. Turn brightness knob. Ctrl+C to stop.")

        last = {'brightness': -1}
        try:
            while True:
                await asyncio.sleep(0.1)
                try:
                    while True:
                        data, _ = sock.recvfrom(1024)
                        for rref_id, value in parse_rref(data):
                            if rref_id == RREF_BRIGHT:
                                b = int(value * 64)
                                if b != last['brightness']:
                                    log.info(f"panel_brightness={value:.3f} → send=0x{b:02x} ({b})")
                                    await write(client, b)
                                    last['brightness'] = b
                except BlockingIOError:
                    pass
        except KeyboardInterrupt:
            pass
        finally:
            unsubscribe_dataref(sock,
                "sim/cockpit2/electrical/panel_brightness_ratio[3]", RREF_BRIGHT)
            unsubscribe_dataref(sock,
                "sim/cockpit2/radios/actuators/audio_selection_adf1", RREF_ADF)
            await write(client, 0x00)
            sock.close()
            log.info("Done.")

if __name__ == '__main__':
    asyncio.run(main())

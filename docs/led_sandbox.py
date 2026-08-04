#!/usr/bin/env python3
# led_sandbox.py — X1000 LED control sandbox v10
#
# PROTOCOL FULLY DECODED (from Wireshark capture, August 2026):
#   Each LED has an ON byte and an OFF byte: OFF = ON + 0x17
#   No reset (0x00) needed — just write ON or OFF byte directly.
#   Brightness: 0x00=max bright, 0x40=off (inverted scale)
#
# LED ON bytes (from BLE scan July 2026):
#   COM1_MIC=0x43  COM2_MIC=0x44  COM3_MIC=0x45  COM_1/2=0x46
#   COM1=0x47      COM2=0x48      COM3=0x49      TEL=0x4a
#   PA=0x4b        SPKR=0x4c      MKR/MUTE=0x4d  HI_SENS=0x4e
#   DME=0x4f       ADF=0x50       AUX=0x51       NAV1=0x52
#   NAV2=0x53      MAN_SQ=0x54    PLAY=0x55      PILOT=0x56
#   COPLT=0x57     VOL=0x58       SQ=0x59
# LED OFF bytes = ON byte + 0x17

import asyncio
import socket
import struct
import logging
import sys

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

LED_OFF_OFFSET = 0x17   # OFF byte = ON byte + 0x17

# LED ON bytes
LED = {
    'COM1_MIC': 0x43, 'COM2_MIC': 0x44, 'COM3_MIC': 0x45, 'COM_12': 0x46,
    'COM1':     0x47, 'COM2':     0x48, 'COM3':     0x49, 'TEL':    0x4a,
    'PA':       0x4b, 'SPKR':     0x4c, 'MKR':      0x4d, 'HI':     0x4e,
    'DME':      0x4f, 'ADF':      0x50, 'AUX':      0x51, 'NAV1':   0x52,
    'NAV2':     0x53, 'MAN_SQ':   0x54, 'PLAY':     0x55, 'PILOT':  0x56,
    'COPLT':    0x57, 'VOL':      0x58, 'SQ':        0x59,
}

RREF_BRIGHT = 1
RREF_ADF    = 2
RREF_NAV1   = 3
RREF_COM1   = 4
RREF_MIC1   = 5

def make_socket():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', LISTEN_PORT))
    sock.setblocking(False)
    return sock

def subscribe(sock, dataref, rref_id, freq=10):
    packet = struct.pack('<4sxii', b'RREF', freq, rref_id)
    packet += dataref.encode().ljust(400, b'\x00')
    sock.sendto(packet, (XPLANE_IP, XPLANE_PORT))
    log.info(f"Subscribed [{rref_id}]: {dataref}")

def unsubscribe(sock, dataref, rref_id):
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

async def w(client, value):
    await client.write_gatt_char(
        BEZEL_CHAR_UUID, bytearray([value]), response=True)
    log.debug(f"  BLE → 0x{value:02x} ({value})")

async def led_on(client, led_name):
    await w(client, LED[led_name])

async def led_off(client, led_name):
    await w(client, LED[led_name] + LED_OFF_OFFSET)

async def init_bezel(client, brightness):
    """Send initial state: brightness + all tracked LEDs off."""
    await w(client, brightness)
    for name in LED:
        await w(client, LED[name] + LED_OFF_OFFSET)

async def main():
    log.info("Scanning...")
    await BleakScanner.discover(timeout=5.0)
    log.info(f"Connecting to {PFD_MAC}...")

    async with BleakClient(PFD_MAC) as client:
        log.info("Connected")

        sock = make_socket()
        subscribe(sock, "sim/cockpit2/electrical/panel_brightness_ratio[3]", RREF_BRIGHT)
        subscribe(sock, "sim/cockpit2/radios/actuators/audio_selection_adf1",  RREF_ADF)
        subscribe(sock, "sim/cockpit2/radios/actuators/audio_selection_nav1",  RREF_NAV1)
        subscribe(sock, "sim/cockpit2/radios/actuators/audio_selection_com1",  RREF_COM1)
        subscribe(sock, "sim/cockpit2/radios/actuators/audio_com_selection",   RREF_MIC1)

        # Wait for first packet to know brightness
        log.info("Waiting for X-Plane data...")
        await asyncio.sleep(0.5)

        brightness = 0x20
        adf_on = nav1_on = com1_on = mic1_on = False

        # Read initial state
        try:
            while True:
                data, _ = sock.recvfrom(1024)
                for rref_id, value in parse_rref(data):
                    if rref_id == RREF_BRIGHT:
                        brightness = int((1.0 - value) * 64)
                    elif rref_id == RREF_ADF:
                        adf_on  = (value > 0.5)
                    elif rref_id == RREF_NAV1:
                        nav1_on = (value > 0.5)
                    elif rref_id == RREF_COM1:
                        com1_on = (value > 0.5)
                    elif rref_id == RREF_MIC1:
                        mic1_on = (int(value) == 6)  # 6=COM1 mic
        except BlockingIOError:
            pass

        # Initialise bezel: set brightness, turn all LEDs off
        log.info(f"Initialising bezel: brightness=0x{brightness:02x}")
        await w(client, brightness)
        for name in LED:
            await w(client, LED[name] + LED_OFF_OFFSET)

        # Set initial LED state from datarefs
        if com1_on: await led_on(client, 'COM1')
        if mic1_on: await led_on(client, 'COM1_MIC')
        if nav1_on: await led_on(client, 'NAV1')
        if adf_on:  await led_on(client, 'ADF')
        log.info(f"Initial: com1={com1_on} mic1={mic1_on} nav1={nav1_on} adf={adf_on}")

        log.info("Live mode — toggle ADF/NAV1/COM1 in sim. Ctrl+C to stop.")

        # Track previous state to send only changes
        last = {
            'brightness': brightness,
            'adf': adf_on, 'nav1': nav1_on,
            'com1': com1_on, 'mic1': mic1_on,
        }

        try:
            while True:
                await asyncio.sleep(0.1)
                try:
                    while True:
                        data, _ = sock.recvfrom(1024)
                        for rref_id, value in parse_rref(data):
                            if rref_id == RREF_BRIGHT:
                                brightness = int((1.0 - value) * 64)
                            elif rref_id == RREF_ADF:
                                adf_on  = (value > 0.5)
                            elif rref_id == RREF_NAV1:
                                nav1_on = (value > 0.5)
                            elif rref_id == RREF_COM1:
                                com1_on = (value > 0.5)
                            elif rref_id == RREF_MIC1:
                                mic1_on = (int(value) == 6)
                except BlockingIOError:
                    pass

                # Send only what changed
                if brightness != last['brightness']:
                    log.info(f"brightness → 0x{brightness:02x}")
                    await w(client, brightness)
                    last['brightness'] = brightness

                for flag, name, key in [
                    (adf_on,  'ADF',      'adf'),
                    (nav1_on, 'NAV1',     'nav1'),
                    (com1_on, 'COM1',     'com1'),
                    (mic1_on, 'COM1_MIC', 'mic1'),
                ]:
                    if flag != last[key]:
                        if flag:
                            log.info(f"{name} ON")
                            await led_on(client, name)
                        else:
                            log.info(f"{name} OFF")
                            await led_off(client, name)
                        last[key] = flag

        except KeyboardInterrupt:
            pass
        finally:
            unsubscribe(sock, "sim/cockpit2/electrical/panel_brightness_ratio[3]", RREF_BRIGHT)
            unsubscribe(sock, "sim/cockpit2/radios/actuators/audio_selection_adf1",  RREF_ADF)
            unsubscribe(sock, "sim/cockpit2/radios/actuators/audio_selection_nav1",  RREF_NAV1)
            unsubscribe(sock, "sim/cockpit2/radios/actuators/audio_selection_com1",  RREF_COM1)
            unsubscribe(sock, "sim/cockpit2/radios/actuators/audio_com_selection",   RREF_MIC1)
            await w(client, 0x00)
            sock.close()
            log.info("Done.")

if __name__ == '__main__':
    asyncio.run(main())

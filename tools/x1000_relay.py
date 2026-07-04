#!/usr/bin/env python3
# x1000_relay.py — X1000 display relay: UDP frames -> WebSocket -> Safari
#
# No external dependencies — uses only Python standard library.
#
# Usage:
#   python3 x1000_relay.py
#
# PFD iPad: open Safari -> http://192.168.1.12:8080/
# MFD iPad: open Safari -> http://192.168.1.12:8081/

import asyncio
import hashlib
import base64
import struct
import socket
import logging
import sys

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s %(levelname)s %(message)s')
log = logging.getLogger('x1000')

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

UDP_PORT_PFD = 9000
UDP_PORT_MFD = 9001
WS_PORT_PFD  = 8080
WS_PORT_MFD  = 8081

MAGIC_FRAME = 0x464B3158
MAGIC_CHUNK = 0x434B3158

# ---------------------------------------------------------------------------
# HTML page
# ---------------------------------------------------------------------------

HTML = """<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<title>X1000</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html{background:#000;height:100%}
body{
  width:100vw;
  height:100vh;
  background:#000;
  overflow:hidden;
  display:flex;
  align-items:center;
  justify-content:center;
  /* respect safe area so image sits below status bar */
  padding-top:env(safe-area-inset-top);
  padding-bottom:env(safe-area-inset-bottom);
  padding-left:env(safe-area-inset-left);
  padding-right:env(safe-area-inset-right);
}
#d{
  width:100%;
  height:100%;
  object-fit:fill;
  display:block;
}
#s{position:fixed;bottom:calc(8px + env(safe-area-inset-bottom));
   left:50%;transform:translateX(-50%);
   color:#666;font:12px monospace;pointer-events:none;z-index:10}
</style>
</head>
<body>
<img id="d">
<div id="s">Connecting...</div>
<script>
var img=document.getElementById('d');
var st=document.getElementById('s');
var url='ws://'+location.hostname+':'+location.port+'/ws';
var prev=null;
function connect(){
  var ws=new WebSocket(url);
  ws.binaryType='arraybuffer';
  ws.onopen=function(){st.textContent='Connected';setTimeout(function(){st.style.display='none'},2000)};
  ws.onmessage=function(e){
    var blob=new Blob([e.data],{type:'image/jpeg'});
    var u=URL.createObjectURL(blob);
    img.onload=function(){if(prev)URL.revokeObjectURL(prev);prev=u};
    img.src=u;
  };
  ws.onclose=function(){st.style.display='';st.textContent='Reconnecting...';setTimeout(connect,1000)};
  ws.onerror=function(){ws.close()};
}
connect();
</script>
</body>
</html>"""

HTML_BYTES = HTML.encode('utf-8')

# ---------------------------------------------------------------------------
# Minimal WebSocket server (RFC 6455) — no external libs
# ---------------------------------------------------------------------------

WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'

def ws_handshake_response(key: str) -> bytes:
    accept = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode()).digest()
    ).decode()
    return (
        'HTTP/1.1 101 Switching Protocols\r\n'
        'Upgrade: websocket\r\n'
        'Connection: Upgrade\r\n'
        f'Sec-WebSocket-Accept: {accept}\r\n'
        '\r\n'
    ).encode()

def ws_frame(data: bytes) -> bytes:
    """Encode binary WebSocket frame (opcode 0x02)."""
    length = len(data)
    if length <= 125:
        header = bytes([0x82, length])
    elif length <= 65535:
        header = struct.pack('!BBH', 0x82, 126, length)
    else:
        header = struct.pack('!BBQ', 0x82, 127, length)
    return header + data

def http_response(body: bytes, content_type: str = 'text/html; charset=utf-8') -> bytes:
    return (
        f'HTTP/1.1 200 OK\r\n'
        f'Content-Type: {content_type}\r\n'
        f'Content-Length: {len(body)}\r\n'
        f'Connection: close\r\n'
        f'\r\n'
    ).encode() + body

# ---------------------------------------------------------------------------
# Frame reassembler
# ---------------------------------------------------------------------------

class Reassembler:
    def __init__(self):
        self.pending = {}

    def feed(self, data: bytes):
        if len(data) < 12:
            return None
        magic = struct.unpack_from('<I', data, 0)[0]
        if magic == MAGIC_FRAME:
            dlen = struct.unpack_from('<I', data, 8)[0]
            return data[12:12+dlen]
        elif magic == MAGIC_CHUNK:
            if len(data) < 20:
                return None
            seq, idx, total, dlen = struct.unpack_from('<IIII', data, 4)
            chunk = data[20:20+dlen]
            if seq not in self.pending:
                self.pending[seq] = {}
            self.pending[seq][idx] = chunk
            if len(self.pending[seq]) == total:
                frame = b''.join(self.pending[seq][i] for i in range(total))
                del self.pending[seq]
                for k in [k for k in self.pending if k < seq-5]:
                    del self.pending[k]
                return frame
        return None

# ---------------------------------------------------------------------------
# Relay class
# ---------------------------------------------------------------------------

class Relay:
    def __init__(self, name, udp_port, ws_port):
        self.name     = name
        self.udp_port = udp_port
        self.ws_port  = ws_port
        self.clients  = set()   # set of asyncio.StreamWriter
        self.reassembler = Reassembler()
        self.frame_count = 0

    async def handle_connection(self, reader, writer):
        """Handle incoming TCP connection — either HTTP or WebSocket upgrade."""
        try:
            # Read HTTP request headers
            request = b''
            while b'\r\n\r\n' not in request:
                chunk = await reader.read(4096)
                if not chunk:
                    return
                request += chunk

            headers_raw = request.split(b'\r\n\r\n')[0].decode('utf-8', errors='replace')
            headers = {}
            for line in headers_raw.split('\r\n')[1:]:
                if ':' in line:
                    k, _, v = line.partition(':')
                    headers[k.strip().lower()] = v.strip()

            # WebSocket upgrade?
            if headers.get('upgrade', '').lower() == 'websocket':
                key = headers.get('sec-websocket-key', '')
                writer.write(ws_handshake_response(key))
                await writer.drain()

                addr = writer.get_extra_info('peername')
                log.info(f'{self.name}: Safari connected from {addr}')
                self.clients.add(writer)
                try:
                    # Keep alive — read and discard incoming WS frames
                    while True:
                        data = await reader.read(256)
                        if not data:
                            break
                except Exception:
                    pass
                finally:
                    self.clients.discard(writer)
                    log.info(f'{self.name}: Safari disconnected')
            else:
                # Plain HTTP — serve the HTML page
                writer.write(http_response(HTML_BYTES))
                await writer.drain()
        except Exception as e:
            log.debug(f'{self.name}: connection error: {e}')
        finally:
            try:
                writer.close()
            except Exception:
                pass

    async def udp_receiver(self):
        loop = asyncio.get_event_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4*1024*1024)
        sock.bind(('0.0.0.0', self.udp_port))
        sock.setblocking(False)
        log.info(f'{self.name}: UDP listening on :{self.udp_port}')

        while True:
            try:
                data = await loop.sock_recv(sock, 65536)
            except Exception:
                await asyncio.sleep(0.001)
                continue

            frame = self.reassembler.feed(data)
            if frame is None:
                continue

            self.frame_count += 1
            if self.frame_count == 1:
                log.info(f'{self.name}: first frame {len(frame)} bytes')

            if not self.clients:
                continue

            msg = ws_frame(frame)
            dead = set()
            for writer in list(self.clients):
                try:
                    writer.write(msg)
                    await writer.drain()
                except Exception:
                    dead.add(writer)
            self.clients -= dead

    async def run(self):
        server = await asyncio.start_server(
            self.handle_connection, '0.0.0.0', self.ws_port,
            reuse_port=True)
        log.info(f'{self.name}: HTTP/WS on :{self.ws_port} '
                 f'-> open http://<PC_IP>:{self.ws_port}/ on iPad')
        async with server:
            await asyncio.gather(
                server.serve_forever(),
                self.udp_receiver()
            )

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

async def main():
    pfd = Relay('PFD', UDP_PORT_PFD, WS_PORT_PFD)
    mfd = Relay('MFD', UDP_PORT_MFD, WS_PORT_MFD)
    log.info(f'PFD iPad: http://<PC_IP>:{WS_PORT_PFD}/')
    log.info(f'MFD iPad: http://<PC_IP>:{WS_PORT_MFD}/')
    await asyncio.gather(pfd.run(), mfd.run())

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='X1000 display relay')
    parser.add_argument('--pfd-port', type=int, default=UDP_PORT_PFD)
    parser.add_argument('--mfd-port', type=int, default=UDP_PORT_MFD)
    args = parser.parse_args()
    UDP_PORT_PFD = args.pfd_port
    UDP_PORT_MFD = args.mfd_port
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info('Stopped.')

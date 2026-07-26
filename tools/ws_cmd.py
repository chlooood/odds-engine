#!/usr/bin/env python3
"""Send one live-configuration command to a running odds_engine --ws.

    python3 tools/ws_cmd.py 8080 set stale-ms 5000
    python3 tools/ws_cmd.py 8080 set devig power

Exists so the control channel can be exercised without a browser, and so CI
can assert on the ACK. Speaks just enough RFC 6455 to connect, send one masked
text frame, and read one reply.
"""
import base64, json, os, socket, struct, sys


def read_frame(sock):
    def need(n):
        buf = b""
        while len(buf) < n:
            d = sock.recv(n - len(buf))
            if not d:
                raise RuntimeError("connection closed mid-frame")
            buf += d
        return buf

    b0, b1 = need(2)
    length = b1 & 0x7F
    if length == 126:
        length = struct.unpack("!H", need(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", need(8))[0]
    mask = need(4) if (b1 & 0x80) else None
    payload = need(length)
    if mask:
        payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
    return b0 & 0x0F, payload


def main():
    if len(sys.argv) < 5:
        print(__doc__.strip())
        return 2
    port = int(sys.argv[1])
    cmd, key, raw = sys.argv[2], sys.argv[3], sys.argv[4]
    try:
        value = float(raw) if raw.replace(".", "", 1).isdigit() else raw
    except ValueError:
        value = raw

    sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
    nonce = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((
        "GET / HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {nonce}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode())

    buf = b""
    while b"\r\n\r\n" not in buf:
        d = sock.recv(4096)
        if not d:
            raise RuntimeError("closed during handshake")
        buf += d
    if b" 101 " not in buf.split(b"\r\n")[0]:
        raise RuntimeError("handshake refused: " + buf.split(b"\r\n")[0].decode())

    payload = json.dumps({"cmd": cmd, "key": key, "value": value}).encode()
    mask = os.urandom(4)
    header = bytearray([0x81])
    n = len(payload)
    if n < 126:
        header.append(0x80 | n)
    elif n <= 0xFFFF:
        header.append(0x80 | 126)
        header += struct.pack("!H", n)
    else:
        header.append(0x80 | 127)
        header += struct.pack("!Q", n)
    sock.sendall(bytes(header) + mask +
                 bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))

    # The engine also pushes price frames; skip them until the ACK arrives.
    for _ in range(50):
        opcode, data = read_frame(sock)
        if opcode == 0x8:
            raise RuntimeError("engine closed the connection")
        if opcode != 0x1:
            continue
        msg = json.loads(data)
        if "ok" in msg:
            print(json.dumps(msg))
            return 0 if msg["ok"] else 1
    raise RuntimeError("no acknowledgement received")


if __name__ == "__main__":
    sys.exit(main())

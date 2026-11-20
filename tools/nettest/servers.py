"""Local test servers: HTTP on 18080, HTTPS (self-signed, if cert.pem/key.pem exist) on 18443, WebSocket on 18081."""
import asyncio, base64, gzip, hashlib, json, ssl, struct, sys, threading, time, os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *a):
        pass

    def reply(self, code, body, ctype='text/plain', extra=()):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_ANY(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n) if n else b''
        p = u.path
        if p == '/hello':
            self.reply(200, 'hello world')
        elif p == '/json':
            self.reply(200, json.dumps({'a': 1, 'b': [1, 2, 3], 's': 'x'}), 'application/json')
        elif p == '/echo':
            self.reply(200, json.dumps({'method': self.command, 'body': body.decode('latin1'), 'query': u.query,
                                        'headers': {k.lower(): v for k, v in self.headers.items()}}), 'application/json')
        elif p == '/slow':
            time.sleep(int(q.get('ms', ['1000'])[0]) / 1000)
            self.reply(200, 'slow done')
        elif p == '/hang':
            time.sleep(30)
            try:
                self.reply(200, 'late')
            except Exception:
                pass
        elif p == '/big':
            data = (b'0123456789abcdef' * 1000000)[:int(q.get('n', ['1000000'])[0])]
            self.reply(200, data, 'application/octet-stream')
        elif p == '/redirect':
            self.reply(302, '', extra=[('Location', '/hello')])
        elif p == '/404':
            self.reply(404, 'nope: not found page body', 'text/plain')
        elif p == '/gzip':
            self.reply(200, gzip.compress(b'compressed hello ' * 100), 'text/plain', extra=[('Content-Encoding', 'gzip')])
        elif p == '/cookies':
            self.reply(200, 'c', extra=[('Set-Cookie', 'a=1; Path=/'), ('Set-Cookie', 'b=2; Path=/'), ('X-Dup', 'one'), ('X-Dup', 'two')])
        elif p == '/head':
            self.reply(200, 'xx', extra=[('X-Thing', 'yes')])
        else:
            self.reply(404, 'unknown ' + p)

    do_GET = do_POST = do_PUT = do_DELETE = do_PATCH = do_HEAD = do_ANY


def serve_http(port, tls):
    srv = ThreadingHTTPServer(('127.0.0.1', port), H)
    srv.daemon_threads = True
    if tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(os.path.join(HERE, 'cert.pem'), os.path.join(HERE, 'key.pem'))
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    srv.serve_forever()


# ---- a small WebSocket server ----
GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


async def read_frame(r):
    h = await r.readexactly(2)
    fin = h[0] & 0x80
    op = h[0] & 0x0F
    masked = h[1] & 0x80
    n = h[1] & 0x7F
    if n == 126:
        n = struct.unpack('>H', await r.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack('>Q', await r.readexactly(8))[0]
    mask = await r.readexactly(4) if masked else b''
    data = await r.readexactly(n)
    if masked:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return fin, op, data


def frame(op, data=b'', fin=True):
    head = bytes([(0x80 if fin else 0) | op])
    n = len(data)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack('>H', n)
    else:
        head += bytes([127]) + struct.pack('>Q', n)
    return head + data


async def ws_client(r, w):
    try:
        req = await r.readuntil(b'\r\n\r\n')
        lines = req.decode().split('\r\n')
        headers = {}
        for ln in lines[1:]:
            if ':' in ln:
                k, v = ln.split(':', 1)
                headers[k.strip().lower()] = v.strip()
        path = lines[0].split(' ')[1]
        if path.startswith('/forbidden'):
            w.write(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n')
            await w.drain()
            w.close()
            return
        key = headers.get('sec-websocket-key', '')
        acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        proto = ''
        if 'sec-websocket-protocol' in headers:
            proto = 'Sec-WebSocket-Protocol: ' + headers['sec-websocket-protocol'].split(',')[0].strip() + '\r\n'
        w.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n%s\r\n' % (acc, proto)).encode())
        await w.drain()
        w.write(frame(1, ('hello from server; auth=' + headers.get('authorization', '-') + '; x=' + headers.get('x-test', '-')).encode()))
        await w.drain()
        buf = b''
        while True:
            fin, op, data = await read_frame(r)
            if op == 8:
                code = struct.unpack('>H', data[:2])[0] if len(data) >= 2 else 1005
                w.write(frame(8, data))
                await w.drain()
                print('server: client closed', code, data[2:], flush=True)
                break
            if op == 9:
                w.write(frame(10, data))
                await w.drain()
                continue
            if op in (1, 2, 0):
                buf += data
                if not fin:
                    continue
                msg, buf = buf, b''
                if op == 1 or True:
                    text = msg.decode('latin1') if op != 1 else msg.decode()
                if msg == b'close':
                    w.write(frame(8, struct.pack('>H', 4001) + b'bye'))
                    await w.drain()
                    # wait for the client's close frame
                    try:
                        await asyncio.wait_for(read_frame(r), 3)
                    except Exception:
                        pass
                    break
                if msg == b'drop':
                    w.close()
                    return
                if msg == b'big':
                    w.write(frame(1, b'B' * 300000))
                    await w.drain()
                    continue
                if msg == b'burst':
                    for i in range(500):
                        w.write(frame(1, ('burst %d' % i).encode()))
                    await w.drain()
                    continue
                if msg == b'idle':
                    await asyncio.sleep(0.1)
                    continue
                w.write(frame(1 if op == 1 else 2, msg))
                await w.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        try:
            w.close()
        except Exception:
            pass


async def ws_main():
    srv = await asyncio.start_server(ws_client, '127.0.0.1', 18081)
    async with srv:
        await srv.serve_forever()


if __name__ == '__main__':
    threading.Thread(target=serve_http, args=(18080, False), daemon=True).start()
    if os.path.exists(os.path.join(HERE, 'cert.pem')):
        threading.Thread(target=serve_http, args=(18443, True), daemon=True).start()
    print('servers up', flush=True)
    asyncio.run(ws_main())

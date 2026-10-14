#!/usr/bin/env python3
"""TLS terminator: accepts TLS on :8883, pipes plaintext to 127.0.0.1:1883.

Pure byte pipe. Logs every chunk and parses the MQTT fixed header of
client->broker bytes (handles split and coalesced packets).
"""
import argparse
import asyncio
import ssl
import sys
import time
from datetime import datetime

TYPES = {1: "CONNECT", 2: "CONNACK", 3: "PUBLISH", 4: "PUBACK", 5: "PUBREC",
         6: "PUBREL", 7: "PUBCOMP", 8: "SUBSCRIBE", 9: "SUBACK",
         10: "UNSUBSCRIBE", 11: "UNSUBACK", 12: "PINGREQ", 13: "PINGRESP",
         14: "DISCONNECT"}


def ts() -> str:
    return datetime.now().strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3]


def log(msg: str) -> None:
    print(f"{ts()} {msg}", flush=True)


class MqttParser:
    """Incremental MQTT fixed-header parser; emits (type_name, total_len)."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.skip = 0  # remaining body bytes of the current packet

    def feed(self, data: bytes) -> list[str]:
        out: list[str] = []
        self.buf += data
        while True:
            if self.skip:
                n = min(self.skip, len(self.buf))
                del self.buf[:n]
                self.skip -= n
                if self.skip:
                    break
            if len(self.buf) < 2:
                break
            mult, rem, i = 1, 0, 1
            while True:
                if i >= len(self.buf):
                    return out  # need more length bytes
                b = self.buf[i]
                rem += (b & 0x7F) * mult
                i += 1
                if not b & 0x80:
                    break
                mult *= 128
                if mult > 128 ** 3:
                    out.append("MALFORMED remaining-length")
                    self.buf.clear()
                    return out
            first = self.buf[0]
            ptype = first >> 4
            name = TYPES.get(ptype, f"RESERVED({ptype})")
            extra = ""
            if ptype == 1:
                # CONNECT: proto name len(2)+name+level+flags+keepalive(2)
                try:
                    nl = int.from_bytes(self.buf[i:i + 2], "big")
                    k = i + 2 + nl
                    if len(self.buf) >= k + 4:
                        ka = int.from_bytes(self.buf[k + 2:k + 4], "big")
                        extra = f" keepalive={ka}s"
                except Exception:
                    pass
            out.append(f"{name} flags=0x{first & 0xF:x} remlen={rem}{extra}")
            del self.buf[:i]
            self.skip = rem
        return out


async def pump(src, dst, conn: str, direction: str, parse: bool) -> None:
    parser = MqttParser() if parse else None
    try:
        while True:
            data = await src.read(65536)
            if not data:
                log(f"{conn} {direction} EOF")
                break
            msg = f"{conn} {direction} {len(data)} bytes"
            if parser:
                pk = parser.feed(data)
                if pk:
                    msg += " | MQTT: " + "; ".join(pk)
            log(msg)
            dst.write(data)
            await dst.drain()
    except (ConnectionError, ssl.SSLError, OSError) as e:
        log(f"{conn} {direction} ERROR {type(e).__name__}: {e}")
    finally:
        try:
            dst.close()
        except Exception:
            pass


def make_ctx(cert: str, key: str, keylog: str) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(cert, key)
    try:
        ctx.set_ciphers("ALL:@SECLEVEL=0")  # permissive: RSA/ECDHE, GCM/CBC
    except ssl.SSLError:
        ctx.set_ciphers("ALL")
    ctx.keylog_filename = keylog
    return ctx


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--keylog", required=True)
    ap.add_argument("--listen", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--backend-host", default="127.0.0.1")
    ap.add_argument("--backend-port", type=int, default=1883)
    a = ap.parse_args()
    ctx = make_ctx(a.cert, a.key, a.keylog)
    n = 0

    async def handle(r, w):
        nonlocal n
        n += 1
        conn = f"c{n}"
        peer = w.get_extra_info("peername")
        sslobj = w.get_extra_info("ssl_object")
        log(f"{conn} TLS OK peer={peer} version={sslobj.version()} "
            f"cipher={sslobj.cipher()} sni={getattr(sslobj, 'server_hostname', None)}")
        try:
            br, bw = await asyncio.open_connection(a.backend_host, a.backend_port)
        except OSError as e:
            log(f"{conn} backend connect failed: {e}")
            w.close()
            return
        await asyncio.gather(
            pump(r, bw, conn, "dev->broker", True),
            pump(br, w, conn, "broker->dev", False),
        )
        log(f"{conn} closed")

    # Handshake failures: asyncio reports them via the loop exception handler.
    loop = asyncio.get_running_loop()

    def on_exc(_loop, context):
        exc = context.get("exception")
        if isinstance(exc, ssl.SSLError) or "SSL" in str(context.get("message", "")):
            log(f"TLS HANDSHAKE FAILED: {context.get('message')} | {exc!r} "
                f"(reason={getattr(exc, 'reason', None)})")
        else:
            log(f"loop error: {context.get('message')} {exc!r}")

    loop.set_exception_handler(on_exc)

    # SNI/offered-ciphers logging for failures too
    def sni_cb(sslobj, name, _ctx):
        log(f"ClientHello SNI={name!r}")
    ctx.sni_callback = sni_cb

    srv = await asyncio.start_server(handle, a.listen, a.port, ssl=ctx,
                                     ssl_handshake_timeout=15)
    log(f"listening on {a.listen}:{a.port} -> {a.backend_host}:{a.backend_port}")
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

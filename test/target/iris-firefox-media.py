#!/usr/bin/env python3
"""Minimal WebDriver BiDi client: media-surface frame/drop evidence from Firefox.

Firefox has no CDP; its remote agent speaks BiDi at ws://127.0.0.1:PORT/session.
This reads getVideoPlaybackQuality() from the page's video element twice over a
window. It never claims hardware by itself: MOZ_LOG and the driver trace do that.
"""
import base64
import json
import os
import socket
import struct
import sys
import time

STATS = r"""(function(){
  var vids=[];
  function walk(r){
    if(!r || !r.querySelectorAll) return;
    Array.prototype.forEach.call(r.querySelectorAll('video'), function(v){vids.push(v);});
    Array.prototype.forEach.call(r.querySelectorAll('*'), function(e){if(e.shadowRoot) walk(e.shadowRoot);});
  }
  walk(document);
  var v=vids.filter(function(x){return x.getVideoPlaybackQuality && x.getVideoPlaybackQuality().totalVideoFrames>0;})[0]||vids[0];
  if(!v) return JSON.stringify({n:0,href:location.href,title:document.title});
  var q=v.getVideoPlaybackQuality?v.getVideoPlaybackQuality():null;
  return JSON.stringify({n:vids.length,href:location.href,title:document.title,
    decoded:q?q.totalVideoFrames:null,dropped:q?q.droppedVideoFrames:null,
    w:v.videoWidth,h:v.videoHeight,paused:v.paused,ct:v.currentTime,
    ua:navigator.userAgent,ready:v.readyState});
})()"""


class BiDi:
    def __init__(self, port, timeout=30.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET /session HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("bidi closed during handshake")
            buf += chunk
        if b"101" not in buf.split(b"\r\n", 1)[0]:
            raise RuntimeError(f"handshake refused: {buf.splitlines()[0]!r}")
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
        self.next_id = 0
        self.sock.settimeout(timeout)

    def _need(self, count):
        while len(self.buf) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("bidi closed")
            self.buf += chunk

    def _frame(self):
        while True:
            self._need(2)
            first, second = self.buf[0], self.buf[1]
            length = second & 0x7F
            off = 2
            if length == 126:
                self._need(4)
                length = struct.unpack(">H", self.buf[2:4])[0]
                off = 4
            elif length == 127:
                self._need(10)
                length = struct.unpack(">Q", self.buf[2:10])[0]
                off = 10
            self._need(off + length)
            payload = self.buf[off : off + length]
            self.buf = self.buf[off + length :]
            if first & 0x0F == 1:
                return json.loads(payload.decode())
            if first & 0x0F == 8:
                raise RuntimeError("bidi sent close")

    def call(self, method, params=None, timeout=30.0):
        self.next_id += 1
        call_id = self.next_id
        msg = json.dumps({"id": call_id, "method": method, "params": params or {}}).encode()
        header = b"\x81"
        mask = os.urandom(4)
        n = len(msg)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(msg)))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"{method} timed out")
            self.sock.settimeout(remaining)
            reply = self._frame()
            if reply.get("id") != call_id:
                continue
            if reply.get("type") == "error":
                raise RuntimeError(f"{method} failed: {reply}")
            return reply.get("result", reply)

    def evaluate(self, expression, context, timeout=20.0):
        result = self.call(
            "script.evaluate",
            {
                "expression": expression,
                "target": {"context": context},
                "awaitPromise": True,
                "resultOwnership": "none",
            },
            timeout=timeout,
        )
        if result.get("type") == "exception":
            raise RuntimeError(f"js failed: {result.get('exceptionDetails')}")
        remote = result.get("result", {})
        if remote.get("type") == "string":
            return json.loads(remote["value"])
        return remote


def main():
    port, seconds, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
    b = BiDi(port)
    session = b.call("session.new", {"capabilities": {}}, timeout=30)
    version = session.get("capabilities", {}).get("browserVersion")
    tree = b.call("browsingContext.getTree", {}, timeout=20)
    contexts = tree.get("contexts", [])
    chosen = contexts[0] if contexts else None
    for ctx in contexts:
        url = ctx.get("url", "")
        if "youtube" in url or "bilibili" in url or "playback.html" in url:
            chosen = ctx
            break
    if not chosen:
        raise RuntimeError("no browsing context")
    context = chosen["context"]
    first = b.evaluate(STATS, context)
    time.sleep(seconds)
    second = b.evaluate(STATS, context)
    report = {
        "browser_version": version,
        "context_url": chosen.get("url"),
        "stats_first": first,
        "stats_second": second,
    }
    if isinstance(first.get("decoded"), int) and isinstance(second.get("decoded"), int):
        report["window_decoded_frames"] = second["decoded"] - first["decoded"]
        report["window_dropped_frames"] = (second.get("dropped") or 0) - (first.get("dropped") or 0)
    with open(out, "w") as f:
        json.dump(report, f, indent=2, sort_keys=True)
        f.write("\n")
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

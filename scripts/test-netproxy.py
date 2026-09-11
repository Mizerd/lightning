#!/usr/bin/env python3
"""A CONNECT proxy that can be throttled or cut, for ONE app.

Two live tests were blocked on "I cannot stage this without root":
  * the send-queue wedge needs a mid-send NETWORK FAILURE
  * the upload-progress percentage needs an upload slow enough to sample

Both are reachable without touching the machine's networking: reqwest (which
matrix-sdk uses) honours HTTPS_PROXY, so pointing ONLY Lightning at this
process gives a network whose behaviour is under test control. It tunnels
bytes for CONNECT and never inspects TLS, so nothing is decrypted here.

  netproxy.py --port 8888                  plain tunnel
  netproxy.py --port 8888 --kbytes 200     throttle every tunnel
  touch <ctl>/cut                          drop all tunnels and refuse new
  rm <ctl>/cut                             resume
"""
import argparse, os, select, socket, threading, time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8888)
# KIBIBYTES PER SECOND, per direction, per tunnel — not kilobits, and not an
# aggregate. Named for what it does: the earlier `--kbps` overstated the rate
# by eight and hid that two concurrent tunnels get twice the budget.
ap.add_argument("--kbytes", type=int, default=0,
                help="KiB/s per direction per tunnel; 0 = unthrottled")
ap.add_argument("--ctl", default="/tmp/netproxy-ctl")
args = ap.parse_args()
os.makedirs(args.ctl, exist_ok=True)
CUT = os.path.join(args.ctl, "cut")
live = []

def pump(a, b, budget):
    """Copy a->b, honouring the throttle and the cut file."""
    while True:
        if os.path.exists(CUT):
            return
        r, _, _ = select.select([a], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = a.recv(budget or 65536)
        except OSError:
            return
        if not chunk:
            return
        try:
            b.sendall(chunk)
        except OSError:
            return
        if budget:
            time.sleep(len(chunk) / (budget * 1.0))

def handle(cli):
    try:
        req = b""
        while b"\r\n\r\n" not in req:
            part = cli.recv(4096)
            if not part:
                return
            req += part
        line = req.split(b"\r\n")[0].decode("latin1")
        verb, target = line.split()[0], line.split()[1]
        if verb.upper() != "CONNECT":
            cli.sendall(b"HTTP/1.1 405 Method Not Allowed\r\n\r\n")
            return
        if os.path.exists(CUT):
            cli.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
            return
        host, _, port = target.partition(":")
        up = socket.create_connection((host, int(port or 443)), timeout=10)
        cli.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        live.extend([cli, up])
        budget = int(args.kbytes * 1024) if args.kbytes else 0
        t = threading.Thread(target=pump, args=(up, cli, budget), daemon=True)
        t.start()
        pump(cli, up, budget)
        try: up.close()
        except OSError: pass
    except Exception:
        pass
    finally:
        try: cli.close()
        except OSError: pass

def watch_cut():
    """A cut must break tunnels that are ALREADY open, or an in-flight
    request just keeps going and the test proves nothing."""
    seen = False
    while True:
        now = os.path.exists(CUT)
        if now and not seen:
            for s in list(live):
                try: s.shutdown(socket.SHUT_RDWR)
                except OSError: pass
            live.clear()
        seen = now
        time.sleep(0.2)

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", args.port))
srv.listen(64)
threading.Thread(target=watch_cut, daemon=True).start()
print(f"proxy on 127.0.0.1:{args.port} kbytes={args.kbytes or 'unlimited'} ctl={args.ctl}", flush=True)
while True:
    c, _ = srv.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()

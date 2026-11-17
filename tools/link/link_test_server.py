#!/usr/bin/env python3
"""Vyrn Link test server: a stand-in for Vyrn, so you can build and test a game mod without the app.

It accepts one game at a time, checks its hello and pairing code, shows what the game sends, and sends the AI's
"input" messages in real-time or lockstep mode. Protocol: docs/user/vyrn-link.md

Type a letter + Enter while it runs:
  j  toggle holding jump      t  tap jump      r  send {"type":"restart"}      s  send {"type":"stop"}
  p  print the full latest state                q  quit
"""
import argparse
import json
import random
import socket
import sys
import threading
import time

from wslite import WSClosed, WSError, server_handshake

PROTOCOL = 1


def lan_addresses():
    addrs = set()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))  # no packet is sent; this only picks the outgoing interface
            addrs.add(s.getsockname()[0])
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except OSError:
        pass
    addrs.discard("127.0.1.1")
    return sorted(addrs) or ["127.0.0.1"]


def summarize(state, width=110):
    parts = []
    for k, v in state.items():
        if k == "type":
            continue
        if isinstance(v, float):
            v = round(v, 2)
        if isinstance(v, list) and len(v) > 6:
            v = str(v[:6])[:-1] + ", …]"
        parts.append(f"{k}={v}")
    text = " ".join(parts)
    return text if len(text) <= width else text[: width - 1] + "…"


class Controls:
    """What the person at the terminal (or the --policy) wants the AI to do."""

    def __init__(self, button, policy):
        self.lock = threading.Lock()
        self.button = button
        self.policy = policy
        self.holding = policy == "hold"
        self.taps = 0
        self.outbox = []
        self.quit = False
        self.decisions = 0
        self.print_state = False

    def decide(self):
        """Called once per AI decision: returns (hold list, tap list, extra messages)."""
        with self.lock:
            self.decisions += 1
            tap = []
            if self.taps:
                self.taps -= 1
                tap = [self.button]
            if self.policy.startswith("every:"):
                n = max(1, int(self.policy.split(":", 1)[1]))
                if self.decisions % n == 0:
                    tap = [self.button]
            elif self.policy == "random":
                self.holding = random.random() < 0.2
            hold = [self.button] if self.holding else []
            out, self.outbox = self.outbox, []
            return hold, tap, out


def stdin_loop(ctl: Controls):
    for line in sys.stdin:
        cmd = line.strip().lower()[:1]
        with ctl.lock:
            if cmd == "j":
                ctl.holding = not ctl.holding
                print(f"[you] {'holding' if ctl.holding else 'released'} {ctl.button}", flush=True)
            elif cmd == "t":
                ctl.taps += 1
            elif cmd == "r":
                ctl.outbox.append({"type": "restart"})
            elif cmd == "s":
                ctl.outbox.append({"type": "stop"})
            elif cmd == "p":
                ctl.print_state = True
            elif cmd == "q":
                ctl.quit = True
                return
    # stdin closed (e.g. running in the background): keep serving, just without typed commands.


def serve_one(ws, args, ctl: Controls):
    peer = ws.peer()
    local = peer[0] in ("127.0.0.1", "::1")
    ws.sock.settimeout(10)
    hello = ws.recv_json()
    ws.sock.settimeout(None)
    if not isinstance(hello, dict) or hello.get("type") != "hello":
        ws.send_json({"type": "error", "code": "protocol", "message": 'the first message must be {"type":"hello",...}'})
        return
    if hello.get("protocol", PROTOCOL) != PROTOCOL:
        ws.send_json({"type": "error", "code": "protocol", "message": f"this server speaks protocol {PROTOCOL}"})
        return
    if not local and str(hello.get("code", "")) != args.code:
        ws.send_json({"type": "error", "code": "bad_code", "message": "wrong pairing code"})
        print(f"[link] refused {peer[0]}: wrong pairing code {hello.get('code')!r}", flush=True)
        return
    ws.send_json({"type": "welcome", "protocol": PROTOCOL, "vyrn": "test-server", "project": "link test",
                  "buttons": [args.button], "lockstep": args.lockstep, "frames_per_step": args.frames_per_step,
                  "tick_hz": args.tick_hz})
    print(f"[link] connected: {hello.get('game', '?')} ({hello.get('client', '?')}) from {peer[0]}"
          f" — {'lockstep' if args.lockstep else f'real time at {args.tick_hz} decisions/s'}", flush=True)

    latest = {"state": None, "count": 0}
    lock = threading.Lock()
    done = threading.Event()

    def send_input(frame):
        hold, tap, extra = ctl.decide()
        for msg in extra:
            ws.send_json(msg)
            print(f"[sent] {json.dumps(msg)}", flush=True)
        ws.send_json({"type": "input", "frame": frame, "hold": hold, "tap": tap})

    def reader():
        try:
            while not done.is_set():
                msg = ws.recv_json()
                if msg is None:
                    break
                kind = msg.get("type") if isinstance(msg, dict) else None
                if kind == "state":
                    with lock:
                        latest["state"], latest["count"] = msg, latest["count"] + 1
                    if args.lockstep:
                        send_input(msg.get("frame"))
                elif kind == "event":
                    print(f"[event] {summarize(msg)}", flush=True)
                    if args.auto_restart and msg.get("name") in ("death", "complete"):
                        with ctl.lock:
                            ctl.outbox.append({"type": "restart"})
                else:
                    print(f"[game] {json.dumps(msg)[:200]}", flush=True)
        except (WSClosed, WSError, OSError, ValueError) as e:
            print(f"[link] connection ended: {e}", flush=True)
        finally:
            done.set()

    threading.Thread(target=reader, daemon=True).start()
    last_print, last_count = time.monotonic(), 0
    period = 1.0 / args.tick_hz
    next_tick = time.monotonic()
    try:
        while not done.is_set() and not ctl.quit:
            now = time.monotonic()
            if not args.lockstep and now >= next_tick:
                with lock:
                    state = latest["state"]
                if state is not None:
                    send_input(state.get("frame"))
                next_tick += period
                if now - next_tick > 1.0:  # we fell far behind: skip, don't burst
                    next_tick = now + period
            if now - last_print >= 1.0:
                with lock:
                    state, count = latest["state"], latest["count"]
                rate = (count - last_count) / (now - last_print)
                last_print, last_count = now, count
                if state is not None:
                    hold = "holding" if ctl.holding else "released"
                    print(f"[state {rate:5.1f}/s, {hold}] {summarize(state)}", flush=True)
                else:
                    print("[link] connected, waiting for the first state…", flush=True)
            with ctl.lock:
                if ctl.print_state:
                    ctl.print_state = False
                    print(json.dumps(latest["state"], indent=1), flush=True)
            if args.lockstep:
                with ctl.lock:
                    pending = list(ctl.outbox)
                    ctl.outbox.clear()
                for msg in pending:  # e.g. restart/stop typed while the game waits for input
                    ws.send_json(msg)
                    print(f"[sent] {json.dumps(msg)}", flush=True)
            time.sleep(min(0.005, period / 4))
    finally:
        done.set()
        try:
            ws.send_json({"type": "stop"})
        except OSError:
            pass
        ws.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=7457)
    ap.add_argument("--code", default="".join(random.choice("0123456789") for _ in range(6)),
                    help="6-digit pairing code the game must send (not needed from 127.0.0.1)")
    ap.add_argument("--button", default="jump")
    ap.add_argument("--tick-hz", type=float, default=30.0, help="AI decisions per second in real-time mode")
    ap.add_argument("--lockstep", action="store_true", help="the game waits for an input after every state")
    ap.add_argument("--frames-per-step", type=int, default=1)
    ap.add_argument("--no-auto-restart", dest="auto_restart", action="store_false",
                    help="don't send restart after a death/complete event")
    ap.add_argument("--policy", default="manual", help="manual (default), hold, random, or every:N (tap every N decisions)")
    args = ap.parse_args()

    ctl = Controls(args.button, args.policy)
    srv = socket.create_server(("0.0.0.0", args.port), reuse_port=False)
    srv.settimeout(0.5)
    urls = ", ".join(f"ws://{a}:{args.port}/" for a in lan_addresses())
    print(f"Vyrn Link test server — connect your game to {urls} (or ws://127.0.0.1:{args.port}/ here)")
    print(f"Pairing code: {args.code}   Mode: {'lockstep' if args.lockstep else 'real time'}   "
          f"Commands: j hold/release, t tap, r restart, s stop, p print state, q quit", flush=True)
    threading.Thread(target=stdin_loop, args=(ctl,), daemon=True).start()
    try:
        while not ctl.quit:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            try:
                ws = server_handshake(conn)
            except (WSError, WSClosed, OSError) as e:
                print(f"[link] bad connection attempt: {e}", flush=True)
                conn.close()
                continue
            try:
                serve_one(ws, args, ctl)
            except (WSClosed, WSError, OSError, ValueError) as e:
                print(f"[link] connection ended: {e}", flush=True)
            finally:
                ws.close()
            print("[link] waiting for the game to connect…", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()

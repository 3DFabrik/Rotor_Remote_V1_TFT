"""Move the rotor over rotctld and print the angle error after each stop.

Default: from the current heading, turn +step degrees, then return.
Absolute targets: --targets 0,90,180,270
"""

import argparse
import socket
import sys
import time


def wrap360(deg):
    return deg % 360.0


def signed_error(actual, target):
    return (actual - target + 180.0) % 360.0 - 180.0


class Rotctl:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _readline(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(256)
            if not chunk:
                raise ConnectionError("rotctld closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("ascii", "replace").strip()

    def command(self, line, replies):
        self.sock.sendall((line + "\n").encode("ascii"))
        lines = []
        for _ in range(replies):
            lines.append(self._readline())
        return lines

    def position(self):
        az, _el = self.command("p", 2)
        return float(az)

    def goto(self, az):
        reply = self.command("P %.1f 0" % wrap360(az), 1)
        if not reply or not reply[0].startswith("RPRT 0"):
            raise RuntimeError("set_pos failed: %s" % reply)

    def stop(self):
        try:
            self.command("S", 1)
        except OSError:
            pass


def wait_until_settled(rot, timeout, stable_s, epsilon):
    deadline = time.time() + timeout
    last = None
    stable_since = None
    samples = []
    while time.time() < deadline:
        az = rot.position()
        samples.append(az)
        now = time.time()
        if last is None or abs(signed_error(az, last)) > epsilon:
            stable_since = now
        elif stable_since is not None and now - stable_since >= stable_s:
            return az
        last = az
        print("  %6.1f deg" % az, end="\r", flush=True)
        time.sleep(0.4)
    print()
    raise TimeoutError("rotor did not settle (last %s)" % (samples[-1] if samples else "?"))


def run_move(rot, target, timeout, stable_s, epsilon):
    before = rot.position()
    print("goto %6.1f   (now %6.1f)" % (target, before))
    rot.goto(target)
    actual = wait_until_settled(rot, timeout, stable_s, epsilon)
    err = signed_error(actual, target)
    print("  target %6.1f   actual %6.1f   error %+6.1f" % (target, actual, err))
    return target, actual, err


def main():
    parser = argparse.ArgumentParser(description="Move the rotor and report angle error")
    parser.add_argument("--host", default="192.168.1.77")
    parser.add_argument("--port", type=int, default=4533)
    parser.add_argument("--step", type=float, default=40.0, help="relative move, then return")
    parser.add_argument("--targets", default="", help="absolute degrees, comma separated")
    parser.add_argument("--timeout", type=float, default=90.0, help="seconds per move")
    parser.add_argument("--stable", type=float, default=3.0, help="seconds without motion")
    parser.add_argument("--epsilon", type=float, default=0.6, help="degrees counted as standing")
    args = parser.parse_args()

    rot = Rotctl(args.host, args.port, timeout=5.0)
    results = []
    try:
        start = rot.position()
        print("connected %s:%d   start %6.1f deg" % (args.host, args.port, start))
        if args.targets.strip():
            targets = [wrap360(float(part)) for part in args.targets.split(",") if part.strip()]
        else:
            targets = [wrap360(start + args.step), wrap360(start)]
        for target in targets:
            results.append(run_move(rot, target, args.timeout, args.stable, args.epsilon))
    except KeyboardInterrupt:
        print("\nstopping")
        rot.stop()
        return 1
    finally:
        rot.close()

    print()
    print("%8s %8s %8s" % ("target", "actual", "error"))
    worst = 0.0
    for target, actual, err in results:
        print("%8.1f %8.1f %+8.1f" % (target, actual, err))
        worst = max(worst, abs(err))
    print("max |error| %5.1f deg" % worst)
    return 0


if __name__ == "__main__":
    sys.exit(main())

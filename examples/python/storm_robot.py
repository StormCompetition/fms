#!/usr/bin/env python3
"""Example STORM FMS robot client, protocol version 1. Python 3.8+, standard library only.

This is a starting point, not a robot. It connects to the FMS, authenticates, sends
heartbeats, and tells your code when the robot may and may not move. Put your own
motor code in Robot.on_motion (and, if you like, on_phase and on_score).

The safety rules from docs/protocol.md that this client implements:

  * Start disabled. Nothing may move until the FMS enables the robot.
  * Loss of Signal (rule R7). No server message for heartbeat_timeout_ms (capped at
    1 s) means the link is lost: halt.
  * Disabled means stopped (rule C17), and an authorization reason this client does
    not know is treated as disabled.

Usage:

    STORM_TOKEN=<64 hex chars> python3 storm_robot.py --host 10.0.0.2 --team 12

Each change prints one line, for example "motion: enabled (phase)" or
"motion: disabled (loss_of_signal)".
"""

import argparse
import os
import select
import signal
import socket
import struct
import sys
import time

CLIENT_NAME = "storm-example-python"
CLIENT_VERSION = "1.0"

# Framing (docs/protocol.md, Framing).
MAGIC = b"STRM"
VERSION = 1
HEADER = struct.Struct(">4sBBHHH")  # magic, version, msg_type, flags, payload_len, sequence
MAX_PAYLOAD = 1024

# Message types.
HELLO, HEARTBEAT, GOODBYE = 0x01, 0x02, 0x03
WELCOME, REJECT, MATCH_INFO, PHASE_CHANGE = 0x81, 0x82, 0x83, 0x84
MOVE_AUTH, SCORE_UPDATE, STATUS, ERROR = 0x85, 0x86, 0x87, 0x8F

PHASES = {0: "IDLE", 1: "LOADED", 2: "PRE_MATCH", 3: "AUTONOMOUS", 4: "REF_PAUSE",
          5: "TELEOP", 6: "ENDED", 7: "FINALIZED", 8: "ABORTED"}
# Authorization reasons. 0 (unknown) and anything not listed here mean disabled.
AUTH_REASONS = {1: "phase", 2: "estop", 3: "admin_disable", 4: "admin_enable",
                5: "loss_of_signal", 6: "disqualified", 7: "field_fault", 8: "no_match"}
REJECT_REASONS = {1: "unknown_team", 2: "inactive_team", 3: "bad_token",
                  4: "ip_not_allowed", 5: "bad_version", 6: "malformed"}
ERROR_CODES = {1: "unexpected_message", 2: "unknown_type", 3: "malformed",
               4: "replaced", 5: "revoked"}
ALLIANCES = {1: "red", 2: "blue"}

ROBOT_HALTED, ROBOT_MOVING_CAPABLE = 0, 1
GOODBYE_SHUTTING_DOWN = 1

LOS_CAP_S = 1.0          # halt within 1 s of the last server message, whatever the config
RECONNECT_AFTER_S = 3.0  # after this much silence, drop the connection and dial again
CONNECT_TIMEOUT_S = 2.0
SEND_TIMEOUT_S = 1.0


class Malformed(Exception):
    """The server sent something that cannot be decoded. Close and reconnect."""


class Reader:
    """Reads fields from one payload. Running out of bytes is malformed. Bytes after
    the last field a message defines are ignored (later revisions may append fields)."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise Malformed("payload too short")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack(">H", self.take(2))[0]

    def u32(self):
        return struct.unpack(">I", self.take(4))[0]

    def bool(self):
        v = self.u8()
        if v > 1:
            raise Malformed("boolean field is %d" % v)
        return v == 1

    def string(self):
        try:
            return self.take(self.u8()).decode("utf-8")
        except UnicodeDecodeError:
            raise Malformed("invalid UTF-8")

    def bytes(self):
        return self.take(self.u16())


def encode_str(s):
    b = s.encode("utf-8")
    if len(b) > 255:
        raise ValueError("string longer than 255 bytes")
    return bytes([len(b)]) + b


def frame(msg_type, payload, seq):
    return HEADER.pack(MAGIC, VERSION, msg_type, 0, len(payload), seq) + payload


class Robot:
    """One robot. Override the on_* methods, or edit them, to connect your hardware."""

    def __init__(self, host, port, team, token):
        self.host, self.port, self.team, self.token = host, port, team, token
        self.sock = None
        self.seq = 0
        self.buf = b""
        self.enabled = False
        self.reason = "starting"
        self.started = time.monotonic()
        self.stopping = False

    # ---- Hooks for your code -------------------------------------------------

    def on_motion(self, enabled, reason):
        """Called whenever the robot's permission to move changes. When enabled is
        False, stop every actuator now. This must not block."""
        print("motion: %s (%s)" % ("enabled" if enabled else "disabled", reason), flush=True)

    def on_phase(self, phase, duration_ms, remaining_ms):
        """PHASE_CHANGE. The change to AUTONOMOUS may start an autonomous routine, but
        the routine may only drive while on_motion says enabled."""
        print("phase: %s %.1f s" % (phase, remaining_ms / 1000), flush=True)

    def on_score(self, red, blue):
        print("score: red %d blue %d" % (red, blue), flush=True)

    # ---- Authorization -------------------------------------------------------

    def set_motion(self, enabled, reason):
        if (enabled, reason) != (self.enabled, self.reason):
            self.enabled, self.reason = enabled, reason
            self.on_motion(enabled, reason)

    def authorize(self, enabled, code):
        name = AUTH_REASONS.get(code)
        if name is None:
            # Rule C17: an unrecognized reason is disabled, whatever `enabled` says.
            self.set_motion(False, "unknown(%d)" % code)
        else:
            self.set_motion(enabled, name)

    # ---- Connection ----------------------------------------------------------

    def send(self, msg_type, payload):
        self.sock.sendall(frame(msg_type, payload, self.seq))
        self.seq = (self.seq + 1) & 0xFFFF

    def send_heartbeat(self):
        uptime = int((time.monotonic() - self.started) * 1000) & 0xFFFFFFFF
        state = ROBOT_MOVING_CAPABLE if self.enabled else ROBOT_HALTED
        self.send(HEARTBEAT, struct.pack(">IB", uptime, state))

    def close(self, goodbye=False):
        if self.sock is None:
            return
        if goodbye:
            try:
                self.send(GOODBYE, bytes([GOODBYE_SHUTTING_DOWN]))
            except OSError:
                pass
        self.sock.close()
        self.sock = None

    def run(self):
        """Connect, and reconnect after any failure, until stop() is called."""
        backoff = 0.5
        while not self.stopping:
            welcomed = False
            try:
                welcomed = self.session()
            except (OSError, Malformed) as e:
                print("connection: %s" % e, flush=True)
            finally:
                # Whatever happened, a robot without a connection is disabled.
                self.set_motion(False, "disconnected")
                self.close(goodbye=self.stopping)
            if welcomed:
                backoff = 0.5
            wake = time.monotonic() + backoff
            while not self.stopping and time.monotonic() < wake:
                time.sleep(0.05)
            backoff = min(backoff * 2, 5.0)

    def stop(self):
        self.stopping = True

    def session(self):
        """One connection. Returns whether the server sent WELCOME."""
        self.sock = socket.create_connection((self.host, self.port), timeout=CONNECT_TIMEOUT_S)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(SEND_TIMEOUT_S)
        self.seq, self.buf = 0, b""
        self.send(HELLO, struct.pack(">H", self.team) + encode_str(self.token)
                  + encode_str(CLIENT_NAME) + encode_str(CLIENT_VERSION))

        self.welcomed = False
        self.hb_interval = 0.1
        self.los_timeout = LOS_CAP_S
        self.last_rx = time.monotonic()
        next_hb = None  # heartbeats start at WELCOME
        while not self.stopping:
            now = time.monotonic()
            silent = now - self.last_rx
            if silent >= self.los_timeout:
                self.set_motion(False, "loss_of_signal")  # rule R7
                if silent >= RECONNECT_AFTER_S:
                    print("connection: no message for %.1f s, reconnecting" % silent, flush=True)
                    return self.welcomed
            if next_hb is None and self.welcomed:
                next_hb = now
            if next_hb is not None and now >= next_hb:
                self.send_heartbeat()
                next_hb = max(next_hb + self.hb_interval, now)

            # Sleep until data arrives or the next heartbeat or Loss of Signal deadline.
            deadline = self.last_rx + (self.los_timeout if silent < self.los_timeout
                                       else RECONNECT_AFTER_S)
            if next_hb is not None:
                deadline = min(deadline, next_hb)
            try:
                ready, _, _ = select.select([self.sock], [], [], max(0.0, deadline - now))
            except InterruptedError:
                continue
            if not ready:
                continue
            data = self.sock.recv(4096)
            if not data:
                print("connection: closed by server", flush=True)
                return self.welcomed
            self.buf += data
            if not self.process():
                return self.welcomed
        return self.welcomed

    def process(self):
        """Handles every complete message in the buffer. Returns False when the
        connection must close."""
        while len(self.buf) >= HEADER.size:
            magic, version, msg_type, _flags, length, _seq = HEADER.unpack_from(self.buf)
            if magic != MAGIC or length > MAX_PAYLOAD:
                # Out of frame. Do not try to resync.
                raise Malformed("bad frame header")
            if len(self.buf) < HEADER.size + length:
                return True
            payload = self.buf[HEADER.size:HEADER.size + length]
            self.buf = self.buf[HEADER.size + length:]
            if version != VERSION:
                raise Malformed("protocol version %d" % version)
            self.last_rx = time.monotonic()  # any server message counts against R7
            if not self.handle(msg_type, Reader(payload)):
                return False
        return True

    def handle(self, t, r):
        if t == WELCOME:
            team, team_name, robot_name, game_id = r.u16(), r.string(), r.string(), r.string()
            interval, timeout, _status = r.u16(), r.u16(), r.u16()
            self.hb_interval = max(interval, 10) / 1000
            self.los_timeout = min(max(timeout, 1) / 1000, LOS_CAP_S)
            self.welcomed = True
            print('connected: team %d "%s" robot "%s" game %s' % (team, team_name, robot_name, game_id),
                  flush=True)
        elif t == REJECT:
            reason, message = r.u8(), r.string()
            print("rejected: %s: %s" % (REJECT_REASONS.get(reason, reason), message), flush=True)
            return False
        elif t == ERROR:
            code, message = r.u8(), r.string()
            print("error: %s: %s" % (ERROR_CODES.get(code, code), message), flush=True)
            if code in (3, 4, 5):  # malformed, replaced, revoked: the server closes
                return False
        elif t == MOVE_AUTH:
            enabled, reason = r.bool(), r.u8()
            self.authorize(enabled, reason)
        elif t == STATUS:
            _phase, enabled, reason = r.u8(), r.bool(), r.u8()
            r.u32(), r.u16(), r.u16()  # remaining, red, blue: decoded so a short payload is caught
            self.authorize(enabled, reason)
        elif t == PHASE_CHANGE:
            phase, duration, remaining = r.u8(), r.u32(), r.u32()
            self.on_phase(PHASES.get(phase, "unknown(%d)" % phase), duration, remaining)
        elif t == MATCH_INFO:
            match_id, number, alliance, opponent, count = r.u32(), r.u16(), r.u8(), r.u16(), r.u8()
            phases = [(r.u8(), r.u32()) for _ in range(count)]
            if match_id == 0:
                print("match: none", flush=True)
            else:
                print("match: %d (number %d) %s vs %s, %d phases" % (
                    match_id, number, ALLIANCES.get(alliance, alliance),
                    "none" if opponent == 0xFFFF else opponent, len(phases)), flush=True)
        elif t == SCORE_UPDATE:
            red, blue, _detail = r.u16(), r.u16(), r.bytes()
            self.on_score(red, blue)
        # Any other message type is ignored, as the protocol requires.
        return True


def main():
    p = argparse.ArgumentParser(description="Example STORM FMS robot client.")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5800)
    p.add_argument("--team", type=int, required=True)
    p.add_argument("--token", default=os.environ.get("STORM_TOKEN", ""),
                   help="team token; prefer the STORM_TOKEN environment variable")
    args = p.parse_args()
    if not 0 <= args.team <= 0xFFFE:
        p.error("--team must be 0 to 65534")
    if not args.token:
        p.error("set STORM_TOKEN or pass --token")

    robot = Robot(args.host, args.port, args.team, args.token)
    robot.on_motion(robot.enabled, robot.reason)  # start disabled

    def on_signal(_sig, _frame):
        robot.stop()
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    robot.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())

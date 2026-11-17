# Python example client

`storm_robot.py` is one file, Python 3.8 or later, standard library only.

    STORM_TOKEN=<token> python3 storm_robot.py --host <fms address> --team 12

| Flag      | Default     |                                                    |
|-----------|-------------|----------------------------------------------------|
| `--host`  | `127.0.0.1` | FMS address                                        |
| `--port`  | `5800`      | FMS robot port                                     |
| `--team`  | required    | your team number                                   |
| `--token` | `$STORM_TOKEN` | your team token. Prefer the environment variable |

## Adding your robot

Subclass `Robot` (or edit it) and fill in:

- `on_motion(enabled, reason)`: **stop every actuator when `enabled` is false.** It is
  called on every change, including Loss of Signal and disconnects. Keep it fast and
  never block in it.
- `on_phase(phase, duration_ms, remaining_ms)`: the change to `AUTONOMOUS` can start
  your autonomous routine. The routine still only drives while motion is enabled.
- `on_score(red, blue)`: optional.

The client runs on one thread. If your control loop runs on another, read
`robot.enabled` there before every actuator command. On Loss of Signal the client keeps
the connection for up to 3 s, so a short Wi-Fi drop recovers without a reconnect. After
that it dials again with backoff. The FMS re-enables the robot only if the rules allow
it.

## What it shows

How the client handles each part of [the protocol](../../docs/protocol.md):

- framing, with every field bounds-checked, and with bytes appended to a known message
  ignored
- unknown message types ignored. Bad magic, an oversized payload or a bad version
  closes the connection, with no attempt to resync
- `enabled` bytes other than 0 or 1 are malformed, never read as enabled
- unknown authorization reasons count as disabled
- Loss of Signal at `min(heartbeat_timeout_ms, 1 s)`
- GOODBYE on SIGINT or SIGTERM

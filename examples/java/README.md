# Java Example Client

`StormRobot.java` is one file, Java 17 or later (the version WPILib uses), JDK only.
Java runs a single source file directly, so there is no build step:

```
STORM_TOKEN=<token> java StormRobot.java --host <fms address> --team 3642
```

| Flag      | Default        |                    |
|-----------|----------------|--------------------|
| `--host`  | `127.0.0.1`    | FMS address        |
| `--port`  | `5800`         | FMS robot port     |
| `--team`  | required       | your team number   |
| `--token` | `$STORM_TOKEN` | your team token    |

Subclass `StormRobot` (or edit it) and fill in:

- `onMotion(enabled, reason)`: **stop every actuator when `enabled` is false.** It is
  called on every change, including Loss of Signal and disconnects. Keep it fast and
  never block in it.
- `onPhase(phase, durationMs, remainingMs)`: the change to `AUTONOMOUS` can start your
  autonomous routine. The routine still only drives while motion is enabled.
- `onScore(red, blue)`: optional.

The client runs on one thread. If your control loop runs on another, call
`robot.isEnabled()` there before every actuator command. On Loss of Signal the client
keeps the connection for up to 3 s, so a short Wi-Fi drop recovers without a reconnect.
After that it dials again with backoff. The FMS re-enables the robot only if the rules
allow it.

## What it shows

How the client handles each part of [the protocol](../../docs/protocol.md):

- framing with `ByteBuffer`, which is big-endian like the protocol. Every field is
  bounds-checked, and bytes appended to a known message are ignored
- unknown message types ignored. Bad magic, an oversized payload or a bad version
  closes the connection, with no attempt to resync
- `enabled` bytes other than 0 or 1 are malformed, never read as enabled
- unknown authorization reasons count as disabled
- Loss of Signal at `min(heartbeat_timeout_ms, 1 s)`
- GOODBYE on SIGINT or SIGTERM

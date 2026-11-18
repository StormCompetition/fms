# Java Example Client

`StormRobot.java` is one file and uses Java 17 or later.

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

- `onMotion(enabled, reason)`: Called whenever the robot is allowed (or not allowed) to move.
- `onPhase(phase, durationMs, remainingMs)`: Called whenever the phase is changed.
- `onScore(red, blue)`: Called whenever the score changes.

## What it shows

How the client handles each part of [the protocol](../../docs/protocol.md):

- framing with `ByteBuffer` which is big endian like the protocol.
- unknown message types are ignored.
- `enabled` bytes other than 0 or 1 are malformed and are never read as enabled
- Loss of Signal at `min(heartbeat_timeout_ms, 1 s)`
- GOODBYE on SIGINT or SIGTERM

# C++ example client

`storm_robot.cpp` is one file, C++17, POSIX sockets, no libraries. It builds on Linux,
including a Raspberry Pi or other single-board computer, and on macOS.

    make
    STORM_TOKEN=<token> ./storm_robot --host <fms address> --team 12

Without make: `g++ -std=c++17 -O2 -Wall -o storm_robot storm_robot.cpp`.

| Flag      | Default     |                                                    |
|-----------|-------------|----------------------------------------------------|
| `--host`  | `127.0.0.1` | FMS address                                        |
| `--port`  | `5800`      | FMS robot port                                     |
| `--team`  | required    | your team number                                   |
| `--token` | `$STORM_TOKEN` | your team token. Prefer the environment variable |

## Adding your robot

Fill in the hooks at the top of `class Robot`:

- `on_motion(enabled, reason)`: **stop every actuator when `enabled` is false.** It is
  called on every change, including Loss of Signal and disconnects. Keep it fast and
  never block in it.
- `on_phase(phase, duration_ms, remaining_ms)`: the change to `AUTONOMOUS` can start
  your autonomous routine. The routine still only drives while motion is enabled.
- `on_score(red, blue)`: optional.

The client runs on one thread. If your control loop runs on another, share the enabled
flag through a `std::atomic<bool>` set in `on_motion`, and check it before every
actuator command. On Loss of Signal the client keeps the connection for up to 3 s, so
a short Wi-Fi drop recovers without a reconnect. After that it dials again with
backoff. The FMS re-enables the robot only if the rules allow it.

## Porting to a microcontroller

The protocol logic (`Reader`, `Writer`, `process`, `handle`) has no OS dependencies.
On an ESP32 or similar, replace `dial`, `send`, `session`'s `poll`/`recv`, and
`Clock` with your board's TCP stack and millisecond timer. Keep the Loss of Signal
check in the main loop, not in a callback that might stop running when the link drops.

## What it shows

The same behavior as the Python client, against [the protocol](../../docs/protocol.md):
bounds-checked decoding, UTF-8 validation, unknown types and appended bytes ignored,
out-of-frame input closes the connection, unknown reasons are disabled, Loss of Signal
at `min(heartbeat_timeout_ms, 1 s)`, and GOODBYE on SIGINT or SIGTERM.

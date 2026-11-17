# Practicing with your own FMS

You can run the real FMS on a laptop and connect your robot to it, so you can test the
whole protocol before an event: the handshake, heartbeats, Loss of Signal, being enabled
and disabled, and phase changes in a timed match.

This is the **team edition** of the FMS. It is the same robot server, match timer, and
scoring the event runs, with the operator TUI and line console. It leaves out the
event's web and admin interfaces.

## Download

Releases are at https://github.com/StormCompetition/fms/releases. Pick the archive for
the computer that will run the FMS (not the robot, unless it runs both):

| Computer                        | Archive                       |
|---------------------------------|-------------------------------|
| Linux, x86-64                   | `fms-<version>-linux-amd64`   |
| Linux, ARM (Raspberry Pi 4/5, 64-bit OS) | `fms-<version>-linux-arm64` |
| macOS, Apple silicon            | `fms-<version>-darwin-arm64`  |
| macOS, Intel                    | `fms-<version>-darwin-amd64`  |
| Windows                         | `fms-<version>-windows-amd64` |

Check the download against `SHA256SUMS` from the same release.

The binaries are not signed. On macOS, clear the quarantine flag once with
`xattr -d com.apple.quarantine fms`. On Windows, SmartScreen may ask you to confirm.

## Run a practice match

Start the FMS with the operator TUI from the folder you unpacked:

    ./fms --cli

It listens for robots on TCP port 5800 on every interface, and keeps its state in
`fms.db` in the current folder. Logs go to `fms.log`. Press F1 at any time to e-stop
every robot. Press ctrl+c twice to quit.

Type commands at the `fms>` prompt. Add your team, using your real team number. The
output shows your token once:

    team add 12 "Iron Gulls" "Gull 3"

Show the token again later with `team token 12`. Connect your robot with that team
number and token, pointing it at the laptop's address. With the Java example client
from this repository:

    STORM_TOKEN=<token> java examples/java/StormRobot.java --host <laptop address> --team 12

The robot shows up in the Robots panel. Then run a match against the built-in House
team, which needs no robot:

    match create 1 12
    match prestart
    match start

`prestart` puts the match in PRE_MATCH, and `start` runs the timed phases: AUTONOMOUS,
REF_PAUSE, then TELEOP. Things to try while it runs:

- `estop 12` and `estop clear 12`: your robot must stop at once.
- `fault on` and `fault off`: a field fault disables every robot and freezes the timer.
- `robot mobility 12 disable --for 10s`: an operator override.
- `score red hull`: watch SCORE_UPDATE arrive.
- Unplug your robot's network, or stop the FMS: your robot must halt on Loss of Signal.

After the match ends, `match finalize` keeps the result, and `match reset` replays it.
Type `help` for every command.

## Settings

Every setting is optional. To change one, copy `fms.example.yaml` to `fms.yaml` next to
the binary and edit it. The file lists every key with its default. Useful ones for
testing:

- `listen`: the address robots connect to, e.g. `"192.168.1.10:5800"`.
- `heartbeat_interval`, `heartbeat_timeout`, `status_interval`: sent to your robot in
  WELCOME. Make sure your robot uses the values it receives, not its own.
- `loose_cannons_2027`: phase lengths and point values.

Use `./fms --console` instead of `--cli` for a plain prompt over ssh or from a script.
`./fms --version` prints the version.

## What it does not cover

Events can change settings, run on their own field network, and may restrict each team
to its own address range. Always check your robot against the event's FMS before your
first match.

# Practicing with your own FMS

You can run the real FMS on any system and connect your robot to it, so you can test the whole protocol: the handshake, heartbeats, Loss of Signal, being enabled and disabled, and phase changes.

This is a trimmed version of the FMS. It is the same robot server, match timer, and scoring the event runs, with the operator TUI and console. It will leave out certain admin processes, routing, etc.

## Download

Releases are at https://github.com/StormCompetition/fms/releases. Pick the archive for the computer that will run the FMS (not the robot, unless it runs both):

| Computer                        | Archive                       |
|---------------------------------|-------------------------------|
| Linux, x86-64                   | `fms-<version>-linux-amd64`   |
| Linux, ARM (Raspberry Pi 4/5, 64-bit OS) | `fms-<version>-linux-arm64` |
| macOS, Apple silicon            | `fms-<version>-darwin-arm64`  |
| macOS, Intel                    | `fms-<version>-darwin-amd64`  |
| Windows                         | `fms-<version>-windows-amd64` |

The binaries are not signed. On macOS, clear the quarantine flag once with `xattr -d com.apple.quarantine fms`.

## Run a practice match

Start the FMS with the operator TUI from the folder you unpacked:

```
./fms --cli
```

It listens for robots on TCP port 5800 on every interface, and keeps its state in `fms.db` in the current folder. Logs go to `fms.log`. Press F1 at any time to e-stop every robot. Press `CTRL + C` twice to quit.

Type commands at the `fms>` prompt. Add your team, using your real team number. The output shows your token once:

```
team add 12 "Iron Gulls" "Gull 3"
```

You can reveal the token again later with `team token 12`. Connect your robot with that team number and token, pointing it at the FMS address. With the Java example client from this repository:

```
STORM_TOKEN=<token> java examples/java/StormRobot.java --host <fms address> --team 12
```

The robot shows up in the Robots panel. Then run a match against the default team which needs no robot:

```
match create 1 12
match prestart
match start
```

`prestart` puts the match in PRE_MATCH, and `start` runs the timed phases: AUTONOMOUS, REF_PAUSE, then TELEOP. A few things you can try:

- `estop 12` and `estop clear 12`.
- `fault on` and `fault off`.
- `robot mobility 12 disable --for 10s`.
- `score red hull`.

After the match ends you can: `match finalize` which keeps the result, and `match reset` which replays it. Type `help` for every command.

## Settings

To change any settings, copy `fms.example.yaml` to `fms.yaml` next to the binary and edit it. The file lists every key with its default. For example you may want to edit:

- `listen`: the address robots connect to, e.g. `"192.168.1.10:5800"`.
- `heartbeat_interval`, `heartbeat_timeout`, `status_interval`: sent to your robot in WELCOME. Make sure your robot uses the values it receives and not its own.
- `loose_cannons_2027`: phase lengths and point values.

Use `./fms --console` instead of `--cli` for a plain prompt over ssh or from a script. `./fms --version` prints the version.

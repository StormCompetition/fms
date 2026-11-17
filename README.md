# STORM FMS: robot protocol and example clients

Everything a team needs to connect a robot to the STORM Field Management System (FMS).
The FMS controls match timing, phase, scoring, and whether each robot may move.
Operators drive their robots directly. The FMS never relays commands.

- [docs/protocol.md](docs/protocol.md): the robot protocol. It is part of the
  competition rules (rule C15), and every robot must implement it.
- [docs/games/loose-cannons-2027.md](docs/games/loose-cannons-2027.md): the 2027
  game, including the layout of the SCORE_UPDATE `detail` bytes.
- [testdata/protocol_vectors.json](testdata/protocol_vectors.json): golden bytes for
  every message, plus invalid inputs your decoder must refuse. Test against these.
- [examples/](examples/): example clients you can start from.
  - [examples/python](examples/python): Python 3.8+, standard library only.
  - [examples/cpp](examples/cpp): C++17 with POSIX sockets, for Linux boards such as a
    Raspberry Pi.

## What every robot must do

These are the safety rules from the protocol. The examples implement all of them.

1. **Start disabled.** Nothing moves until the FMS sends `enabled = 1`.
2. **Loss of Signal (rule R7).** If no message of any kind arrives from the FMS for
   `heartbeat_timeout_ms`, halt. Halt within 1 s even if the timeout is longer.
3. **Disabled means stopped (rule C17).** Obey the latest MOVE_AUTH or STATUS. Treat an
   authorization reason you do not recognize as disabled.
4. Send HEARTBEAT every `heartbeat_interval_ms`, from WELCOME on.

## Running an example

Your event gives you a team number and a 64-character token. Pass the token in the
`STORM_TOKEN` environment variable, so it stays out of your shell history and process
list:

    STORM_TOKEN=<token> python3 examples/python/storm_robot.py --host <fms address> --team 12

Each client prints one line per change:

    motion: disabled (starting)
    connected: team 12 "Iron Gulls" robot "Gull 3" game loose_cannons_2027
    motion: disabled (phase)
    phase: PRE_MATCH 0.0 s
    motion: enabled (phase)

Then put your motor code where the client says to. Disabling must stop your actuators
straight away, from the same thread that receives messages. Don't queue it behind other
work.

## Questions and changes

This repository is published from the FMS sources, so it is read-only here. Report
problems to your event's technical staff. The protocol only changes through a rules
update.

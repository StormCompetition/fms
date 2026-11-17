# STORM FMS

Everything your team will need to connect a robot to the STORM Field Management System (FMS). The FMS controls match timing, phase, scoring, and whether your robot may move. The FMS will never relay commands or anything like that..

- [docs/protocol.md](docs/protocol.md): the protocol. It is part of the competition rules (vua rule C15) and thus must be implemented.
- [docs/games/loose-cannons-2027.md](docs/games/loose-cannons-2027.md): the 2027 game which includes the layout of the SCORE_UPDATE `detail` bytes.
- [testdata/protocol_vectors.json](testdata/protocol_vectors.json): golden bytes for every message to test against.
- [examples/](examples/): an example client you can start from.
  - [examples/java](examples/java): Java 17 or later, JDK only.
- [docs/practice-fms.md](docs/practice-fms.md): run the FMS yourself, to practice before an event.

> [!NOTE]
> **AI Assistance Disclosure:** Portions of this repository, including some documentation, were generated using Large Language Models (LLMs). All AI assisted outputs have been reviewed and tested by human maintainers.

## What every robot must do

At minimum, your robot must do the following. The example client demonstrates each of these.

1. **Start disabled** Nothing moves until the FMS sends `enabled = 1`.
2. **Loss of Signal (rule R7).** If no message of any kind arrives from the FMS for `heartbeat_timeout_ms`, halt. Halt within 1 s even if the timeout is longer.
3. **Disabled means stopped (rule C17).** Obey the latest MOVE_AUTH or STATUS.
4. Send HEARTBEAT every `heartbeat_interval_ms`, from WELCOME and onward.

## Running an example

Your team dashboard gives you a 64-character token and a team number you will use to identify your connection. Pass the token in the `STORM_TOKEN` environment variable:

```bash
STORM_TOKEN=<token> java examples/java/StormRobot.java --host <fms address> --team 3942
```

## FMS Source & Example Servers

The source code of the actual FMS will remain private for the time being. However, we have provided prebuilt servers that can be run on both Windows and Linux. These are trimmed down versions of what will actually be running on the field, but should provide the core functionality for testing.

## Questions and Issues

If you encounter any issues, please feel free to email [storm@ou.edu](mailto:storm@ou.edu) or create an [issue](https://github.com/StormCompetition/fms/issues) here on this github page.
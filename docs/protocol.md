# STORM FMS Robot Protocol (Version 1)

The FMS is the field's authority on match timing, phase, scoring, and whether each robot may move.

Reference clients are in `examples/`. Golden byte vectors for every message are in `testdata/protocol_vectors.json` and at the end of this document.

## Transport

- TCP. Default server port 5800.
- The robot connects to the server. One connection per team (rule C4).
- Clients should set `TCP_NODELAY` as messages are generally small and we want to reduce latency.
- The server limits connections per source IP address (default 4). Connections over the limit are closed immediately without a message.

## Framing

All integers are unsigned and big endian. Every message is a 12-byte header followed by `payload_len` bytes of payload.

    offset size field
    0      4    magic        "STRM" (0x5354524D)
    4      1    version      0x01
    5      1    msg_type
    6      2    flags        reserved (set to 0)
    8      2    payload_len  bytes following the header (max 1024)
    10     2    sequence     per sender and starts at 0, +1 per message. range is 0 to 65535

Receiver rules:

- Bad magic, or `payload_len` over 1024: close the connection immediately.
- Version other than 0x01: the server sends REJECT `bad_version` and closes.
- A payload shorter than the fields of its message type is malformed.
- Bytes after the last known field of a message are ignored. Later revisions of version 1 may append fields to a message. They never change the meaning or position of the fields before them.
- `sequence` is purely informational.

### Field Types

| Type   | Encoding                                                     |
|--------|--------------------------------------------------------------|
| u8     | 1 byte                                                       |
| u16    | 2 bytes, big endian                                          |
| u32    | 4 bytes, big endian                                          |
| string | u8 length, then that many bytes of UTF-8 (max 255 bytes)     |
| bytes  | u16 length, then that many raw bytes                         |

Invalid UTF-8 in a string is malformed.

The `enabled` fields in MOVE_AUTH and STATUS are u8 and must be exactly 0 or 1. Any other value is malformed and must be ignored or treated as disabled.

## Connection Lifecycle

1. Robot connects and sends HELLO. The server closes the connection if HELLO does not arrive within 2s. Any other message before HELLO gets REJECT `malformed` and is closed.
2. The server checks in order: team exists and is active, token matches, source IP is inside the team's allowed CIDRs. Any failure is REJECTED with the matching reason and a close.
3. On success the server sends in order: WELCOME, MATCH_INFO (only if the team is in the loaded match), MOVE_AUTH, STATUS.
4. The robot sends HEARTBEAT every `heartbeat_interval_ms` from WELCOME starting as soon as possible.
5. The server sends STATUS every `status_interval_ms`, plus event messages (PHASE_CHANGE, MOVE_AUTH, SCORE_UPDATE, MATCH_INFO) when things change.
6. Either side may close. The robot should send GOODBYE first when it shuts down on purpose.

If a second valid connection arrives for a team, it replaces the first. The server sends ERROR `replaced` on the old connection and closes it.

## Safety requirements

- **Loss of Signal (rule R7).** If the robot receives no server message of any kind for `heartbeat_timeout_ms`, it must treat the connection as lost and halt all motion. It must halt within 1s of the last server message regardless of the configured timeout.
- **Disabled (rule C17).** When the most recent MOVE_AUTH or STATUS says `enabled = 0` the robot is not allowed to be controlled. Treat any authorization reason you do not recognize as disabled.
- **Starting Disabled.** A robot is disabled from connect until the server says otherwise.
- The server side view: if no HEARTBEAT arrives for `heartbeat_timeout_ms` the server marks the robot `loss_of_signal` and disables it. It will re-enable it once heartbeats resume.

## Messages

### Robot -> Server

#### 0x01 HELLO

| Field          | Type   | Notes                                       |
|----------------|--------|---------------------------------------------|
| team_id        | u16    | 0 is the default team                       |
| token          | string | 64 hex characters                           |
| client_name    | string | free text for logging                       |
| client_version | string | free text for logging                       |

#### 0x02 HEARTBEAT

No fields. The payload is empty (length 0).

#### 0x03 GOODBYE

| Field  | Type | Notes                 |
|--------|------|-----------------------|
| reason | u8   | see Goodbye reasons   |

### Server -> Robot

#### 0x81 WELCOME

| Field                 | Type   | Notes                                     |
|-----------------------|--------|-------------------------------------------|
| team_id               | u16    |                                           |
| team_name             | string | max 64 bytes                              |
| robot_name            | string | max 64 bytes                              |
| game_id               | string | e.g. `loose_cannons_2027`                 |
| heartbeat_interval_ms | u16    | how often the robot sends HEARTBEAT       |
| heartbeat_timeout_ms  | u16    | Loss of Signal threshold, both directions |
| status_interval_ms    | u16    | how often the server sends STATUS         |

`game_id` tells the robot how to read the `detail` field of SCORE_UPDATE.

#### 0x82 REJECT

| Field   | Type   | Notes                         |
|---------|--------|-------------------------------|
| reason  | u8     | see Reject reasons            |
| message | string | human readable for logging    |

The server closes the connection after sending REJECT.

#### 0x83 MATCH_INFO

| Field            | Type | Notes                                         |
|------------------|------|-----------------------------------------------|
| match_id         | u32  | 0 = no match                                  |
| match_number     | u16  |                                               |
| alliance         | u8   | see Alliance                                  |
| opponent_team_id | u16  | 0xFFFF = no opponent                          |
| phase_count      | u8   | number of timed phases that follow            |
| phases           | ...  | `phase_count` entries of (phase u8, duration_ms u32) |

Sent when a match containing the team is loaded or reset, on connect if the team is in the loaded match, and with `match_id = 0` when a new match is loaded that does not include the team. When `match_id` is 0 the remaining fields are zero and `phase_count` is 0.

`phases` lists the timed phases in the order they run. For STORM 2027 this is AUTONOMOUS 20000, REF_PAUSE 5000, TELEOP 220000.

#### 0x84 PHASE_CHANGE

| Field              | Type | Notes                                 |
|--------------------|------|---------------------------------------|
| phase              | u8   | see Phases                            |
| phase_duration_ms  | u32  | 0 for untimed phases                  |
| phase_remaining_ms | u32  | 0 for untimed phases                  |

Sent to robots in the loaded match on every phase change. The change to AUTONOMOUS is a valid trigger for starting an autonomous routine.

A field fault does not change the phase. The timer freezes and robots receive MOVE_AUTH `disabled, field_fault`. STATUS keeps reporting the frozen `phase_remaining_ms`.

#### 0x85 MOVE_AUTH

| Field   | Type | Notes                     |
|---------|------|---------------------------|
| enabled | u8   | 0 disabled, 1 enabled     |
| reason  | u8   | see Authorization reasons |

Sent whenever the robot's authorization changes, and on connect. An e-stop is delivered
ahead of anything else queued for the robot.

#### 0x86 SCORE_UPDATE

| Field      | Type  | Notes                                  |
|------------|-------|----------------------------------------|
| red_total  | u16   |                                        |
| blue_total | u16   |                                        |
| detail     | bytes | game specific, layout set by `game_id` |

Sent to robots in the loaded match when the score changes. The detail layout for each game is in `docs/games/`. 

- STORM 2027 (Loose Cannons): [loose-cannons-2027.md](games/loose-cannons-2027.md).

#### 0x87 STATUS

| Field              | Type | Notes                                  |
|--------------------|------|----------------------------------------|
| phase              | u8   | phase of the loaded match, IDLE if none |
| enabled            | u8   | this robot's authorization             |
| auth_reason        | u8   | see Authorization reasons              |
| phase_remaining_ms | u32  | 0 for untimed phases                   |
| red_total          | u16  |                                        |
| blue_total         | u16  |                                        |

Sent every `status_interval_ms` to every connected robot.

#### 0x8F ERROR

| Field   | Type   | Notes                    |
|---------|--------|--------------------------|
| code    | u8     | see Error codes          |
| message | string | human readable, for logs |

Codes `malformed`, `replaced`, and `revoked` are followed by a close. Others are not.

## Enums

### Message types

| Value | Name         | Direction       |
|-------|--------------|-----------------|
| 0x01  | HELLO        | robot to server |
| 0x02  | HEARTBEAT    | robot to server |
| 0x03  | GOODBYE      | robot to server |
| 0x81  | WELCOME      | server to robot |
| 0x82  | REJECT       | server to robot |
| 0x83  | MATCH_INFO   | server to robot |
| 0x84  | PHASE_CHANGE | server to robot |
| 0x85  | MOVE_AUTH    | server to robot |
| 0x86  | SCORE_UPDATE | server to robot |
| 0x87  | STATUS       | server to robot |
| 0x8F  | ERROR        | server to robot |

Robots must ignore server message types they do not recognize. The server answers an unknown robot message type with ERROR `unknown_type` and keeps the connection open.

### Phases

| Value | Name       | Robots May Move                            |
|-------|------------|--------------------------------------------|
| 0     | IDLE       | no                                         |
| 1     | LOADED     | no                                         |
| 2     | PRE_MATCH  | if the rules allows it (rule M2, default yes) |
| 3     | AUTONOMOUS | yes                                        |
| 4     | REF_PAUSE  | no                                         |
| 5     | TELEOP     | yes                                        |
| 6     | ENDED      | no (rule M4)                               |
| 7     | FINALIZED  | no                                         |
| 8     | ABORTED    | no                                         |

### Authorization Reasons

| Value | Name           | Meaning                                         |
|-------|----------------|-------------------------------------------------|
| 0     | unknown        | treat as disabled                               |
| 1     | phase          | decided by the match phase                      |
| 2     | estop          | e-stopped (robot or field), latched             |
| 3     | admin_disable  | disabled by an official                         |
| 4     | admin_enable   | enabled by an official for testing or inspection |
| 5     | loss_of_signal | server stopped hearing heartbeats               |
| 6     | disqualified   | team disqualified from the match                |
| 7     | field_fault    | match paused for a field fault                  |
| 8     | no_match       | team is not in the loaded match                 |

### Reject Reasons

| Value | Name           |
|-------|----------------|
| 1     | unknown_team   |
| 2     | inactive_team  |
| 3     | bad_token      |
| 4     | ip_not_allowed |
| 5     | bad_version    |
| 6     | malformed      |

### Error Codes

| Value | Name               | Meaning                                    |
|-------|--------------------|--------------------------------------------|
| 1     | unexpected_message | valid message at the wrong time (e.g. second HELLO) |
| 2     | unknown_type       | unrecognized msg_type                      |
| 3     | malformed          | payload could not be decoded (connection closes) |
| 4     | replaced           | a newer connection for this team took over (connection closes) |
| 5     | revoked            | credentials or permissions changed (connection closes) |

### Alliance

| Value | Name |
|-------|------|
| 1     | red  |
| 2     | blue |

### Goodbye Reasons

| Value | Name         |
|-------|--------------|
| 0     | unspecified  |
| 1     | shutting_down |
| 2     | restarting   |

## Example Exchance

    robot  HELLO        team_id=12 token=... client_name="bot" client_version="1.0"
    server WELCOME      team_id=12 team_name="Iron Gulls" ... game_id="loose_cannons_2027"
    server MATCH_INFO   match_id=7 match_number=3 alliance=red opponent=40 phases=[3:20000 4:5000 5:220000]
    server MOVE_AUTH    enabled=0 reason=phase
    server STATUS       phase=LOADED enabled=0 reason=phase remaining=0
    robot  HEARTBEAT    every 100 ms
    server STATUS       every 100 ms
    server PHASE_CHANGE phase=PRE_MATCH
    server MOVE_AUTH    enabled=1 reason=phase
    server PHASE_CHANGE phase=AUTONOMOUS duration=20000 remaining=20000
    ...

## Revisions

None so far.

## Test Vectors

Generated by `go generate ./internal/protocol`. Do not edit by hand.

<!-- vectors:begin -->

#### hello (HELLO, seq 0)

    {"team_id":12,"token":"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff","client_name":"example","client_version":"1.0"}

    header   53 54 52 4d 01 01 00 00 00 4f 00 00
    payload  00 0c 40 30 30 31 31 32 32 33 33 34 34 35 35 36
             36 37 37 38 38 39 39 61 61 62 62 63 63 64 64 65
             65 66 66 30 30 31 31 32 32 33 33 34 34 35 35 36
             36 37 37 38 38 39 39 61 61 62 62 63 63 64 64 65
             65 66 66 07 65 78 61 6d 70 6c 65 03 31 2e 30

#### heartbeat (HEARTBEAT, seq 1)

    {}

    header   53 54 52 4d 01 02 00 00 00 00 00 01

#### goodbye (GOODBYE, seq 3)

    {"reason":1}

    header   53 54 52 4d 01 03 00 00 00 01 00 03
    payload  01

#### welcome (WELCOME, seq 0)

    {"team_id":12,"team_name":"Iron Gulls","robot_name":"Gull 3","game_id":"loose_cannons_2027","heartbeat_interval_ms":100,"heartbeat_timeout_ms":500,"status_interval_ms":100}

    header   53 54 52 4d 01 81 00 00 00 2d 00 00
    payload  00 0c 0a 49 72 6f 6e 20 47 75 6c 6c 73 06 47 75
             6c 6c 20 33 12 6c 6f 6f 73 65 5f 63 61 6e 6e 6f
             6e 73 5f 32 30 32 37 00 64 01 f4 00 64

#### reject (REJECT, seq 0)

    {"reason":3,"message":"bad token"}

    header   53 54 52 4d 01 82 00 00 00 0b 00 00
    payload  03 09 62 61 64 20 74 6f 6b 65 6e

#### match_info (MATCH_INFO, seq 1)

    {"match_id":7,"match_number":3,"alliance":1,"opponent_team_id":40,"phases":[{"phase":3,"duration_ms":20000},{"phase":4,"duration_ms":5000},{"phase":5,"duration_ms":220000}]}

    header   53 54 52 4d 01 83 00 00 00 19 00 01
    payload  00 00 00 07 00 03 01 00 28 03 03 00 00 4e 20 04
             00 00 13 88 05 00 03 5b 60

#### match_info_practice (MATCH_INFO, seq 1)

    {"match_id":8,"match_number":4,"alliance":2,"opponent_team_id":65535,"phases":[{"phase":3,"duration_ms":20000},{"phase":4,"duration_ms":5000},{"phase":5,"duration_ms":220000}]}

    header   53 54 52 4d 01 83 00 00 00 19 00 01
    payload  00 00 00 08 00 04 02 ff ff 03 03 00 00 4e 20 04
             00 00 13 88 05 00 03 5b 60

#### match_info_none (MATCH_INFO, seq 1)

    {"match_id":0,"match_number":0,"alliance":0,"opponent_team_id":0,"phases":null}

    header   53 54 52 4d 01 83 00 00 00 0a 00 01
    payload  00 00 00 00 00 00 00 00 00 00

#### phase_change (PHASE_CHANGE, seq 2)

    {"phase":3,"phase_duration_ms":20000,"phase_remaining_ms":20000}

    header   53 54 52 4d 01 84 00 00 00 09 00 02
    payload  03 00 00 4e 20 00 00 4e 20

#### move_auth_enabled (MOVE_AUTH, seq 3)

    {"enabled":true,"reason":1}

    header   53 54 52 4d 01 85 00 00 00 02 00 03
    payload  01 01

#### move_auth_estop (MOVE_AUTH, seq 4)

    {"enabled":false,"reason":2}

    header   53 54 52 4d 01 85 00 00 00 02 00 04
    payload  00 02

#### score_update (SCORE_UPDATE, seq 5)

    {"red_total":49,"blue_total":6,"detail":"0005010201000601010000000000"}

    header   53 54 52 4d 01 86 00 00 00 14 00 05
    payload  00 31 00 06 00 0e 00 05 01 02 01 00 06 01 01 00
             00 00 00 00

#### status (STATUS, seq 65535)

    {"phase":5,"enabled":true,"auth_reason":1,"phase_remaining_ms":123400,"red_total":49,"blue_total":12}

    header   53 54 52 4d 01 87 00 00 00 0b ff ff
    payload  05 01 01 00 01 e2 08 00 31 00 0c

#### error (ERROR, seq 6)

    {"code":4,"message":"replaced by new connection"}

    header   53 54 52 4d 01 8f 00 00 00 1c 00 06
    payload  04 1a 72 65 70 6c 61 63 65 64 20 62 79 20 6e 65
             77 20 63 6f 6e 6e 65 63 74 69 6f 6e

#### Invalid input

| Name | Bytes | Expected |
|------|-------|----------|
| bad_magic | `535452580102000000000000` | bad_magic |
| payload_too_large | `5354524d0102000004010000` | too_large |
| bad_version | `5354524d0201000000000000` | bad_version |
| unknown_type | `5354524d017f000000000000` | unknown_type |
| short_payload | `5354524d018500000001000000` | malformed |
| bad_utf8 | `5354524d01820000000300000101ff` | malformed |
| bad_bool | `5354524d01850000000200000201` | malformed |

`bad_magic` and `too_large` mean close immediately. `bad_version` gets REJECT `bad_version`. `unknown_type` is ignored by robots. `malformed` gets ERROR `malformed` (or REJECT `malformed` before HELLO completes).

<!-- vectors:end -->

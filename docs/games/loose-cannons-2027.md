# STORM 2027 "Loose Cannons"

game_id: `loose_cannons_2027`. Rule references are to the 2027 game manual, section 3.4
and rules M3 and M6.

## Phases

| Phase      | Default duration | Robots |
|------------|------------------|--------|
| AUTONOMOUS | 20 s             | enabled. Operators may only start or stop an auto routine |
| REF_PAUSE  | 5 s              | disabled. Referees confirm autonomous scoring |
| TELEOP     | 220 s            | enabled |

PRE_MATCH movement is allowed by default (rule M2, `prematch_movement: true`). ENDED is
disabled (rule M4). Durations come from `fms.yaml` under `loose_cannons_2027:` (keys
`auto`, `ref_pause`, `teleop`).

## Points

All values are set by the event in the FMS config under `loose_cannons_2027.scoring`.
Defaults:

| Item                           | Auto | TeleOp |
|--------------------------------|------|--------|
| Leave Staging Zone             | 5    |        |
| Rudder hit                     | 4    | 2      |
| Hull hit                       | 8    | 4      |
| Mast hit                       | 10   | 5      |
| Climb                          |      | 12     |
| Damaged bonus, per entry point |      | 5      |
| Minor penalty                  | -6   | -6     |
| Major penalty                  | -15  | -15    |
| Damaged threshold              | 5 balls |     |

Auto hit values are double the TeleOp values (`auto_multiplier: 2`).

## Rules as implemented

- **Hits** are scored by the alliance that shot the ball into the opposing ship.
  `score red hull` means red put a ball into blue's hull.
- **Phase attribution** comes from the event, not the clock. Hits logged during
  REF_PAUSE default to auto, because that is when referees confirm autonomous scoring.
- **Damaged.** An entry point on a ship is Damaged once 5 live (not voided) balls have
  entered it, counting all phases. Damaged awards nothing on its own. Hits after that
  still score normally.
- **Climb**: hanging from the opposing ship's rope at match end. Logged in ENDED, at most
  once per alliance.
- **Damaged bonus**: +5 for each Damaged entry point on the opposing ship (the ones this
  alliance damaged), only if this alliance Climbed.
- **Penalties** subtract from the offending alliance. `penalty red minor` takes 6 from red.
- **Disqualification** sets that alliance's final score to 0.
- **Floor**: the final score is never below 0.

Total, in order:

    total = auto + teleop + climb + damaged_bonus - penalties
    total = max(total, 0)
    if disqualified: total = 0

## Inputs

| Input   | Logged in                        | Attributed to | Default attribution |
|---------|----------------------------------|---------------|---------------------|
| leave   | AUTONOMOUS, REF_PAUSE            | auto          | auto |
| rudder  | AUTONOMOUS, REF_PAUSE, TELEOP, ENDED | auto, teleop | auto in AUTONOMOUS and REF_PAUSE, teleop after |
| hull    | same as rudder                   | auto, teleop  | same as rudder |
| mast    | same as rudder                   | auto, teleop  | same as rudder |
| climb   | ENDED                            | teleop        | teleop |
| minor   | PRE_MATCH through ENDED          | any           | current phase |
| major   | PRE_MATCH through ENDED          | any           | current phase |
| dq      | PRE_MATCH through ENDED          | any           | current phase |

Refused inputs carry an FMS reason code plus detail, for
example `wrong_phase: leave is only allowed in AUTONOMOUS, REF_PAUSE` or
`already_scored: blue already has climb`.

## SCORE_UPDATE detail

`detail` is 14 bytes: red block then blue block, 7 bytes each.

    offset size field
    0      1    rudder_hits     balls this alliance put in the opposing rudder (all phases, max 255)
    1      1    hull_hits
    2      1    mast_hits
    3      1    damaged         bitmask of opposing entry points this alliance has Damaged
                                bit0 rudder, bit1 hull, bit2 mast
    4      1    climbed         0 or 1
    5      2    penalty_points  points deducted from this alliance (positive number)

Counts saturate at 255 on the wire. The server keeps exact counts.

## Example

Red in auto: leave, 2 hull. TeleOp: 3 more hull (hull now Damaged), 1 mast. Climbs. One
minor penalty.

    auto     5 + 2*8         = 21
    teleop   3*4 + 1*5       = 17
    climb                    = 12
    bonus    1 damaged * 5   =  5
    penalty                  = -6
    total                    = 49

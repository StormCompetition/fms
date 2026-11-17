# STORM 2027 "Loose Cannons"

game_id: `loose_cannons_2027`.

## Phases

| Phase      | Default duration | Robots |
|------------|------------------|--------|
| AUTONOMOUS | 20 s             | enabled |
| REF_PAUSE  | 5 s              | disabled |
| TELEOP     | 220 s            | enabled |

PRE_MATCH movement is allowed by default (rule M2, `prematch_movement: true`). ENDED is disabled (rule M4).

## Points

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

```
total = auto + teleop + climb + damaged_bonus - penalties
total = max(total, 0)
if disqualified: total = 0
```

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

## Example

Red in auto: leave, 2 hull. TeleOp: 3 more hull (hull now Damaged), 1 mast. Climbs. One minor penalty.

    auto     5 + 2*8         = 21
    teleop   3*4 + 1*5       = 17
    climb                    = 12
    bonus    1 damaged * 5   =  5
    penalty                  = -6
    total                    = 49

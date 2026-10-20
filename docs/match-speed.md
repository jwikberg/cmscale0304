# The match you watch: highlight speed and Very Fast

Options: `match_hl_speed` (default 0), `match_speed_max` (default 8).

## The question

At Very Fast, a watched match still takes minutes, and the highlights don't speed up at all. Why?

## What the game does

Timers pace a watched match, not the CPU. The game thread used 12–36% of a core throughout.

The match screen's per-frame update (`0x59e9c0`) works like this:

1. When the screen's timer at `+0x2c20` runs out, the update advances the engine by one or more ticks (`0x59de80`).
2. It then sets the timer again, with a delay that depends on the screen's mode byte at `+0x2cc8`:

| Mode | When | Delay | Does Speed change it? |
|---|---|---|---|
| 1 | a highlight plays | `ticks × replay_ms × 0.125` | no |
| 2 | the match runs on between highlights, up to 40 ticks a step | `ticks × 10 / (speed × 8)` | yes |
| 3 | commentary lines between highlights | the line's hold time divided by speed | yes |

- **The Speed menu** (in the match's Options menu) only writes a double at `+0x2cc0`. Its five values are 0.6, 0.72, 1,
  3 and 8, from Very Slow to Very Fast, stored as constants from `0xd24200` to `0xd24220`.
- **`replay_ms`** comes from a separate replay-speed byte at `+0x2e10`, through `0x596d60`. In a live match it is
  1000 ms, "Real Time". A replay drops it to 2000 ms and then 4000 ms around the incident, which gives the slow motion.
- So highlights always play at 125 ms per tick, whatever the match speed. At Very Fast, the highlights take nearly the
  whole match. Each takes 12–31 s, while the 10–20 match minutes between them go by in 1–3 s.

## How cmscale works

- **`match_hl_speed`** adds a stub at `0x59e1ef`, where the game has just pushed the delay for the timer call. The stub
  divides the delay by 3 (`1`, Fast) or by 8 (`2`, Very Fast), the game's own factors. It does so only when all of
  these hold:
  - the mode is 1, so a highlight is playing;
  - no replay is running (`+0x2e04` is −1);
  - the highlights viewer isn't open (`+0x2e00` is −1).

  Replays and the viewer keep their own speed and slow motion. The engine runs the same ticks in the same order, so
  match results don't change. Only the wall time between ticks does.
- **`match_speed_max`** changes the 8 behind Very Fast, a double in the exe's data at `0xd24220`. The Speed menu reads
  it when the menu is built and when you choose an entry. At startup cmscale writes the configured value there, but
  only if it still reads 8. The game remembers the chosen speed between matches, so pick Very Fast again after
  changing the option.

## How it was researched

- Ghidra decompiles of the match screen's update, advance and timer functions gave the modes and the delay formulas.
- A research probe on `0x59e9c0` and `0x59e1ef` logged every change of mode, speed and replay state. Every 10 s it also
  logged the wall time, the engine ticks, the timer delays per mode and the game thread's CPU time.
- The test watched matches at Medium and at Very Fast, with and without the change.

## Measured

| Run | During a highlight | Between highlights |
|---|---|---|
| Medium, stock | 125 ms per tick, 10–15 s per highlight | 42 ms per step |
| Very Fast, stock | 125 ms per tick. A 119-tick highlight took 14.8 s, and its replay 12.6 s more | 5 ms per step |
| Medium, `match_hl_speed=2` | 16 ms per tick, 222 ticks in 3.8 s | 41–42 ms per step |

With highlights at 8×, the 2D pitch animates normally and the commentary keeps up.

`match_speed_max` above 8 shortens only the stretches between highlights. Over a first half, 32× saved about 3.5 s
compared with 8×. Each step still takes at least about 11 ms of frame time for up to 40 ticks, and some stretches stay
slow at any speed, such as the kick-off and the minutes before half time. Halves at 16× and 32× ended normally, and the
2D pitch and commentary kept working.

Normal play confirmed the first version, in which highlights followed the match speed. The current version gives
highlights a speed of their own instead.

## Limits

- Replays are now the longest wait. One replay at 32× took 12.6 s: 3.9 s at Real Time, 1.9 s at Slow and 6.8 s at Very
  Slow. The Replays item in the match screen's Options menu can turn them off.
- The "Full Match" and "Extended" highlight settings weren't measured. With Full Match, mode 1 covers most of the
  match, so `match_hl_speed` should matter most there.

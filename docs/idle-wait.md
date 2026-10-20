# Idle screens spin a core

Option: `idle_wait` (default 1).

## What the game does

- The main loop (`0x99a980`) calls the message loop function (`0x988240`). That function calls
  `PeekMessage(PM_NOREMOVE)`, and `GetMessage` only if something is queued. Then the main loop goes round again at
  once. The exe never calls `Sleep` or waits anywhere.
- On an idle screen that is about 190 000 polls a second and 89–99% of a core. The time goes into the poll itself and
  into the UI compositor. It doesn't slow the game, but it costs power, heat and battery.

## How cmscale works

The `PeekMessageA` hook counts empty polls that come less than 2 ms after the previous one. After more than 20 in a
row, it waits up to 1 ms with `MsgWaitForMultipleObjects(QS_ALLINPUT)`, which returns at once when input arrives.
`timeBeginPeriod(1)` makes the 1 ms wait accurate.

During day processing the game polls only every 20–90 ms. That resets the count, so processing never waits.

## How it was researched

The sampling profiler (see [research methods](research-methods.md)) recorded the poll and present counts every 250 ms.
On an idle squad screen it showed the poll rate, and that 24% of the samples were inside `NtUserPeekMessage`.

## Measured

| | Idle squad screen | First Continue of the pre-season save |
|---|---|---|
| `idle_wait=0` | 98–99% CPU, ~520 000 polls/s | 8.42 s CPU, 8.52 s wall |
| `idle_wait=1` | 3–7% CPU, ~1 100 polls/s | 8.44 s CPU, 8.53 s wall |

The screen keeps animating, and the news ticker redraws as often as before. Mouse and keyboard response is unchanged.
Normal play confirmed it.

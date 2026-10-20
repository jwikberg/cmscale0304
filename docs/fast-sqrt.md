# The match engine's square roots

Option: `fast_sqrt` (default 1). Exact.

## What the game does

- **The match simulation keeps the CPU busy during day processing.** While the game processed a Continue, one core
  was 94–99% busy, and 96–98% of the profiler's samples were inside the exe's own code. The simulation `0x8e4350` was
  on the stack in 82% of them. File I/O, GDI and Wine cost almost nothing.
- **The hottest single function is `0x8d1cc0`, at 16–19%.** It computes `distance(dx, dy) = floor(sqrt(dx² + dy²))`
  (`stdcall`, `ret 8`). It forms `n = dx*dx + dy*dy` in 32 bits, then takes the square root digit by digit in 10
  branchless steps, about 60 dependent instructions. It returns `floor(sqrt(n))` for `n < 2^24` and 4095 for anything
  larger, including sums that overflowed, because the compare is unsigned.
- `0x8d1d50` uses the same algorithm over 14 steps, as an exact 32-bit `isqrt`.

On a 2003 CPU without a fast square root, this made sense. Today the `sqrtsd` instruction takes a few cycles.

## How cmscale works

cmscale checks the first bytes of both functions and replaces them with a `jmp` to SSE2 versions in the DLL:

- `distance` forms the same 32-bit sum with the same 4095 cut-off, then truncates the result of `sqrtsd`.
- `isqrt` converts the unsigned input to a double exactly, as two halves, then uses `sqrtsd`.

A double holds `n` exactly, and `sqrtsd` rounds correctly, so the truncated result is the exact floor. Using SSE keeps
the game's x87 state out of it. An x87 `fsqrt` at the game's 24-bit precision would round `sqrt(4095² − 1)` up to
4095.

## How it was verified

A test program copied the original machine code from the exe, called it directly under Wine and compared it with the
replacements (see [research methods](research-methods.md)):

- all 2^32 inputs of `isqrt`;
- 300 million `(dx, dy)` pairs for `distance`, covering a full −5000 to 5000 grid and random values of every
  magnitude, including sums that overflow.

There were zero mismatches.

## Measured

| First Continue of the pre-season save, 13 Jul 09:00 to 16 Jul 19:45 | CPU time |
|---|---|
| stock | 9.18–9.41 s |
| `fast_sqrt=1` | 8.01–8.17 s |

Each match day takes 11–13% less CPU. Linux `perf` gave the same picture. On the three-month holiday the match engine
is a smaller share of the work, and the gain is 3–4.5%. Saves and match results are unchanged. Normal play confirmed
it.

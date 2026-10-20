# The random number generator

Option: `fast_rand` (default 2). Exact: it produces the same numbers in the same order.

## What the game does

- `0x983a50` is the game's `rand(n)` (`thiscall`, `ret 4`), built on a Mersenne Twister (MT19937). It takes the next
  of 624 state words and tempers it into `r`. When the words run out, it refills them through `0x9839a0`.
- It returns `(r31 * (int64)(int)(2n)) >> 32` in `eax`, with its sign in `edx`, where `r31 = r & 0x7fffffff`.
- The scaling code at the end of the function, at `0x983a97`, pushes both operands as 64-bit values and calls the
  compiler's `_allmul` helper (`0xa41760`).
- The generator takes about 8% of a match day. `_allmul` is 2.2% of that and the refill 1%. One Continue makes about
  185 million draws.

## How cmscale works

**`fast_rand=1` uses one multiply.** cmscale checks the 31 bytes of scaling code and overwrites them in place with
`and`, `xchg` and one signed `imul`, padded with `int3`.

- The two 64-bit operands are the sign extensions of `r31` and `int(2n)`, so a signed 32-by-32-bit `imul` gives
  exactly the 64-bit product that `_allmul` computes.
- The new code leaves `eax`, `edx` and `ecx` as the original did. `_allmul` leaves `int(2n)` in `ecx`.
- It needs no code cave and no jump to the DLL.

**`fast_rand=2` also refills the state with SSE2.** `0x9839a0` refills the 624 words with the standard recurrence, in
three runs:

- for `i < 227` it reads words that it hasn't updated yet;
- for `227 ≤ i < 623` it reads words 227 places back, which it has already updated;
- for `i = 623` it reads the new `mt[0]`.

Within each run, no word depends on the three before it, so the replacement computes four words per step. A small
thunk keeps `ecx`, because the caller reads the state's index through it right after the call. The thunk also leaves
`eax` and `edx` as the original's last iteration does.

## How it was researched

The profiler ranked `0x983a50` and `_allmul` among the top functions of a match day. The disassembly showed that
`_allmul` always takes its fast path here. The high halves of its operands are only sign extensions, so a single
multiply instruction gives the same product.

## How it was verified

A test program loaded the exe's own generator code pages twice under Wine, one copy as shipped and one with the patches
applied (see [research methods](research-methods.md)). It compared:

- every 32-bit tempered word for 18 edge values of `n`, among them 0, 1, 2^30 ± 1, `INT_MAX`, −1 and `INT_MIN`;
- 10^9 random pairs of a word and `n`;
- 2 × 10^8 full `rand(n)` calls on generators seeded with the game's own seed function, about 320 000 of which went
  through the SSE2 refill;
- 2 × 10^6 refills of random states.

The test compared all 625 state words and `eax`, `edx` and `ecx`, and found zero mismatches. It also caught two broken
controls every time: one that used an unsigned `mul`, and one that flipped a state bit after the refill.

## Measured

| First Continue of the pre-season save | Generator's share of `perf` samples | Generator time |
|---|---|---|
| `fast_rand=0` | 9.2–9.8% | 0.76–0.83 s |
| `fast_rand=1` | 7.1–7.6% | 0.58–0.62 s |
| `fast_rand=2` | the refill drops from 1.4–1.8% to 0.3–0.5% | 0.09 s less again |

`1` saves about 2% of a match day's CPU, and `2` about 1% more. Both are close to the spread between runs, so the
`perf` shares are the firmer numbers. Normal play confirmed both settings.

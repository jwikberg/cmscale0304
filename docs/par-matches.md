# Background matches on threads

Option: `par_matches` (default 8, up to 16; 0 turns it off).

## The question

The match simulation is about 78% of a match day's CPU, and the game runs it on one thread. Can matches run in
parallel?

## What the game does

```
0x61cb90   the day loop: one background fixture per pass, then back to the main loop
 └ 0x61b7b0   plays one fixture
    ├ setup: a fresh 0xa0f90-byte match object; copies the teams in; seeds the match's RNG
    ├ 0x8e4350   the simulation                     ~92% of 0x61b7b0
    ├ 0x61b180   recording: results and stats into the database
    └ frees the match object
```

The simulation mostly keeps to itself:

- Each match has its own object, its own players, ball and officials, and its own Mersenne Twister.
- It reads the database but didn't write to it. A write watch over nine matches, one of them write by write, and a
  static scan of the 822 functions it reaches by direct calls both showed no writes.
- Stock results aren't reproducible anyway. The game seeds each match's generator with `GetTickCount() % 569`, so a
  parallel version only needs to behave equivalently, not identically.

It does share some state:

| Shared state | Found by |
|---|---|
| Two scratch globals, `0xdc0118` and `0xdc011c`, which 1 145 instructions use and a match writes about 15 million times | static scan, write watch |
| A function-local static `PLAYER_TACTICS` at `0xeedbd0`, which one call of `0x911ab0` fills and then reads | static scan |
| A global pool of per-player position-history buffers (`0x8d8090` takes one, `0x8d38f0` returns it) | write watch |
| Global random number generators in the exe's data, with one draw site in the engine at `0x95c6a2` | static scan |
| The text system's 14 string-heap classes and the formatting code around them | write watch, then crashes |

## How cmscale works

**Each thread gets its own copy of the scratch globals.** Each of the 1 145 instructions becomes a `jmp` to a thunk.
The thunk runs the same instruction with an `fs:` prefix and a displacement into the thread's TLS slots, so each thread
reads and writes its own copy. A `jmp` doesn't change the flags, so a `cmp` still feeds the conditional jump after it.
`tools/par_sites.py` generates the list of sites, `src/par_sites.h`, from the exe. cmscale checks each site's bytes
before patching it.

**Locks protect the rest.** cmscale holds a lock around each of these:

- calls to `0x911ab0`;
- the buffer pool;
- any draw (`0x983ae0`) from a generator that lives in the exe's data rather than in a match object;
- the text system: the 46 non-trivial methods in the 14 string-heap vtables, and the 570 calls from the match engine
  into the 12 text functions it calls directly. The text lock uses a forwarder that works with any calling convention.
  It keeps the caller's return address aside, so the called function sees the caller's stack.

**Fixtures run in batches.** The day loop's `call 0x61b7b0` at `0x61cdd4` becomes a call to `play_async`:

1. A worker thread runs the whole of `0x61b7b0` and stops when it reaches the simulation.
2. The game thread returns to the day loop, which moves on to the next fixture.
3. The batch runs every K fixtures (K is `par_matches`), at the end of each fixture list, and whenever the game thread
   plays a fixture itself. The K simulations run together while the game thread waits. Then each worker records its
   fixture in turn, and the game thread shows its score.

Only simulations ever run at the same time, never other game code. Each worker copies the game thread's x87 control
word and MXCSR and gets a 1 MB stack, like the game thread. The match the human manager watches always stays on the
game thread.

## How it was researched

- **Reachability and writes.** A Ghidra script listed every fixed-address write in the 822 functions that the
  simulation reaches by direct calls.
- **The write watch** made all exe data and heap memory read-only during a simulation. It logged every write outside
  the match's own memory, with the writing instruction and the vtable of the object written (see
  [research methods](research-methods.md)).
- **The text system** didn't stand out in either. An early design let the game thread keep drawing during
  simulations, and it crashed in text layout. Screens read strings whose data the simulations' string heaps move.
  Locking only the heaps' methods still crashed, in array code that the engine calls directly. Both crashes led to the
  final design, where only simulations run concurrently and every engine call into text code takes the lock.

## How it was verified

To compare runs exactly, the research build pinned each match's seeds, made `time()` return a constant and replaced
the exe's `GetTickCount` with a virtual clock. Two pinned runs then gave the same fingerprint for every match, a hash of
its generator's final state. Each step of the design was checked against that baseline:

| Configuration | Matches with equal fingerprints |
|---|---|
| per-thread globals | 374 / 374 |
| plus locks | 375 / 375 |
| plus the simulation on one worker thread | 374 / 374 |
| batches, K = 1 | 371 / 371 (same order as stock) |
| batches, K = 4, in parallel, against the same schedule run one at a time | 370 / 370 |
| batches, K = 4, in parallel, two runs | 395 / 395 |

With K above 1, the results differ from a serial run. cmscale sets up all of a batch's fixtures before it records the
first of them, while the stock game sets each one up after it records the previous one. For fixtures between different
clubs on one day that shouldn't matter, but it is the one difference.

## Measured

| Three-month holiday | `par_matches=0` | 4 | 6 | 8 | 12 | 16 |
|---|---|---|---|---|---|---|
| Wall | 804 s | 633 s | 608 s | 638 s | 603 s | 593 s |

- From 4 threads up, the differences are within the noise between runs. The default is 8.
- The rest of the day sets the limit. The game thread still spends about 450 s on other work, mostly the AI's shortlist
  scan (see [prefetch](prefetch.md)).
- A batch also ends with each fixture list, so many batches hold fewer than K fixtures.
- The tests ran 1 251 background matches without a crash, and normal play confirmed it.
- Creating a new game gains nothing. Its pre-season fixtures are quick results that never reach the match engine.

## Limits

Rare situations, such as cup replays, extra time or the end of a season, may reach shared state that the watch and the
static scan didn't see. The engine makes 276 indirect calls, and the tests covered only the ones those runs reached.

# The AI's transfer searches on threads

Option: `par_scan` (default 8, up to 16; 0 turns it off).

## The question

On a long holiday, most of the game thread's time goes to the AI's transfer searches. Each club with an open position
looks for a player about every five days, and each search scores every player in the database who plays that
position. A profile of a 13-month holiday put 74% of the game thread's busy time in those searches, and most of the
rest of the cost peaks in the transfer windows and on 1 July, when contracts run out. Can one search run on several
threads?

## What the game does

```
SHORTLIST_MAN::update_club_requirements                  0x6a27f0
 └ 0x69da00   a club's requirements
    ├ get_playing_staff_for_position           0x6902d0  one search: about 60% of the requirements pass
    └ get_playing_staff_for_position_loan      0x691140  the same for loans: about 17%
```

Both searches loop over a candidate array, about 2 000 to 15 000 players, and keep the best one:

- **An argmax.** A candidate replaces the best only if its score is strictly higher, so the first maximum wins.
- **A small result.** After the loop, the main search reads only six local variables: the best score, the best
  player's id, two figures and two flags for the winner, and a fallback candidate. The loan search reads two.
- **A gate.** The expensive checks run only for candidates above the current best (loan) or the second best (main).
  Those checks write nothing that later candidates read.

That makes a split possible in principle. Give each thread a slice of the candidates, and take the result from the
first slice with the highest best.

## What the loop shares

A write watch logged every write that 300 searches made outside their own stack, about 112 000 distinct
instruction/address pairs (see [research methods](research-methods.md)). A second probe recorded which players each
search touches. Together they showed:

| Shared state | Kind | How cmscale handles it |
|---|---|---|
| A player's value, recomputed into the player object on every read; list cursors kept inside each player | scratch, but only ever on the candidate being scored | nothing: each candidate is in one slice |
| About 25 globals: context passed between evaluation functions, a one-entry date cache, the value function's statics, the date functions' results | scratch shared by all threads | per-thread copies (below) |
| A placeholder manager, which the game points at any club without a manager and then evaluates | one shared object, set for different clubs within a search | a copy per thread |
| A 200-slot cache of lightweight player views | shared cache | lock-free on a hit, a lock on a miss |
| Clubs' staff records, whose data the game rebuilds lazily (the first time it's needed each day) | rebuilt while others read it | the lock, around every call that can rebuild them |
| The global random number generator, drawn from in the fallback and for young players' values | order-dependent | a random stream per candidate (below) |
| One flag in the loan search that, once set, adds a test for every later candidate | carried over | a short serial prefix (below) |
| Guarded singleton pointers that the game fills in on first use | first-use race | filled in beforehand (below) |

## How cmscale works

**Slices.** At the search loop's first iteration, cmscale splits searches of at least 2 000 candidates into
`par_scan` slices, with at least 256 candidates each. Each worker thread copies the search function's stack frame
(locals, saved registers, return address and arguments), sets its slice's start, end and candidate pointer, loads the
game thread's registers and jumps into the loop. A hook at the loop's exit returns the worker when it reaches its own
frame. Nested searches exit at the same address with a different stack pointer, so they pass through. The game thread
waits, then copies the result variables from the first slice with the strictly highest best and continues after the
loop. Workers use the game thread's x87 control word and MXCSR.

**Per-thread copies.** `tools/scan_sites.py` lists the 397 instructions that address the shared globals
(`src/scan_sites.h`), in five blocks. Each instruction becomes a jump to a thunk: `push r; mov r,fs:[slot]`, the
instruction with `[r + offset]` in place of its absolute address, then `pop r`. Each block has its own TLS slot. On
the game thread and on every other thread, cmscale sets the slots to the globals' own addresses when the thread
starts, so they behave exactly as stock. On a search worker they point to the worker's copy, taken from the globals at
the start of each search. The thunk touches no flags. It handles memory operands with an absolute address, indexed
ones and the `moffs` moves, as well as `mov reg,imm` and `push imm` of an address.

**The placeholder manager.** Each worker gets a copy of the heap block that holds it, taken at each search, and the
game's ten loads of the placeholder pointer read the thread's own pointer. After the merge, the last value the game
set on it, in candidate order, goes back into the real one, as a serial search would leave it.

**Random numbers.** On a worker, draws from the global generator come from a generator per candidate instead. It is
seeded from one draw of the real generator at the start of the search and the candidate's number. The gated check gets
a second stream per candidate, because a slice calls it for more candidates than a serial search does. So a split
search gives exactly the result a serial search with the same streams would.

**The loan search's flag.** It only ever goes from 0 to 1, and when a search sets it, it does so within the first 64
candidates in 99.7% of cases. So the first 64 candidates run alone first, and the other slices start with the flag as
the prefix left it. If a slice sets it after all, the slices after the first one that did run again with it set.

**Clubs' staff records.** Each club keeps a list of staff records, and the game rebuilds a record's data lazily:
`0x75a190` rebuilds it if it wasn't refreshed today, and `0x7feff0` does when the club has changed since. A rebuild
frees the old data. On a holiday the game refreshes the records before the searches start, but every call that can
rebuild them takes the lock anyway.

**Creating a new game.** The game builds every club's first shortlist there (`SHORTLIST_MAN::setup`) while it also
builds clubs' staff records, and split searches crashed on records destroyed under them. cmscale keeps the searches
of that phase on the game thread.

**Memory.** The exe can address 2 GB, and creating a new game with every league already uses nearly all of it. The
first version gave its 8 workers 4 MB stacks each, and `par_matches` 64 fixture slots of 1 MB. That ran the game out of
memory ("Memory is dangerously low!") during new-game creation. The workers now have 1 MB stacks, like the game
thread, and `par_matches` has 16 slots.

**First-use races.** The compiler sets a function-local static's guard bit before it stores the value. If two workers
run such code for the first time together, the second can find the bit set and the pointer still 0. `tools/singletons.py`
lists the exe's 111 guarded singleton pointers, which the game fills in from its object registry (`src/singletons.h`).
Before each split search, cmscale fills in those the registry can already provide, as the game's first call would.
After a save is loaded, the first 20 big searches of each kind run on the game thread alone.

## How it was researched

- **The profile split by game date.** A 13-month holiday (13 Jul 2003 to 13 Aug 2004) on the 57 MB save. August 2003,
  with the transfer deadline, cost 10 s per game day, and quiet months 1.7–3.4 s. Even the season rollover was 89% the
  same searches. There was no separate mid-season or end-of-season update worth chasing.
- **The write watch.** It made all of the exe's data and the heap read-only for the length of one search and logged
  every write outside the stack. A single-step mode saw every write, not only the first per page.
- **The person probe.** It recorded the candidate at each iteration and every object the scratch writers got. Over
  600 searches in July 2003 and August 2004, the only object touched while two different candidates were scored was
  the placeholder manager.
- **Decompiles and disassembly** of both loops gave the result variables, the gates and the frame layout.
- **Lock contention.** The first version locked the placeholder and the value function, and splitting the loan search
  made holidays slower: 17 million lock waits in two weeks. A counter of contended locks pointed at the functions to
  make per-thread instead.

## How it was verified

A verify mode in the research build ran each split search on the workers, then again on the game thread with the
same random streams, and compared the six result variables:

| Two-week holiday | Searches split | Differences |
|---|---|---|
| 13–27 Jul 2003 | 15 577 transfer, 2 423 loan | 0 |
| 13–27 Aug 2004 (new season, from the end of a 13-month holiday) | 19 311 transfer, 3 689 loan | 0 |

The 13-month holiday with `par_scan=8` split 208 000 transfer and 39 000 loan searches and completed and saved
normally. With the final version, the verify runs above were repeated (again 0 differences), and creating a new game
with every league completed. Normal play confirmed it.

New-game creation was tested only after the first version shipped, and it found three problems: the memory, the
staff records and the destroyed records above. A write watch over holiday searches can't see code that only runs the
first time a club is set up.

## Measured

`par_scan=8`, with all other defaults:

| | Without `par_scan` | `par_scan=8` |
|---|---|---|
| Two weeks, 13–27 Jul 2003 | 89–91 s | 51–52 s (−43%) |
| Two weeks, 13–27 Aug 2004 | 140–141 s | 90 s (−36%) |
| 13 months, 13 Jul 2003 to 13 Aug 2004 | 1 629 s | 1 175 s (−28%) |
| Creating a new game with every league | 413 s | 431 s (its searches stay on the game thread) |

By month in the 13-month run, transfer windows took 31–40% less time and quiet months 8–26% less. 4 threads give
most of the gain, about −29% on the July fortnight, and 8 a little more.

## Limits

- Results differ from a run without `par_scan`, as they do between two stock runs. The random numbers inside a
  search come from per-candidate streams, not from the game's generator in order. Each draw is still uniform.
- The write watch and the probe covered searches from July and August, and new-game creation turned up shared state
  that holidays didn't reach. A rare path at another time of year may touch shared state the tests didn't see. If the
  game crashes or stops during processing, try `par_scan=0`.

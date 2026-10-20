# The AI shortlist scan

Option: `prefetch` (default 1). Exact: it only reads memory.

## What the game does

On a long holiday, about half the time goes into AI clubs rebuilding their transfer shortlists:

```
SHORTLIST_MAN::update_club_requirements()                0x6a27f0
 └ 0x69da00  a club's requirements pass                            53% of the holiday
    ├ SHORTLIST_MAN::get_playing_staff_for_position()      0x6902d0   45%
    └ SHORTLIST_MAN::get_playing_staff_for_position_loan() 0x691140   ~11%
```

- `get_playing_staff_for_position` loops over an array of candidates for one position. Each 0x14-byte entry starts
  with an index into the person table. Call counts showed about 15 000 candidates per search, which is nearly every
  player in the database who plays that position, and about a hundred searches per pass.
- For each candidate it runs `dynamic_cast` (see [fast-dyncast](fast-dyncast.md)), looks up the employer, then runs a
  chain of filters and scores. The employer lookup follows pointers from the person to the contract array, then to the
  contract and then to the club.

The cost is waiting for memory, not executing instructions. An exact memo of the employer lookup cut that function
from 9.4% to 5.6% of the samples, but the next filter's share rose by the same amount. The cache misses moved to
whichever code read the candidate's data first, and the holiday took no less time.

## How cmscale works

cmscale diverts the head of each candidate loop, `0x6904ad` and the loan variant's `0x691351`, through a stub. The stub
saves every register and the flags, calls the prefetcher, then runs the loop's first instructions as before.

The prefetcher looks ahead in the candidate array and prefetches in stages, so that each stage finds its pointers
already in the cache:

1. 8 candidates ahead, it prefetches the first four cache lines of the person object.
2. 4 ahead, it prefetches the storage of the person's contract array. It finds the array through the this-adjusting
   thunk in vtable slot `+0xdc`, and skips any person type whose thunk isn't one of the four it knows.
3. 2 ahead, it prefetches the contracts and their clubs.

It only reads, and it checks bounds and null pointers, so the game's results can't change.

## How it was researched

- The profiler's stack scan gave the call chain. Assert strings gave the function names, and the exe's RTTI gave the
  class names.
- Call counters on the scan's functions gave the candidate counts.
- Logging the RTTI names of live objects at the lookup showed that the job list is a `DEL_ARRAY<CONTRACT*,char>` of
  `FULL_CONTRACT` objects, each pointing at a `CLUB`.
- Tuning measured candidates per busy CPU second over the first 240 s of the holiday. Distances from 4/2/1 to 20/10/5
  gave similar rates, and 8/4/2 was the best.

## Measured

| | Candidates per CPU second |
|---|---|
| off | 2.13–2.17 M |
| on | 2.40–2.46 M (11–14% more) |

| Three-month holiday, with `fast_sqrt`, `idle_wait` and `fast_dyncast` | Wall |
|---|---|
| `prefetch=0` | 843.9 s, 863.1 s |
| `prefetch=1` | 789.0 s (6.5–8.6% less) |

- The loan search checks 12–15% more candidates per second too, which saves about 1.5% of the holiday.
- Creating a new game with every league selected spends most of its time in the same scan. There `prefetch` saves
  100 s, or 26% of the shortlisting phase.

Normal play confirmed it.

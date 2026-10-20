# `dynamic_cast` under Wine

Option: `fast_dyncast` (default 1). Exact.

## What the game does

- A three-month holiday on a large save spent 11% of its time in `msvcr71.dll`. That was Wine's builtin msvcr71. Its
  image size matched Proton's DLL, not the copy that ships with the game.
- Matched against that DLL's exports, the time was in `__RTDynamicCast` (about 5%) and `strcmp` (about 4%). The
  runtime's `dynamic_cast` walks the class hierarchy and compares mangled type names as strings.
- The exe calls `dynamic_cast` from 703 places. The hottest is `0x722b90`, a `dynamic_cast<ACTUAL_PLAYER*>(PERSON*)`
  that runs once per candidate in the AI's transfer shortlist scan (see [prefetch](prefetch.md)). A holiday makes over
  a billion casts.

## How cmscale works

cmscale hooks the import slot for `__RTDynamicCast` (`0xab61f8`).

- A cast's result depends only on four things: the object's vtable pointer, the offset to that pointer, the source
  type and the target type. The vtable pointer fixes the object's complete type and which part of the object the
  pointer points at.
- cmscale keeps a 4 096-entry table that maps those four values to the pointer adjustment, or to "cast failed". On a
  hit it returns the pointer plus the adjustment without calling the runtime.
- Null pointers and reference casts always go to the runtime, because a reference cast can throw `bad_cast`.

## How it was verified

A verify mode called the runtime on every cast and compared its answer with the table's. A full holiday made
1 174 371 867 casts. All but 141 were table hits, and none disagreed with the runtime.

## Measured

| Three-month holiday | Wall | CPU |
|---|---|---|
| `fast_sqrt=1 idle_wait=1` | 952.8 s | 945.8 s |
| plus `fast_dyncast` | 843.9 s | 837.1 s |

The holiday took 11.4% less time and ended on the same date. msvcr71 dropped from 11% of the samples to 0.3%. Most of
the time left in the hook, about 3% of the samples, is its first read of the object's vtable pointer. That read misses
the cache, and the runtime paid the same miss.

On a Continue, where the match engine dominates, it changes nothing: 7.75 s either way. Casts matter where the AI
scans people.

Normal play confirmed it. On Windows the game uses Microsoft's msvcr71, and the gain there is untested and may be
smaller.

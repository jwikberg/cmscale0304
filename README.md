```
DISCLAIMER: This project was researched and developed through Claude Code.
```

# cmscale

cmscale is a drop-in `dsound.dll` for Championship Manager 03/04, version 4.1.5. It adds:

- **Scaling and widescreen.** cmscale scales the game to any window size or to your desktop resolution and keeps the
  aspect ratio. Fullscreen no longer switches your monitor to 1024×768.
- **Faster day processing.** A three-month holiday takes about 60% less time, and creating a new game with every
  league takes 29% less. Match days are faster too.
- **Fixes and extras.** The mouse pointer stops blinking (Wine), idle screens stop using a whole CPU core, and the startup
  logos are gone. The mouse's back/forward buttons work, and highlights in the matches you watch can play faster.

It changes nothing on disk. The game's exe and saves stay as they are, and deleting `dsound.dll` restores the stock
game.

## Requirements

- **Championship Manager 03/04 patched to 4.1.5.** cmscale checks the exe when the game starts. With any other
  version it only passes sound through and changes nothing.
- **Windows 10 or later, or Wine/Proton.** The DLL uses the Universal C Runtime, which Windows 10 includes. Windows 7
  and 8 need the UCRT update.
- **A CPU with SSE2** for the speed options. Every x86 CPU from the last 20 years has it. `par_matches` needs 4 or
  more cores to help, and so does `par_scan`.

cmscale was developed and tested under Wine/Proton on Linux. Nobody has tested it on Windows yet.

## Install

1. Copy `dsound.dll` and `cmscale.ini` into the game folder, next to `cm0304.exe`.
2. On Wine or Proton, tell Wine to load the game's copy of `dsound.dll` instead of its own:
   - Steam, with the game added as a non-Steam game: set the launch options to
     `WINEDLLOVERRIDES="dsound=n,b" %command%`.
   - Lutris, Faugus, Bottles and similar launchers: add the environment variable `WINEDLLOVERRIDES=dsound=n,b`.
   - Plain Wine: run `WINEDLLOVERRIDES="dsound=n,b" wine cm0304.exe`.
3. Start the game.

To uninstall, delete `dsound.dll` and `cmscale.ini`.

## Options

Every option goes in `cmscale.ini`, under `[cmscale]`, and you can turn each one off on its own. The defaults below
apply when the ini is missing or leaves an option out.

### Display

| Option | Default | What it does | Docs |
|---|---|---|---|
| `zoom` | `1.0` | In a window, the game lays out at the window size divided by `zoom`, and cmscale scales the result to fill the window. `1.5` makes everything 50% bigger. No effect in `-fullscreen` | [scaling](docs/scaling.md) |
| `borderless` | `1` | `-fullscreen` keeps your desktop resolution. A borderless window covers the screen, cmscale scales the game into it, and it doesn't minimise when you Alt-Tab away | [scaling](docs/scaling.md) |
| `renderer` | `opengl` | `opengl` scales on the GPU and uploads only the parts of the screen that changed. `gdi` is a slower CPU fallback, which cmscale also uses when OpenGL isn't available | [scaling](docs/scaling.md) |
| `filter` | `sharp` | `sharp` gives crisp pixels with smooth edges, `smooth` is bilinear and `nearest` is blocky | [scaling](docs/scaling.md) |
| `integer` | `0` | `1` scales only by whole multiples (2×, 3× and so on), with bigger black bars | [scaling](docs/scaling.md) |
| `vsync`, `max_fps` | `0`, `120` | OpenGL only. `vsync=1` waits for vblank on each redraw, and `max_fps` caps redraws per second | [scaling](docs/scaling.md) |
| `cursor_fix` | `1` | Stops the mouse pointer blinking when it moves over buttons, which happens under Wine | [cursor](docs/cursor.md) |

### Exact speed-ups

These options give the same results as the stock game. They replace slow code with faster code that computes the same
thing, or they change only what the CPU does while it waits.

| Option | Default | What it does | Docs |
|---|---|---|---|
| `idle_wait` | `1` | Idle screens use 3–7% of a core instead of 100%. Input and processing are unaffected | [idle-wait](docs/idle-wait.md) |
| `fast_sqrt` | `1` | The match engine's square roots use the CPU's own instruction. Match days take about 11% less CPU | [fast-sqrt](docs/fast-sqrt.md) |
| `fast_rand` | `2` | A faster random number generator that produces the same numbers in the same order. `1` saves about 2% of a match day, and `2` saves about 1% more | [fast-rand](docs/fast-rand.md) |
| `fast_dyncast` | `1` | Remembers the results of C++ type checks instead of comparing type names every time. Long holidays under Wine take about 11% less time | [fast-dyncast](docs/fast-dyncast.md) |
| `prefetch` | `1` | Loads the next candidates' data early during the AI's transfer and loan searches, which then check about 12% more candidates per second | [prefetch](docs/prefetch.md) |

### Parallel processing

| Option | Default | What it does | Docs |
|---|---|---|---|
| `par_matches` | `8` | Simulates the day's background matches on this many threads, up to 16. `0` turns it off. A three-month holiday takes about 21% less time | [par-matches](docs/par-matches.md) |
| `par_scan` | `8` | Splits the AI's transfer and loan searches, which score every player in the database, over this many threads, up to 16. `0` turns it off. Long holidays take about 28% less time, transfer windows about 35% less | [par-scan](docs/par-scan.md) |

Results with these options differ from a run without them. Two stock runs differ from each other too, because the game
seeds each match from the clock and its random number generator depends on timing. With `par_matches`, the simulation
of each match is unchanged, but cmscale sets up a whole fixture list before it records the first match. With
`par_scan`, each search finds the same kind of best candidate, but its random numbers come from a stream per candidate
instead of the game's generator in order. Set either option to `0` to keep the stock behaviour.

### Convenience

| Option | Default | What it does | Docs |
|---|---|---|---|
| `skip_splash` | `1` | Skips the startup logos. The main menu appears after about 1.5 s instead of about 20 s | [skip-splash](docs/skip-splash.md) |
| `mouse_nav` | `1` | The mouse's back/forward side buttons and the keyboard's browser keys go back and forward through screens, like the ◀ ▶ arrows | [mouse-nav](docs/mouse-nav.md) |
| `match_hl_speed` | `0` | Sets the speed of highlights in a match you watch. `0` is stock (Real Time at any match speed), `1` is Fast (3×) and `2` is Very Fast (8×). Replays keep their own speed | [match-speed](docs/match-speed.md) |
| `match_speed_max` | `8` | Sets how fast Very Fast in the match Speed menu is, in multiples of Medium. Stock is 8 and the maximum is 1000. After changing it, pick Very Fast again in a match | [match-speed](docs/match-speed.md) |

## Measured speed-ups

These come from Proton on a 20-thread AMD Ryzen AI 9 365, with one or a few runs each. Holidays vary by 2–3% between
runs.

| Benchmark | Stock | cmscale defaults |
|---|---|---|
| Three-month holiday on a 57 MB save (England down to the Third Division, plus Spain and Sweden) | 982 s | 375–379 s (−62%) |
| 13-month holiday on the same save, to the next August | | 1 175 s, against 1 629 s without `par_scan` (−28%) |
| Creating a new game with every league selected | 555 s | 392 s (−29%) |
| Idle screen | ~100% of a core | 3–7% |

Each option's doc gives its own share.

## Building

The build needs mingw-w64 (`i686-w64-mingw32-gcc` and `i686-w64-mingw32-windres`):

```sh
./mingw-build.sh  # writes dsound.dll next to the script
```

### Versions and releases

Releases are git tags named `vX.Y.Z`. The build stamps the version into the DLL, where Explorer shows it under
Properties > Details. `tools/version.sh` works it out from the tags:

- A build of a release tag gets the release version, such as `1.2.0`.
- Any other build gets a prerelease of the next patch version, such as `1.2.1-dev.5+g1a2b3c4`: 5 commits after
  `v1.2.0`, at commit `1a2b3c4`.

`CMSCALE_VERSION=1.3.0 ./mingw-build.sh` sets the version by hand.

GitHub Actions builds every push to main and every pull request. Each run's page offers `cmscale-<version>.zip`, with
the DLL, the ini, this README and the license.

To make a release, run the **Release** workflow from the Actions tab on main. Pick `patch`, `minor` or `major`, or
type an exact version. The workflow builds the zip and creates a draft release with generated notes. Review the draft
on the Releases page and publish it. Publishing creates the `vX.Y.Z` tag, and later builds of main count from it.

`src/par_sites.h` lists the 1 145 game instructions that `par_matches` redirects. `tools/par_sites.py` generates it
from the 4.1.5 exe with GNU `objdump`:

```sh
python3 -I tools/par_sites.py cm0304.exe > src/par_sites.h
```

`par_scan` uses two more generated lists: `src/scan_sites.h`, the 397 instructions that address the globals it gives
each thread its own copy of, and `src/singletons.h`, the 111 singleton pointers it fills in before a split search:

```sh
python3 -I tools/scan_sites.py cm0304.exe > src/scan_sites.h
python3 -I tools/singletons.py cm0304.exe > src/singletons.h
```

You only need to run them again if you change a generator.

## How it works

The game imports one function from `dsound.dll`. Windows looks for `dsound.dll` in the game folder before the system
folder, so the game loads cmscale at startup. cmscale passes sound calls on to the real DirectSound. If the exe is
4.1.5, it also patches the game in memory, in two ways:

- **Import hooks.** cmscale points a few of the game's Windows calls at its own versions: drawing, window size, the
  mouse cursor and the message loop.
- **Code patches.** A handful of game functions get a jump to a faster equivalent in the DLL, or a small stub that runs
  extra code first. cmscale compares the original bytes before each patch and skips the patch if they differ.

Each doc covers one option: how it works, how it was found and how it was checked.

- [scaling](docs/scaling.md): the present hook, zoom, borderless fullscreen and the OpenGL renderer
- [cursor](docs/cursor.md): the blinking pointer
- [idle-wait](docs/idle-wait.md): the busy message loop
- [fast-sqrt](docs/fast-sqrt.md): the match engine's distance function
- [fast-rand](docs/fast-rand.md): the random number generator's multiply and refill
- [fast-dyncast](docs/fast-dyncast.md): `dynamic_cast` under Wine
- [prefetch](docs/prefetch.md): the cache misses in the AI's shortlist scan
- [par-matches](docs/par-matches.md): background matches on threads
- [par-scan](docs/par-scan.md): the AI's transfer and loan searches on threads
- [skip-splash](docs/skip-splash.md): the startup logos
- [mouse-nav](docs/mouse-nav.md): back and forward through the screen history
- [match-speed](docs/match-speed.md): highlight speed and Very Fast
- [research methods](docs/research-methods.md): the tools and test setup behind all of the above

## Known limitations

- `zoom` has no effect in `-fullscreen`. There the game always lays out at its own size, 1024×768 by default, and
  cmscale scales that picture up.
- The mouse pointer is the game's 32×32 hardware cursor, and cmscale doesn't scale it.
- `par_matches` was checked on match days, a three-month holiday and new-game creation. Rarer situations, such as cup
  replays, extra time or the end of a season, may run code that those tests didn't reach. If the game crashes during
  processing, try `par_matches=0`.
- `par_scan` was checked with a verify mode on two two-week holidays (July 2003 and August 2004) and run through a
  13-month holiday, but the analysis behind it covered searches from July and August. If the game crashes during
  processing, try `par_scan=0`.

## Disclaimer

cmscale is an unofficial fan project, not affiliated with or endorsed by Sports Interactive or Eidos. It contains no
game data and changes no files. You need your own copy of the game.

## License

MIT
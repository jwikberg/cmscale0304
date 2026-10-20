# Research methods

This page describes the tools and test setup behind the other docs. All addresses are virtual addresses in
`cm0304.exe` 4.1.5. The exe has no relocation table and always loads at `0x400000`, so the addresses never change.

## The target

| | |
|---|---|
| File | `cm0304.exe` 4.1.5, 9 785 344 bytes |
| SHA-256 | `68c5b94a890c3e525dc81ff44b8ce9f9f3134a08b912c41ba81487da22a69f80` |
| Build | MSVC .NET 2003, CRT `msvcr71.dll`, PE timestamp `0x4048900c` (2004-03-05) |
| Rendering | GDI only. The game draws into a software framebuffer and copies it to the window with `StretchDIBits`. No DirectDraw or Direct3D |
| Threads | One. The whole game runs on its main thread |

cmscale checks the image base and the PE timestamp before it patches anything.

## Static analysis

- **Disassembly.** `i686-w64-mingw32-objdump -d -M intel cm0304.exe` gave the full disassembly. Cross-references to
  the import slots (IAT) found every caller of each Windows function.
- **Decompiles.** Headless Ghidra 12.1.2 decompiled the stock exe.
- **Class names.** The exe keeps MSVC RTTI, which names all 4 203 vtables (`PERSON`, `ACTUAL_PLAYER`, `SHORTLIST_MAN`,
  `DEL_ARRAY<CONTRACT*,char>` and so on). Assert strings inside functions give many method names, such as
  `SHORTLIST_MAN::get_playing_staff_for_position`.
- **Reachability.** A Ghidra script walked every function that the match simulation reaches by direct calls, 822 in
  all, and listed their writes to fixed addresses.

## Runtime tools

These tools lived in a research build of cmscale. The public DLL doesn't include them.

- **A sampling profiler.** A thread suspends the game thread up to 1 000 times a second and records `eip` plus every
  return address into the exe that it finds on the stack. The exe omits frame pointers, so the profiler scans the
  stack and counts a value only if a `call` instruction ends right before it. Every 250 ms it also records the game
  thread's CPU time and how often the message loop and the present ran. That tells computing apart from waiting and
  idling. Linux `perf`, attached to the Wine process, gave the same ranking and shares.
- **Call counters.** A trampoline counts the calls to any function.
- **A write watch.** For the length of one match simulation, or one club's shortlisting, the watch makes the exe's
  data and all heap memory read-only. It then logs every write outside the match's own memory, with the writing
  instruction, the registers and the vtable of the object written. An option single-steps each write, so the watch
  sees every write instead of only the first per page.
- **Hardware data breakpoints.** The debug registers on one field found every instruction that reads it.
- **Match fingerprints and pinned seeds.** The research build logged one line per simulated match, with its seeds and
  a hash of its random number generator's final state. With the seeds and the clock pinned, two runs gave identical
  fingerprints, so a change could be checked match by match.
- **An autopilot.** It loads a save, presses Continue N times or goes on holiday until a date, then saves and quits,
  with no input. That made the benchmarks repeatable.

## Exactness tests

Every code replacement that claims the same results was tested against the game's own machine code. A test program
copied the relevant code pages out of the exe and loaded two copies under Wine, one unchanged and one with cmscale's
patch applied. It called both with the same inputs and compared their outputs: every register that matters, and any
state. A control run with a deliberately broken patch showed that the test catches a difference.

## Test setup

- Automated runs used Proton-CachyOS (wine-11.0) on Xvfb with Mesa llvmpipe. A helper exe in the same prefix sent real
  input with `SendInput`, because posted messages don't trigger hover or clicks.
- Display bugs that Xvfb doesn't show were tested on the real desktop, KWin on Xwayland with radeonsi.
- Every option that is on by default went through normal play on that desktop before it became the default.
- The host was an AMD Ryzen AI 9 365 with 4 Zen 5 and 6 Zen 5c cores, 20 threads.
- Two saves were used. The first is a pre-season save (Fulham, 13 Jul 2003), whose first Continue runs to 16 Jul
  19:45. The second is a 57 MB save with England down to the Third Division, Spain down to the Second and Sweden's
  First, with an unemployed manager. It was used for a three-month holiday from 13 Jul to 20 Oct 2003.

Single runs of long benchmarks vary by 2–3%, because the game isn't deterministic. It seeds each match from
`GetTickCount`.

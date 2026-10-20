# The startup logos

Option: `skip_splash` (default 1).

## What the game does

- Startup shows four logos in turn. Each fades in, stays for 3 s and fades out. The first is a black screen for one
  second, then come Sports Interactive, Eidos and "Let's Kick Racism Out Of Football". The main menu appears about
  20 s after launch.
- At `0x44ab65`, startup (`0x44a9b0`) installs the logo player `0x449a90` as the main window's per-frame callback, at
  `+0x48`. `0x997490` calls it every frame until it returns false, and then the main menu runs.
- The player's table lists each logo with its duration in milliseconds and whether it can be skipped. All four can.
  Any input sets the skip flag `0xd4c78a`, and the player then jumps past every logo that can be skipped. The player's
  first call clears the flag.

## How cmscale works

cmscale checks the instruction that installs the callback (`mov dword [edi+0x48], 0x449a90`) and rewrites it to install
a wrapper. The wrapper calls the original player and then sets the skip flag. The player skips at once, as if a key had
been pressed when the first logo appeared.

## How it was researched

A Ghidra decompile of `0x449a90` showed the logo table, the skip flag and how the player handles it. Screenshots every
0.5 s during startup gave the timings.

## Measured

The main menu appears 1.5 s after launch instead of about 20 s. No logo is drawn, and the screen stays black for the
first second while the window comes up. Loading a save and playing work as before.

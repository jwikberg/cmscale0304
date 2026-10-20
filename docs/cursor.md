# The blinking mouse pointer

Option: `cursor_fix` (default 1).

## The problem

Under Wine, the pointer flashes whenever it moves onto something new, such as a button or a list row.

## What the game does

- The game uses a hardware cursor. `0x9e37a0` builds a 32×32 image and passes it to `CreateIconIndirect`.
- The cursor object's show method (vtable `0xc53e88`, slot `+4`, `0x9e3720`) calls
  `ShowCursor(FALSE); SetCursor(h); ShowCursor(TRUE)`. The game calls it each time the hover target changes, about
  twice a second while the mouse moves.
- The window class has no cursor (`hCursor = NULL`), and the game doesn't handle `WM_SETCURSOR`.

On Windows the hide and show inside one call never reach the screen. Under Wine each `ShowCursor` updates the X cursor
at once, so every cursor change blanks the pointer for a moment.

## How cmscale works

- cmscale hooks `ShowCursor` (`0xab6260`). It keeps the game's display count itself and applies it to the real cursor
  only from the message loop, in its `PeekMessageA` hook. A hide followed by a show within one call cancels out, and a
  real hide still takes effect at the next pass of the loop.
- The `SetCursor` hook (`0xab6264`) skips calls that would set the cursor that is already showing.
- On `WM_SETCURSOR` in the client area, cmscale sets the game's current cursor.

## How it was researched

The show and hide methods came from the cursor object's vtable in the disassembly. A test setup moved the mouse over
the main menu and counted the calls. The game made about 2 `SetCursor` calls a second, each with a `ShowCursor` pair,
and none while the mouse was still. So the game sets the cursor when the hover target changes, not every frame.

## Verified

Screenshots don't capture a hardware cursor, so the check was normal play on the real desktop. The pointer no longer
blinks.

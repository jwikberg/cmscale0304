# Mouse back/forward buttons

Option: `mouse_nav` (default 1).

## What the game does

- **The screen history** (`0x9c34b0`) is a list with a current index. A `goto` message with `scin` set to −3 goes to
  the previous screen, and −2 goes to the next.
- Three inputs send those messages, all through the window's message dispatcher `0x99bfa0`:

  | Input | Function | Condition |
  |---|---|---|
  | the ◀ ▶ arrows under the date (actions `PREV` and `NEXT`) | `0x99acd0` | the screen didn't handle the action itself |
  | a key press (`keyp`) | `0x99b8e0` | game key 12 or 13 (Left or Right) with Alt |
  | the mouse wheel | `0x99a890` | with Shift |

- **Keys.** On every message the window procedure stores the Shift, Ctrl and Alt state from `GetKeyState` in the window
  object's `+0x40`. `WM_KEYDOWN` stores the game's key code at `+0x3c`. On the next frame, `0x9904b0` turns a non-zero
  code into a `keyp` message.
- **Alt+Left does nothing on Windows.** With Alt held, Windows sends `WM_SYSKEYDOWN`, and the window procedure ignores
  it. The handler was probably written for the Mac version, where Option+arrow arrives as a normal key.
- The window procedure handles no `WM_XBUTTON*` or `WM_APPCOMMAND` messages.

## How cmscale works

cmscale's window procedure handles the side buttons before the game sees them:

- On `WM_XBUTTONDOWN`, or its double-click variant, it stores game key 12 (back button) or 13 (forward button) at
  `+0x3c` and Alt at `+0x40`, and doesn't pass the message on. It also swallows `WM_XBUTTONUP`.
- `WM_APPCOMMAND` with `BROWSER_BACKWARD` or `BROWSER_FORWARD` does the same. That covers the browser keys on keyboards,
  and mice whose driver sends the side buttons this way.
- Every message the game handles rewrites `+0x40` from `GetKeyState`. So until the frame takes the key, cmscale sets
  Alt again after each one.

The press then follows the game's own `keyp` path at the usual point in the frame, as Alt+Left or Alt+Right would.

## How it was researched

Ghidra decompiles of the history list and the key handler showed the Alt+arrow binding. The window procedure's jump
table showed why that binding is unreachable. A live test with `SendInput` confirmed that Alt+Left does nothing in the
stock game.

## Verified

With a save loaded, the test opened News after a few other screens:

| Input | Result |
|---|---|
| back button | the previous screen, and ▶ lights up |
| forward button | back to News |
| Alt+Left | no change, as predicted |
| back button with `mouse_nav=0` | no change |

A real mouse under Wine/Proton confirmed it. There, X11 and Wayland buttons 8 and 9 arrive as the side buttons. Nobody
has tested it on Windows.

## Limits

- A widget that takes key presses itself, such as a text field, might swallow the press. None did on the screens tried.
- During day processing, the press waits for the next frame, like any key.

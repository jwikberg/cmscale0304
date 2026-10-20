# Scaling, zoom and borderless fullscreen

Options: `zoom`, `borderless`, `renderer`, `filter`, `integer`, `vsync`, `max_fps`.

## What the game does

- **One call puts every pixel on the window.** The game draws its whole UI in software into a 32 bpp top-down
  framebuffer. It then copies the changed rectangle to the window 1:1 with `StretchDIBits` at `0x987e25`. That is the
  exe's only `StretchDIBits` call. The only other drawing calls are for the cursor and for printing.
- **The window object** has vtable `0xc48214`, and `GetWindowLongA(hwnd, 0)` returns a pointer to it. The framebuffer
  is at `+0x64` and its size at `+0x7a/+0x7c`. The minimum layout size is at `+0x7e/+0x80` and the maximum at
  `+0x82/+0x84`.
- **The layout range.** Startup (`0x44a9b0`) gives the app a default size of 1024×768, a minimum of 1024×768 (800×600
  with `-small_screen`) and a maximum of 1600×1200.
- **Windowed mode already reflows.** On `WM_SIZE` the game reallocates its framebuffer at the new client size and lays
  the UI out again, within that range. It reads the client size only from `WM_SIZE` and `GetClientRect`.
- **Fullscreen changes the display mode.** The game enumerates display modes and calls `ChangeDisplaySettingsA`, which
  drives a modern monitor at 1024×768. It also minimises itself with `CloseWindow` when it loses focus.
- **Mouse input** comes only from the `lParam` of `WM_MOUSEMOVE`. The exe imports neither `GetCursorPos` nor
  DirectInput.

## How cmscale works

cmscale replaces these import slots:

| Slot | Hook |
|---|---|
| `StretchDIBits` (`0xab605c`) | scales the framebuffer to the window, keeps its aspect ratio and adds black bars |
| `RegisterClassA` (`0xab628c`) | subclasses the game's window class `"main"` and adds `CS_OWNDC`, which OpenGL needs |
| `GetClientRect` (`0xab62d4`) | returns the logical size |
| `CreateWindowExA`, `MoveWindow` | in a window, multiplies the client size by `zoom` and fits it to the work area. In fullscreen, covers the monitor |
| `ChangeDisplaySettingsA`, `CloseWindow` | do nothing (`borderless=1`) |

cmscale's window procedure runs before the game's:

- **`WM_SIZE`.** cmscale tells the game a logical size, the real size divided by `zoom` and clamped to the game's
  minimum and maximum. The game lays out at that size, and cmscale scales the result to the real window.
- **Mouse messages.** cmscale subtracts the bar offset from the coordinates, divides by the scale and clamps the result
  to the framebuffer.
- **`WM_GETMINMAXINFO`.** cmscale drops the game's own size limits. The game is never told less than its minimum, and
  the scaler shrinks the picture instead.

The OpenGL renderer is the default:

- The game thread copies only the changed rectangle into a staging buffer and wakes a render thread.
- The render thread owns the GL context. It uploads the changed rectangles with `glTexSubImage2D`, keeping a list of
  up to 32, and draws one textured quad.
- `max_fps` caps the redraws. Updates that arrive in between go into the next redraw.
- `filter=sharp` is a small shader. It samples the nearest source pixel inside each pixel and blends one output pixel
  wide at the edges.

The GDI renderer stretches the whole frame but clips it to the scaled changed rectangle. Stretching only the rectangle
would leave seams, and GDI computes only the pixels inside the clip.

## How it was researched

- Static analysis found the single present call, the window object's fields and the input path. The sources were
  cross-references to the import slots, and Ghidra decompiles of the window procedure `0x9894d0` and of window
  creation `0x9889c0`.
- **The first GDI version was too slow in game.** It stretched the whole frame on every present. Under Wine, a full
  1024×768 to 2400×1800 HALFTONE stretch took 33 ms, and the same stretch clipped to 240×100 took 0.2 ms. That led to
  the clipping, and then to the OpenGL renderer.
- **The game hung when resizing or switching to fullscreen.** A watchdog that sampled the game thread's stack traced
  the hang to a bug in cmscale. When the framebuffer size crossed a power of two, cmscale reallocated the texture while
  `GL_UNPACK_ROW_LENGTH` still held the old width. The driver read past the end of the buffer. Wine turned the fault
  into an error return, and the next GL call never returned. `WINEDEBUG=+opengl` showed the failing `glTexImage2D`.
  cmscale now resets the unpack state first.
- **The render thread** came from that investigation and stayed. If a driver call ever stalls, only the picture waits,
  and the game keeps running.
- **The interface was cropped after resizing.** The game sets a minimum window size (the minimum layout times `zoom`),
  and some Wayland and X window managers ignore it. Wine then kept drawing a window larger than the one on screen.
  Dropping the game's limits fixed it.

## Verified

- A harness under Xvfb drove the present hook with a synthetic frame and read back the front buffer. The orientation,
  the bars and the position of the changed rectangle were all correct.
- On the real desktop (KWin on Xwayland, radeonsi), windows resized between 300×200 and 2400×1600, and switched between
  fullscreen and windowed several times, with no stall.
- Normal play confirmed it.

## Limits

- `zoom` has no effect in `-fullscreen`. There the game allocates its framebuffer from its own requested size, not
  from `WM_SIZE`. It lays out at 1024×768, or at its saved fullscreen size, and cmscale scales that up.
- `integer=1` can add bars on all four sides when the next whole multiple doesn't fill either dimension.
- The cursor stays 32×32 physical pixels.

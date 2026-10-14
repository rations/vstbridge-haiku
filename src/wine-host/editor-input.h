// vstbridge: a Wine VST/CLAP plugin bridge
// Copyright (C) 2020-2024 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// Haiku-only. Feeds input into a plugin's editor window.
//
// The editor is not embedded in the host's window and there is no display
// server delivering events to it, so this side has to play the part one would.
// Wine's own graphics drivers do that through `NtUserSendHardwareInput()`,
// which is what these wrap.
//
// **The implementation is C, not C++, and deliberately so.** It needs Wine's
// internal `ntuser.h`, which does not compile as C++: it uses `class` as a
// parameter name and relies on implicit conversions from `void*` that C++
// rejects. Keeping it in its own C translation unit is the same split the
// Haiku graphics driver uses, and avoids either patching a Wine header or
// building the whole host with `-fpermissive`.

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Send a mouse event to `window`.
 *
 * @param screen_x Where the pointer is, in **screen** coordinates. Unlike the
 *   public `SendInput()`, this path takes raw pixels rather than coordinates
 *   normalized into a 0..65535 range.
 * @param screen_y As `screen_x`.
 * @param flags `MOUSEEVENTF_*`, which must include `MOUSEEVENTF_ABSOLUTE`
 *   for the coordinates above to be read as given.
 * @param mouse_data The wheel delta for `MOUSEEVENTF_WHEEL`, otherwise zero.
 */
BOOL vstbridge_send_mouse_input(HWND window,
                                int screen_x,
                                int screen_y,
                                DWORD flags,
                                DWORD mouse_data);

/**
 * Hand Wine any mouse motion it is still holding back for `window`.
 *
 * Since Wine 11.13, `NtUserSendHardwareInput()` coalesces plain pointer motion
 * per thread and only passes it on with the next button or wheel event, or
 * when the driver ends a batch of events with a lone
 * `MOUSEEVENTF_MOVE_NOCOALESCE` input, as winex11 and winewayland do. Without
 * this, a drag or a hover would only reach the plugin on the next click. Call
 * it once after each batch of mouse events.
 */
BOOL vstbridge_flush_mouse_input(HWND window);

/**
 * Send a key event to `window`.
 *
 * @param virtual_key A Win32 `VK_*` code.
 * @param flags `KEYEVENTF_*`; pass `KEYEVENTF_KEYUP` for a release.
 */
BOOL vstbridge_send_key_input(HWND window, WORD virtual_key, DWORD flags);

#ifdef __cplusplus
}
#endif

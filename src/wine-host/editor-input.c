/*
 * vstbridge: a Wine VST/CLAP plugin bridge
 * Copyright (C) 2020-2024 Robbert van der Helm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * See `editor-input.h` for why this is C rather than C++.
 */

#include <windows.h>

#include <ntuser.h>

#include "editor-input.h"

BOOL vstbridge_send_mouse_input(HWND window,
                                int screen_x,
                                int screen_y,
                                DWORD flags,
                                DWORD mouse_data) {
    INPUT input;

    memset(&input, 0, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = screen_x;
    input.mi.dy = screen_y;
    input.mi.dwFlags = flags;
    input.mi.mouseData = mouse_data;

    return NtUserSendHardwareInput(window, 0, &input, 0);
}

BOOL vstbridge_send_key_input(HWND window, WORD virtual_key, DWORD flags) {
    INPUT input;

    memset(&input, 0, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtual_key;
    input.ki.dwFlags = flags;

    return NtUserSendHardwareInput(window, 0, &input, 0);
}

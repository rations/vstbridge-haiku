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

// Haiku-only. This is the Wine-side half of the editor bridge: it reads the
// pixels a hosted plugin has drawn into its own editor window so they can be
// handed to the native side and painted into jackDAW's FX window.
//
// This exists because a plugin editor cannot be embedded into the host window
// on Haiku at all. Every application gets its own view token space and
// app_server refuses a parent view token belonging to another team, so there is
// no equivalent of X11 reparenting or XEmbed. Copying pixels across a process
// boundary is the remaining option.
//
// Two properties of Wine make that copy cheap and complete, and neither holds
// on real Windows:
//
//  * A window's painting lands in an in-memory backing store (a
//    `window_surface`) rather than straight into a screen framebuffer, so
//    reading the window's DC yields the pixels the plugin drew regardless of
//    what is or is not on screen.
//  * Child windows never get a surface of their own -- win32u sets
//    `needs_surface = FALSE` for them and they draw into their top-level
//    ancestor's surface. A single `BitBlt()` from the top-level window's DC
//    therefore captures the complete editor, child controls included, with none
//    of the `PrintWindow()` gymnastics that capturing a Windows window
//    requires.
//
// A window still has to be `WS_VISIBLE` for win32u to give it a surface and for
// Win32 to keep delivering `WM_PAINT`. Hiding the editor window is therefore
// not an option; keeping it visible while stopping it from reaching the screen
// is a graphics driver concern, not this file's.

#include <cstdint>
#include <string>

#include <windows.h>

#include "../common/logging/common.h"

/**
 * Grabs the contents of a plugin's editor window into a 32-bit top-down DIB.
 *
 * The DIB is allocated once per size and reused for every frame, so a steady
 * state capture performs no allocation. Everything here runs on the GUI thread
 * and must never be reached from the audio thread.
 */
class EditorCapture {
   public:
    /**
     * Set up capturing for an editor window. No buffer is allocated until the
     * first `capture()` call, since the plugin's editor size is not yet known
     * at construction time.
     *
     * @param logger A logger instance created with
     *   `Logger::create_wine_stderr()`.
     * @param window The plugin's top-level editor window.
     */
    EditorCapture(Logger& logger, HWND window) noexcept;

    ~EditorCapture() noexcept;

    EditorCapture(const EditorCapture&) = delete;
    EditorCapture& operator=(const EditorCapture&) = delete;
    EditorCapture(EditorCapture&&) = delete;
    EditorCapture& operator=(EditorCapture&&) = delete;

    /**
     * Copy the editor window's current contents into our buffer, reallocating
     * it first if the editor has been resized since the last call.
     *
     * The size is taken from the window's client area rather than being passed
     * in, so a resize the plugin performed on its own cannot desynchronize the
     * buffer from what is actually being drawn.
     *
     * @return Whether a frame was captured. A `false` return is not fatal --
     *   the window may simply have no client area yet -- and leaves any
     *   previously captured frame intact.
     */
    bool capture() noexcept;

    /**
     * A pointer to the most recently captured frame, or `nullptr` if nothing
     * has been captured yet. The layout is top-down 32 bits per pixel, `BGRX`
     * in memory order, with no padding between rows: `stride()` always equals
     * `width() * 4`.
     *
     * The pointer is invalidated by any `capture()` call that resizes the
     * buffer.
     */
    const uint8_t* pixels() const noexcept { return pixels_; }

    uint16_t width() const noexcept { return width_; }
    uint16_t height() const noexcept { return height_; }

    /**
     * The number of bytes per row, and the number of bytes in the whole frame.
     */
    uint32_t stride() const noexcept { return uint32_t{width_} * 4; }
    uint32_t size_bytes() const noexcept { return stride() * height_; }

    /**
     * Write the most recently captured frame out as a BMP file. This exists to
     * verify the capture path by eye before there is anything on the native
     * side able to display a frame, and is only reached when
     * `VSTBRIDGE_CAPTURE_DUMP` is set.
     *
     * @return Whether the file was written.
     */
    bool dump_bmp(const std::string& path) const noexcept;

   private:
    /**
     * Throw away the current buffer and allocate one for this size. Sets
     * `pixels_` to `nullptr` and the dimensions to zero if anything fails.
     */
    void reset_buffer(uint16_t width, uint16_t height) noexcept;

    /**
     * Release the DIB and the memory DC, leaving the object in the same state
     * as a freshly constructed one.
     */
    void release_buffer() noexcept;

    Logger& logger_;

    /**
     * The plugin's top-level editor window. Not owned.
     */
    HWND window_;

    /**
     * The memory DC `dib_` is selected into, and the bitmap the window is
     * blitted into. Both are null until the first successful `reset_buffer()`.
     */
    HDC memory_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    /**
     * Whatever was selected into `memory_dc_` before `dib_` was, so it can be
     * put back before the DC is deleted.
     */
    HGDIOBJ previous_bitmap_ = nullptr;

    /**
     * The pixel data backing `dib_`. Owned by GDI, freed along with the bitmap.
     */
    uint8_t* pixels_ = nullptr;

    uint16_t width_ = 0;
    uint16_t height_ = 0;

    /**
     * Set after a capture failure has been logged so a permanently failing
     * window cannot flood the log at the frame rate.
     */
    bool logged_capture_failure_ = false;
};

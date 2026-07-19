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

// This is the Haiku counterpart to `editor.h`, which is X11/XCB-only. It is
// reached through `editor.h` under `#ifdef __HAIKU__` so no call site has to
// care which one it got.
//
// There is no embedding here, and there cannot be. Haiku gives every
// application its own view token space, and app_server refuses a parent view
// token that belongs to another team, so nothing equivalent to XEmbed or X11
// reparenting exists: a BView cannot be adopted by a window in another process.
// The bitmap-over-shared-memory design that works around this is phase 3 work.
//
// For now the plugin's own Wine window is simply shown as a normal top-level
// window. It is fully interactive -- phase 0 confirmed a Windows plugin GUI
// renders and responds to input under Wine on Haiku -- it just floats
// separately from jackDAW rather than sitting inside it. That keeps phase 1
// focused on audio, IPC and realtime behaviour with no GUI risk attached.

#include <memory>
#include <optional>
#include <string>

#include <windows.h>

#include <function2/function2.hpp>

#include "../common/configuration.h"
#include "../common/logging/common.h"
#include "utils.h"

/**
 * The name of the Win32 window class we'll use for the Win32 window that the
 * plugin can embed itself in.
 */
constexpr char vstbridge_window_class_name[] = "vstbridge plugin";

/**
 * A RAII wrapper around windows created using `CreateWindow()` that will post a
 * `WM_CLOSE` message to the window's message loop so it can clean itself up
 * later. Directly calling `DestroyWindow()` might hang for a second or two, so
 * deferring this increases responsiveness.
 *
 * This is the same idea as the X11 build's `DeferredWin32Window`, minus the X11
 * connection it has no use for here.
 */
class DeferredWin32Window {
   public:
    DeferredWin32Window(MainContext& main_context, HWND window) noexcept;

    ~DeferredWin32Window() noexcept;

    DeferredWin32Window(const DeferredWin32Window&) = delete;
    DeferredWin32Window& operator=(const DeferredWin32Window&) = delete;
    DeferredWin32Window(DeferredWin32Window&&) = delete;
    DeferredWin32Window& operator=(DeferredWin32Window&&) = delete;

    const HWND handle_;

   private:
    MainContext& main_context_;
};

/**
 * Creates the Win32 window a hosted plugin embeds its own editor into. Mirrors
 * the public surface of the X11 `Editor` so the bridges do not need to know
 * which platform they were built for, but performs no reparenting: the window
 * is an ordinary top-level Wine window.
 */
class Editor {
   public:
    /**
     * Open a top-level window and create a handle to it that the hosted plugin
     * can draw into.
     *
     * @param main_context The application's main IO context running on the GUI
     *   thread, used to defer window destruction.
     * @param config This instance's configuration.
     * @param logger A logger instance created with
     *   `Logger::create_wine_stderr()`.
     * @param parent_window_handle The handle the VST host passed for the editor
     *   to embed itself into. **Ignored here** -- it refers to a view owned by
     *   another team, which this process cannot attach to. Kept in the
     *   signature so the call sites stay identical across platforms.
     * @param timer_proc A function to run on a timer. Used by VST2 plugins to
     *   periodically call `effEditIdle` from the message loop thread.
     */
    Editor(MainContext& main_context,
           const Configuration& config,
           Logger& logger,
           const size_t parent_window_handle,
           std::optional<fu2::unique_function<void()>> timer_proc =
               std::nullopt);

    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;
    Editor(Editor&&) = delete;
    Editor& operator=(Editor&&) = delete;

    /**
     * Resize the window to this new size, called whenever the plugin requests a
     * resize or the host resizes the window through the plugin API.
     */
    void resize(uint16_t width, uint16_t height);

    /**
     * Show the window, called after the plugin has embedded itself.
     */
    void show() noexcept;

    /**
     * No-op. Kept so the message loop can call it unconditionally; there are no
     * X11 events to pump on Haiku, and Win32 messages are handled by the
     * regular message loop as usual.
     */
    void handle_x11_events() noexcept;

    /**
     * Get the Win32 window handle so it can be passed to an `effEditOpen()`
     * call or a VST3 `attached()`.
     */
    HWND win32_handle() const noexcept;

   private:
    Logger& logger_;

    /**
     * The Win32 window the plugin draws into.
     */
    DeferredWin32Window win32_window_;

    /**
     * The function the idle timer runs. The window procedure reaches this
     * through the window's `GWLP_USERDATA`, so it has to outlive the timer --
     * hence its position between the window and the timer here, since members
     * are destroyed in reverse declaration order.
     */
    fu2::unique_function<void()> idle_timer_proc_;

    /**
     * The timer used to periodically call `idle_timer_proc_`, if a timer proc
     * was given. Killed before `idle_timer_proc_` is destroyed.
     */
    std::optional<Win32Timer> timer_;
};

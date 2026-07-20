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
#include "../common/editor-shm.h"
#include "../common/logging/common.h"
#include "editor-capture.h"
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
    Editor(
        MainContext& main_context,
        const Configuration& config,
        Logger& logger,
        const size_t parent_window_handle,
        std::optional<fu2::unique_function<void()>> timer_proc = std::nullopt);

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

    /**
     * Run the idle timer callback, if this editor was given one. Called from
     * the window procedure, which reaches this object through the window's
     * `GWLP_USERDATA`.
     */
    void run_idle_timer();

    /**
     * Grab a frame of the plugin's editor and publish it to the native side.
     * Called from the window procedure on the capture timer.
     */
    void run_capture();

    /**
     * Start carrying this editor's frames over a shared memory object with
     * this name, so the native plugin can paint them into the host's window.
     *
     * The object is created here when the plugin has already reported an
     * editor size, so `surface_config()` has something to hand back straight
     * away. If it has not, the object is created on the first captured frame
     * instead.
     *
     * @param width The editor's width as the plugin reported it, or 0 if it
     *   has not reported one yet.
     * @param height As `width`.
     */
    void enable_surface(const std::string& name,
                        uint16_t width,
                        uint16_t height);

    /**
     * The configuration the native side needs in order to map the same shared
     * memory object, or `std::nullopt` if no frame has been captured yet.
     */
    std::optional<EditorShmSurface::Config> surface_config() const noexcept;

   private:
    /**
     * Copy the frame `capture_` just took into the shared memory object,
     * creating or re-laying-out that object first if the editor's size has
     * changed. Only called when a surface has been asked for.
     */
    void publish_frame();

    Logger& logger_;

    /**
     * The Win32 window the plugin draws into.
     */
    DeferredWin32Window win32_window_;

    /**
     * The function the idle timer runs. The window procedure reaches this
     * through `run_idle_timer()`, so it has to outlive the timer -- hence its
     * position between the window and the timer here, since members are
     * destroyed in reverse declaration order.
     */
    fu2::unique_function<void()> idle_timer_proc_;

    /**
     * The timer used to periodically call `idle_timer_proc_`, if a timer proc
     * was given. Killed before `idle_timer_proc_` is destroyed.
     */
    std::optional<Win32Timer> timer_;

    /**
     * Reads the plugin's editor window back into a bitmap. Declared after the
     * window so it is destroyed before it, since it holds the window handle.
     */
    EditorCapture capture_;

    /**
     * The timer driving `capture_`, only set when capturing is enabled.
     */
    std::optional<Win32Timer> capture_timer_;

    /**
     * The shared memory object the captured frames are published to, and the
     * name it was asked to use. The object is created on the first captured
     * frame, once the editor's size is known, and re-laid-out whenever that
     * size changes.
     */
    std::optional<EditorShmSurface> surface_;
    std::string surface_name_;

    /**
     * The interval both timers run at, kept so the capture timer can be
     * started later than the constructor if a surface is asked for.
     */
    unsigned int capture_interval_ms_ = 0;

    /**
     * Set after a surface setup failure has been logged, so a permanently
     * failing surface cannot flood the log at the frame rate.
     */
    bool logged_surface_failure_ = false;

    /**
     * Where `VSTBRIDGE_CAPTURE_DUMP` asked for captured frames to be written,
     * if it was set. Frames are written as `<prefix>-<frame>.bmp`.
     */
    std::optional<std::string> capture_dump_prefix_;

    /**
     * How many times `run_capture()` has run, used to pick which frames get
     * dumped. Saturates rather than wrapping so a long lived editor cannot
     * start dumping again.
     */
    uint32_t captured_frames_ = 0;
};

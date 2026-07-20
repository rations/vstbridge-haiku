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

// The Haiku counterpart to `editor.cpp`. See `editor-haiku.h` for why there is
// no embedding here and what replaces it.

#include "editor-haiku.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>

using namespace std::literals::chrono_literals;

namespace {

/**
 * The IDs we'll use for our timers. These only have to be unique within the
 * window.
 */
constexpr size_t idle_timer_id = 1337;
constexpr size_t capture_timer_id = 1338;

/**
 * Which frames get written out when `VSTBRIDGE_CAPTURE_DUMP` is set. At the
 * default 60 fps these land roughly 0.1, 0.5 and 2 seconds after the editor
 * opens, which is enough to tell a plugin that never draws from one that just
 * had not drawn yet when the first frame was taken.
 */
constexpr uint32_t capture_dump_frames[] = {5, 30, 120};

/**
 * The window procedure for our editor windows.
 *
 * The X11 build needs an elaborate one to deal with reparenting, coordinate
 * fixups and focus grabbing. None of that applies to a plain top-level window,
 * so this only has to drive the idle timer that VST2 plugins rely on for
 * `effEditIdle`.
 */
LRESULT CALLBACK window_proc(HWND handle,
                             UINT message,
                             WPARAM wParam,
                             LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            // Stash whatever the `Editor` handed us at creation time so
            // `WM_TIMER` can reach it without a global
            const auto* create_params = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(
                handle, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create_params->lpCreateParams));
        } break;
        case WM_TIMER: {
            if (wParam != idle_timer_id && wParam != capture_timer_id) {
                break;
            }

            // `GWLP_USERDATA` only holds the `Editor` once its constructor has
            // gotten far enough to set it, and a timer cannot fire before it
            // has been created after that point, so this is null only during
            // window creation
            auto* editor = reinterpret_cast<Editor*>(
                GetWindowLongPtr(handle, GWLP_USERDATA));
            if (!editor) {
                break;
            }

            if (wParam == idle_timer_id) {
                editor->run_idle_timer();
            } else {
                editor->run_capture();
            }

            return 0;
        } break;
    }

    return DefWindowProc(handle, message, wParam, lParam);
}

/**
 * Lazily register the window class the plugin's editor window uses.
 */
ATOM get_window_class() noexcept {
    static ATOM window_class_handle = 0;
    if (!window_class_handle) {
        WNDCLASSEX window_class{};

        window_class.cbSize = sizeof(WNDCLASSEX);
        window_class.style = CS_DBLCLKS;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = GetModuleHandle(nullptr);
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = vstbridge_window_class_name;

        window_class_handle = RegisterClassEx(&window_class);
    }

    return window_class_handle;
}

}  // namespace

DeferredWin32Window::DeferredWin32Window(MainContext& main_context,
                                         HWND window) noexcept
    : handle_(window), main_context_(main_context) {}

DeferredWin32Window::~DeferredWin32Window() noexcept {
    // As in the X11 build, closing is deferred twice over: `WM_CLOSE` is posted
    // rather than calling `DestroyWindow()` directly, because that can hang for
    // a second or two, and the post itself is delayed so a message loop tick
    // landing between `IPlugView::removed()` and `IPlugView::~IPlugView` cannot
    // stall the host.
    try {
        std::shared_ptr<asio::steady_timer> destroy_timer =
            std::make_shared<asio::steady_timer>(main_context_.context_);
        destroy_timer->expires_after(1s);

        // The timer keeps itself alive by capturing a copy of its own shared
        // pointer, and cleans itself up once this lambda has run
        destroy_timer->async_wait(
            [destroy_timer, handle = handle_](const std::error_code& error) {
                if (error) {
                    return;
                }

                PostMessage(handle, WM_CLOSE, 0, 0);
            });
    } catch (const std::exception&) {
        // If the timer could not be set up then fall back to closing the window
        // immediately -- a delayed close is an optimization, leaking the window
        // is not acceptable
        PostMessage(handle_, WM_CLOSE, 0, 0);
    }
}

Editor::Editor(MainContext& main_context,
               const Configuration& config,
               Logger& logger,
               const size_t /*parent_window_handle*/,
               std::optional<fu2::unique_function<void()>> timer_proc)
    : logger_(logger),
      // Unlike the X11 build this is a normal decorated top-level window rather
      // than a `WS_EX_TOOLWINDOW`/`WS_POPUP` one sized to the whole screen.
      // Nothing is going to reparent or clip it, so it should look and behave
      // like any other window.
      //
      // NOTE: The window procedure reaches this `Editor` through the window's
      //       `GWLP_USERDATA` to service both timers, so everything a timer
      //       touches has to outlive the window. `idle_timer_proc_` and
      //       `capture_` are declared after `win32_window_` in the header for
      //       that reason -- destruction runs in reverse.
      win32_window_(main_context,
                    CreateWindowEx(0,
                                   reinterpret_cast<LPCSTR>(get_window_class()),
                                   "vstbridge plugin",
                                   WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT,
                                   CW_USEDEFAULT,
                                   256,
                                   256,
                                   nullptr,
                                   nullptr,
                                   GetModuleHandle(nullptr),
                                   nullptr)),
      // `win32_window_` is declared before this, so its handle is already
      // valid here. It may still be null if window creation failed, which
      // `EditorCapture` tolerates by never capturing anything.
      capture_(logger, win32_window_.handle_) {
    if (!win32_window_.handle_) {
        logger_.log("Could not create the plugin's editor window");
        return;
    }

    // Both timers reach this object through the window, so this has to be set
    // before either of them is created
    SetWindowLongPtr(win32_window_.handle_, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(this));

    capture_interval_ms_ = static_cast<unsigned int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            config.event_loop_interval())
            .count());

    if (timer_proc) {
        idle_timer_proc_ = std::move(*timer_proc);
        timer_.emplace(win32_window_.handle_, idle_timer_id,
                       capture_interval_ms_);
    }

    if (const char* dump_prefix = std::getenv("VSTBRIDGE_CAPTURE_DUMP")) {
        if (*dump_prefix) {
            capture_dump_prefix_ = dump_prefix;
            capture_timer_.emplace(win32_window_.handle_, capture_timer_id,
                                   capture_interval_ms_);

            logger_.log("Editor capture: enabled, frames will be written to '" +
                        *capture_dump_prefix_ + "-<frame>.bmp'");
        }
    }
}

void Editor::run_idle_timer() {
    if (idle_timer_proc_) {
        idle_timer_proc_();
    }
}

void Editor::publish_frame() {
    // The editor's size is only known once the plugin has drawn, so the object
    // is created here rather than in `enable_surface()`, and re-laid-out
    // whenever the plugin resizes itself
    EditorShmSurface::Config config;
    if (!EditorShmSurface::config_for(surface_name_, capture_.width(),
                                      capture_.height(), config)) {
        return;
    }

    try {
        if (!surface_) {
            surface_.emplace(config, true);
            logger_.log("Editor surface: publishing " +
                        std::to_string(config.width) + "x" +
                        std::to_string(config.height) + " frames through '" +
                        config.name + "'");
        } else if (surface_->width() != config.width ||
                   surface_->height() != config.height) {
            surface_->resize(config);
            logger_.log("Editor surface: resized to " +
                        std::to_string(config.width) + "x" +
                        std::to_string(config.height));
        }
    } catch (const std::exception& error) {
        // Losing the surface costs the editor its picture in the host, but it
        // must not take the plugin down with it
        if (!logged_surface_failure_) {
            logger_.log("Editor surface: could not be set up: " +
                        std::string(error.what()));
            logged_surface_failure_ = true;
        }

        surface_.reset();
        return;
    }

    logged_surface_failure_ = false;
    surface_->write_frame(capture_.pixels(), capture_.width(),
                          capture_.height());
}

void Editor::enable_surface(const std::string& name,
                            uint16_t width,
                            uint16_t height) {
    surface_name_ = name;

    // Create the object up front, since the plugin has already reported how
    // big its editor is. The native side asks for it once immediately after
    // attaching, and waiting for the first captured frame would mean having
    // nothing to hand it on that call.
    if (width > 0 && height > 0) {
        EditorShmSurface::Config config;
        if (EditorShmSurface::config_for(surface_name_, width, height,
                                         config)) {
            try {
                surface_.emplace(config, true);
                logger_.log("Editor surface: publishing " +
                            std::to_string(config.width) + "x" +
                            std::to_string(config.height) +
                            " frames through '" + config.name + "'");
            } catch (const std::exception& error) {
                // Not fatal: `publish_frame()` tries again on the first
                // captured frame
                logger_.log("Editor surface: could not be set up yet: " +
                            std::string(error.what()));
                surface_.reset();
            }
        }
    }

    // The capture timer may already be running for the frame dump
    if (!capture_timer_ && win32_window_.handle_) {
        capture_timer_.emplace(win32_window_.handle_, capture_timer_id,
                               capture_interval_ms_);
    }
}

std::optional<EditorShmSurface::Config> Editor::surface_config()
    const noexcept {
    if (!surface_) {
        return std::nullopt;
    }

    return surface_->config();
}

void Editor::run_capture() {
    if (!capture_.capture()) {
        return;
    }

    if (!surface_name_.empty()) {
        publish_frame();
    }

    // Saturate rather than wrap, so an editor left open for a very long time
    // cannot come back around and start dumping frames again
    if (captured_frames_ < UINT32_MAX) {
        captured_frames_++;
    }

    if (!capture_dump_prefix_) {
        return;
    }

    for (const uint32_t frame : capture_dump_frames) {
        if (captured_frames_ == frame) {
            capture_.dump_bmp(*capture_dump_prefix_ + "-" +
                              std::to_string(frame) + ".bmp");
            break;
        }
    }
}

void Editor::resize(uint16_t width, uint16_t height) {
    if (!win32_window_.handle_) {
        return;
    }

    // The plugin reports the size it wants for its *client* area, so the window
    // has to be grown to fit the decorations around it or the GUI ends up
    // cropped
    RECT window_rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&window_rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    SetWindowPos(win32_window_.handle_, nullptr, 0, 0,
                 window_rect.right - window_rect.left,
                 window_rect.bottom - window_rect.top,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
}

void Editor::show() noexcept {
    if (!win32_window_.handle_) {
        return;
    }

    ShowWindow(win32_window_.handle_, SW_SHOWNORMAL);
}

void Editor::handle_x11_events() noexcept {
    // Nothing to pump: there is no X11 connection here, and Win32 messages are
    // dispatched by the regular message loop
}

HWND Editor::win32_handle() const noexcept {
    return win32_window_.handle_;
}

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

using namespace std::literals::chrono_literals;

namespace {

/**
 * The ID we'll use for the idle timer. Only has to be unique within the window.
 */
constexpr size_t idle_timer_id = 1337;

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
            // Stash the callback the `Editor` handed us at creation time so
            // `WM_TIMER` can reach it without a global
            const auto* create_params =
                reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(
                handle, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create_params->lpCreateParams));
        } break;
        case WM_TIMER: {
            if (wParam != idle_timer_id) {
                break;
            }

            auto* timer_proc = reinterpret_cast<fu2::unique_function<void()>*>(
                GetWindowLongPtr(handle, GWLP_USERDATA));
            if (timer_proc) {
                (*timer_proc)();
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
      // NOTE: The idle timer callback is handed to `CreateWindowEx` so the
      //       window procedure can reach it, which means it has to outlive the
      //       window. `idle_timer_proc_` is declared after `win32_window_` in
      //       the header for that reason -- destruction runs in reverse.
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
                                   nullptr)) {
    if (!win32_window_.handle_) {
        logger_.log("Could not create the plugin's editor window");
        return;
    }

    if (timer_proc) {
        idle_timer_proc_ = std::move(*timer_proc);
        SetWindowLongPtr(win32_window_.handle_, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(&idle_timer_proc_));

        timer_.emplace(win32_window_.handle_, idle_timer_id,
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           config.event_loop_interval())
                           .count());
    }
}

void Editor::resize(uint16_t width, uint16_t height) {
    if (!win32_window_.handle_) {
        return;
    }

    // The plugin reports the size it wants for its *client* area, so the window
    // has to be grown to fit the decorations around it or the GUI ends up
    // cropped
    RECT window_rect{0, 0, static_cast<LONG>(width),
                     static_cast<LONG>(height)};
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

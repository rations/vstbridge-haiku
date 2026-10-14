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
#include <cstring>
#include <iostream>

// Wine's internal `ntuser.h` does not compile as C++, so the input injection
// lives behind a small C shim
#include "editor-input.h"

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
 * Warn when the prefix is not using Wine's null graphics driver.
 *
 * The editor is bridged by capturing the plugin window's backing store and
 * painting it in the host, so the plugin's own on-screen window is never wanted
 * -- it just floats over the host as a second copy of the same editor. The null
 * driver is what suppresses it: it declines to create a driver window surface,
 * which sends win32u down its offscreen surface path instead, so the window
 * still gets somewhere to draw and still receives `WM_PAINT` while never
 * reaching a display. Hiding the window would not do, since an invisible window
 * gets no surface at all and stops painting entirely.
 *
 * This cannot be set from here. The driver is chosen once per prefix, by
 * explorer when it creates the desktop, from
 * `HKCU\Software\Wine\Drivers\Graphics` -- so all this can do is say what is
 * wrong and how to fix it. Anything other than the null driver still works,
 * just with the plugin's own window visible alongside the host's copy.
 */
void warn_unless_null_graphics_driver(Logger& logger) noexcept {
    // Left empty when the key or the value is missing, which means Wine picks
    // its compiled-in default rather than the null driver
    char driver[32]{};

    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\Drivers", 0,
                      KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD type = 0;
        // One short of the buffer, so a value stored without its terminator
        // still leaves the zero initialization in place at the end
        DWORD size = sizeof(driver) - 1;
        if (RegQueryValueExA(key, "Graphics", nullptr, &type,
                             reinterpret_cast<BYTE*>(driver),
                             &size) != ERROR_SUCCESS ||
            type != REG_SZ) {
            driver[0] = '\0';
        }

        RegCloseKey(key);
    }

    if (std::strcmp(driver, "null") == 0) {
        return;
    }

    logger.log("");
    logger.log("WARNING: This Wine prefix uses the '" +
               std::string(driver[0] ? driver : "<default>") +
               "' graphics driver.");
    logger.log("         The plugin's own editor window will be visible on");
    logger.log("         screen on top of the copy drawn in the host. Set it");
    logger.log("         to 'null' to keep the editor to the host window:");
    logger.log("");
    logger.log("           wine reg add 'HKCU\\Software\\Wine\\Drivers' \\");
    logger.log("             /v Graphics /d null /f");
    logger.log("");
}

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

namespace {

bool popup_fix_tracing = false;
int popup_fix_users = 0;
HHOOK popup_fix_hook = nullptr;

LRESULT CALLBACK popup_fix_cbt_proc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_CREATEWND) {
        const HWND window = reinterpret_cast<HWND>(wParam);
        const LONG style = GetWindowLong(window, GWL_STYLE);
        const LONG ex_style = GetWindowLong(window, GWL_EXSTYLE);

        // Only captionless, non-resizable popup windows that are not supposed
        // to show up in the task bar. Regular plugin windows and dialogs are
        // left alone.
        if (!(style & WS_CHILD) && (style & WS_POPUP) &&
            (style & WS_CAPTION) != WS_CAPTION && !(style & WS_THICKFRAME) &&
            (style & WS_SYSMENU) && !(ex_style & WS_EX_APPWINDOW)) {
            SetWindowLong(window, GWL_STYLE, style & ~WS_SYSMENU);

            // This goes to STDERR instead of through a `Logger` since the hook
            // can outlive the editor that installed it. STDERR gets captured
            // and logged by the plugin side.
            if (popup_fix_tracing) {
                std::cerr << "DEBUG: Removed WS_SYSMENU from popup window "
                          << window << std::hex << " (style 0x" << style
                          << ", exstyle 0x" << ex_style << std::dec
                          << ") so Wine won't let a window manager manage it"
                          << std::endl;
            }
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace

PopupStyleFixer::PopupStyleFixer(bool enabled, Logger& logger)
    : enabled_(enabled) {
    if (!enabled_) {
        return;
    }

    // All editors live on the same GUI thread, so a single thread-local hook
    // shared between them is enough
    if (popup_fix_users++ == 0) {
        popup_fix_tracing = logger.editor_tracing_;
        popup_fix_hook = SetWindowsHookEx(WH_CBT, popup_fix_cbt_proc, nullptr,
                                          GetCurrentThreadId());
    }
}

PopupStyleFixer::~PopupStyleFixer() noexcept {
    if (!enabled_ || --popup_fix_users > 0) {
        return;
    }

    if (popup_fix_hook) {
        UnhookWindowsHookEx(popup_fix_hook);
        popup_fix_hook = nullptr;
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
      capture_(logger, win32_window_.handle_),
      popup_style_fixer_(config.editor_popup_fix, logger) {
    if (!win32_window_.handle_) {
        logger_.log("Could not create the plugin's editor window");
        return;
    }

    // The bridge depends on a prefix-wide setting it cannot make itself, so a
    // prefix that has not been set up says so in the log rather than silently
    // growing a second editor window
    warn_unless_null_graphics_driver(logger_);

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

namespace {

/**
 * Translate one of Haiku's raw character codes into a Win32 virtual key code.
 *
 * This mapping lives here rather than on the native side because this is the
 * only side where the `VK_*` constants exist as real symbols, so it is written
 * against the Windows headers instead of copying their values into a file that
 * cannot check them. The two are genuinely different: `B_RETURN` is 0x0a where
 * `VK_RETURN` is 0x0d, and Home, End and the page keys all disagree too.
 *
 * The Haiku constants are spelled numerically because this file is compiled
 * with winegcc and cannot include the Interface Kit's headers; the names from
 * `headers/os/interface/InterfaceDefs.h` are given alongside each one.
 *
 * Only the keys a plugin editor realistically needs are mapped. Anything else
 * falls through as its own code, which is correct for the letters and digits
 * that already share Win32's numbering.
 */
UINT virtual_key_from_haiku(uint32_t raw_char) noexcept {
    switch (raw_char) {
        case 0x01:  // B_HOME
            return VK_HOME;
        case 0x04:  // B_END
            return VK_END;
        case 0x08:  // B_BACKSPACE
            return VK_BACK;
        case 0x09:  // B_TAB
            return VK_TAB;
        case 0x0a:  // B_RETURN / B_ENTER
            return VK_RETURN;
        case 0x0b:  // B_PAGE_UP
            return VK_PRIOR;
        case 0x0c:  // B_PAGE_DOWN
            return VK_NEXT;
        case 0x1b:  // B_ESCAPE
            return VK_ESCAPE;
        case 0x1c:  // B_LEFT_ARROW
            return VK_LEFT;
        case 0x1d:  // B_RIGHT_ARROW
            return VK_RIGHT;
        case 0x1e:  // B_UP_ARROW
            return VK_UP;
        case 0x1f:  // B_DOWN_ARROW
            return VK_DOWN;
        case 0x7f:  // B_DELETE
            return VK_DELETE;
        default:
            break;
    }

    // Win32 uses the uppercase letter as the virtual key code, so lowercase
    // has to be folded up or the plugin sees the wrong key entirely
    if (raw_char >= 'a' && raw_char <= 'z') {
        return static_cast<UINT>(raw_char - 'a' + 'A');
    }

    return static_cast<UINT>(raw_char);
}

}  // namespace

void Editor::drain_input() {
    if (!surface_ || !win32_window_.handle_) {
        return;
    }

    // The window's position is needed to turn client coordinates back into the
    // screen coordinates the input system expects. Fetched once per drain
    // rather than per event, since it cannot change midway through.
    RECT window_rect{};
    POINT client_origin{0, 0};
    const bool have_position =
        GetWindowRect(win32_window_.handle_, &window_rect) &&
        ClientToScreen(win32_window_.handle_, &client_origin);

    EditorInputEvent event{};
    while (surface_->pop_input(event)) {
        // Everything in the mapping is untrusted, so an unrecognised event is
        // dropped rather than being cast to an enumerator it does not name
        switch (static_cast<EditorInputType>(event.type)) {
            case EditorInputType::mouse_move:
            case EditorInputType::mouse_down:
            case EditorInputType::mouse_up:
            case EditorInputType::mouse_wheel:
                if (have_position) {
                    inject_mouse(event, client_origin);
                }
                break;
            case EditorInputType::key_down:
            case EditorInputType::key_up:
                inject_key(event);
                break;
            default:
                break;
        }
    }
}

void Editor::inject_mouse(const EditorInputEvent& event,
                          const POINT& client_origin) noexcept {
    // The injection path takes raw screen pixels, unlike the public
    // `SendInput()`, which scales absolute coordinates into a 0..65535 range.
    // Only `NtUserSendInput()` runs that conversion, so passing normalized
    // coordinates here would put the pointer in the wrong place entirely.
    DWORD flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    DWORD mouse_data = 0;

    switch (static_cast<EditorInputType>(event.type)) {
        case EditorInputType::mouse_down:
            switch (static_cast<EditorMouseButton>(event.button)) {
                case EditorMouseButton::left:
                    flags |= MOUSEEVENTF_LEFTDOWN;
                    break;
                case EditorMouseButton::right:
                    flags |= MOUSEEVENTF_RIGHTDOWN;
                    break;
                case EditorMouseButton::middle:
                    flags |= MOUSEEVENTF_MIDDLEDOWN;
                    break;
                default:
                    break;
            }
            break;
        case EditorInputType::mouse_up:
            switch (static_cast<EditorMouseButton>(event.button)) {
                case EditorMouseButton::left:
                    flags |= MOUSEEVENTF_LEFTUP;
                    break;
                case EditorMouseButton::right:
                    flags |= MOUSEEVENTF_RIGHTUP;
                    break;
                case EditorMouseButton::middle:
                    flags |= MOUSEEVENTF_MIDDLEUP;
                    break;
                default:
                    break;
            }
            break;
        case EditorInputType::mouse_wheel:
            flags |= MOUSEEVENTF_WHEEL;
            mouse_data = static_cast<DWORD>(event.wheel * WHEEL_DELTA);
            break;
        default:
            break;
    }

    // No target window is named, so the server routes the event by position the
    // way a real mouse driver's would. Naming the editor window instead would
    // pin every event to it: the server only hit-tests when the named window is
    // invisible, so a dropdown the plugin opened -- a separate top-level window
    // sitting above the editor -- would never see a click.
    vstbridge_send_mouse_input(nullptr, client_origin.x + event.x,
                               client_origin.y + event.y, flags, mouse_data);
}

void Editor::inject_key(const EditorInputEvent& event) noexcept {
    const DWORD flags =
        static_cast<EditorInputType>(event.type) == EditorInputType::key_up
            ? KEYEVENTF_KEYUP
            : 0;

    vstbridge_send_key_input(
        win32_window_.handle_,
        static_cast<WORD>(virtual_key_from_haiku(event.button)), flags);
}

void Editor::run_capture() {
    // Input is drained on the same timer as the capture, so it is handled on
    // the thread that owns the editor window rather than on whichever thread
    // the native side happened to write it from
    drain_input();

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

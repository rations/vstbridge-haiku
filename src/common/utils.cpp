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

#include "utils.h"

#include <stdlib.h>

#include <sched.h>
#include <xmmintrin.h>

#ifdef __HAIKU__
#include <OS.h>
#include <pthread.h>
#endif

namespace fs = ghc::filesystem;

using namespace std::literals::string_view_literals;

/**
 * If this environment variable is set to `1`, then we won't enable the watchdog
 * timer. This is only necessary when running the Wine process under a different
 * namespace than the host.
 */
constexpr char disable_watchdog_timer_env_var[] = "VSTBRIDGE_NO_WATCHDOG";

/**
 * If this environment variable is set, vstbridge will store its sockets and
 * other temporary files here instead of in `$XDG_RUNTIME_DIR` or `/tmp`. This
 * is only relevant when using some namespacing setup for sandboxing.
 */
constexpr char temp_dir_override_env_var[] = "VSTBRIDGE_TEMP_DIR";

fs::path get_temporary_directory() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const auto directory = getenv(temp_dir_override_env_var)) {
        return fs::path(directory);
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
    } else if (const auto directory = getenv("XDG_RUNTIME_DIR")) {
        return fs::path(directory);
    } else {
        return fs::temp_directory_path();
    }
}

#ifdef __HAIKU__
// Haiku declares `sched_getparam()` and `sched_setscheduler()` nowhere -- its
// <sched.h> only offers `sched_yield()` and the priority range queries. Thread
// scheduling goes through `pthread_setschedparam()`, which libroot implements
// on top of `_kern_set_thread_priority()`. Two constraints follow from that
// implementation and shape everything below:
//
//   * SCHED_FIFO is rejected outright with ENOTSUP. Only SCHED_RR and
//     SCHED_OTHER are accepted, so realtime here means round robin.
//   * A SCHED_RR priority must be >= B_FIRST_REAL_TIME_PRIORITY (100) and a
//     SCHED_OTHER priority must be below it, or the call fails with EINVAL.
//
// The jack2 Haiku port reaches the same conclusion from the other direction:
// its `jack_rt_policy()` probes `sched_get_priority_min(SCHED_FIFO)`, gets -1,
// and falls back to SCHED_RR. Priorities are clamped with the same runtime
// range queries it uses rather than against hardcoded constants.

std::optional<int> get_realtime_priority() noexcept {
    // Unlike Linux, *every* Haiku thread has a positive priority
    // (B_NORMAL_PRIORITY is 10), so a nonzero value does not imply realtime
    // scheduling and the policy has to be checked instead. libroot reports
    // SCHED_RR exactly when the priority is >= B_FIRST_REAL_TIME_PRIORITY.
    int policy = 0;
    sched_param current_params{};
    if (pthread_getschedparam(pthread_self(), &policy, &current_params) == 0 &&
        policy == SCHED_RR) {
        return current_params.sched_priority;
    } else {
        return std::nullopt;
    }
}

bool set_realtime_priority(bool sched_fifo, int priority) noexcept {
    // The `sched_fifo` parameter keeps its upstream meaning -- "realtime or
    // not" -- but selects SCHED_RR, since Haiku has no usable SCHED_FIFO.
    const int policy = sched_fifo ? SCHED_RR : SCHED_OTHER;

    // Callers pass Linux-shaped priorities (the default is 5, and values
    // propagated from the host come from `get_realtime_priority()`), which do
    // not line up with Haiku's ranges at all. Clamping into the policy's real
    // range is what makes both meaningful: a Linux-ish 5 becomes
    // B_FIRST_REAL_TIME_PRIORITY, the lowest realtime priority, which keeps us
    // below jackDAW's own audio threads rather than competing with them.
    const int min_priority = sched_get_priority_min(policy);
    const int max_priority = sched_get_priority_max(policy);
    if (min_priority == -1 || max_priority == -1) {
        return false;
    }

    // Dropping out of realtime should land at the normal priority rather than
    // at the bottom of the SCHED_OTHER range, so returning threads behave like
    // ordinary ones instead of being starved.
    int target = sched_fifo ? priority : B_NORMAL_PRIORITY;
    if (target < min_priority) {
        target = min_priority;
    } else if (target > max_priority) {
        target = max_priority;
    }

    sched_param params{.sched_priority = target};
    return pthread_setschedparam(pthread_self(), policy, &params) == 0;
}

std::optional<rlim_t> get_memlock_limit() noexcept {
    // No RLIMIT_MEMLOCK on Haiku, and nothing to report: the platform has no
    // `MAP_LOCKED`, so the audio buffers are never locked in the first place
    // (see `audio-shm.cpp`). Callers use this only to print a diagnostic.
    return std::nullopt;
}

std::optional<rlim_t> get_rttime_limit() noexcept {
    // No RLIMIT_RTTIME on Haiku. Nothing caps the runtime of a realtime thread
    // the way it does on Linux, so there is no limit to report.
    return std::nullopt;
}
#else
std::optional<int> get_realtime_priority() noexcept {
    sched_param current_params{};
    if (sched_getparam(0, &current_params) == 0 &&
        current_params.sched_priority > 0) {
        return current_params.sched_priority;
    } else {
        return std::nullopt;
    }
}

bool set_realtime_priority(bool sched_fifo, int priority) noexcept {
    sched_param params{.sched_priority = (sched_fifo ? priority : 0)};
    return sched_setscheduler(0, sched_fifo ? SCHED_FIFO : SCHED_OTHER,
                              &params) == 0;
}

std::optional<rlim_t> get_memlock_limit() noexcept {
    rlimit limits{};
    if (getrlimit(RLIMIT_MEMLOCK, &limits) == 0) {
        return limits.rlim_cur;
    } else {
        return std::nullopt;
    }
}

std::optional<rlim_t> get_rttime_limit() noexcept {
    rlimit limits{};
    if (getrlimit(RLIMIT_RTTIME, &limits) == 0) {
        return limits.rlim_cur;
    } else {
        return std::nullopt;
    }
}
#endif

bool is_watchdog_timer_disabled() {
    // This is safe because we're not storing the pointer anywhere and the
    // environment doesn't get modified anywhere
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* disable_watchdog_env = getenv(disable_watchdog_timer_env_var);

    return disable_watchdog_env && disable_watchdog_env == "1"sv;
}

size_t strlcpy_buffer(char* dst, const std::string& src, size_t size) {
    if (size == 0) {
        return src.size();
    }

    // Make sure there's always room for a null terminator
    const size_t copy_len = std::min(size - 1, src.size());
    std::copy(src.c_str(), src.c_str() + copy_len, dst);
    dst[copy_len] = 0;

    return src.size();
}

std::string xml_escape(std::string string) {
    // Implementation idea stolen from https://stackoverflow.com/a/5665377
    std::string escaped;
    escaped.reserve(
        static_cast<size_t>(static_cast<double>(string.size()) * 1.1));
    for (const char& character : string) {
        switch (character) {
            case '&':
                escaped.append("&amp;");
                break;
            case '\"':
                escaped.append("&quot;");
                break;
            case '\'':
                escaped.append("&apos;");
                break;
            case '<':
                escaped.append("&lt;");
                break;
            case '>':
                escaped.append("&gt;");
                break;
            default:
                escaped.push_back(character);
                break;
        }
    }

    return escaped;
}

std::string url_encode_path(std::string path) {
    // We only need to escape a couple of special characters here. This is used
    // in the notifications as well as in the XDND proxy. We encode the reserved
    // characters mentioned here, with the exception of the forward slash:
    // https://en.wikipedia.org/wiki/Percent-encoding#Reserved_characters
    std::string escaped;
    escaped.reserve(
        static_cast<size_t>(static_cast<double>(path.size()) * 1.1));
    for (const char& character : path) {
        switch (character) {
            // Spaces are somehow in the above list, but Bitwig Studio requires
            // spaces to be escaped in the `text/uri-list` format
            case ' ':
                escaped.append("%20");
                break;
            case '!':
                escaped.append("%21");
                break;
            case '#':
                escaped.append("%23");
                break;
            case '$':
                escaped.append("%24");
                break;
            case '%':
                escaped.append("%25");
                break;
            case '&':
                escaped.append("%26");
                break;
            case '\'':
                escaped.append("%27");
                break;
            case '(':
                escaped.append("%28");
                break;
            case ')':
                escaped.append("%29");
                break;
            case '*':
                escaped.append("%2A");
                break;
            case '+':
                escaped.append("%2B");
                break;
            case ',':
                escaped.append("%2C");
                break;
            case ':':
                escaped.append("%3A");
                break;
            case ';':
                escaped.append("%3B");
                break;
            case '=':
                escaped.append("%3D");
                break;
            case '?':
                escaped.append("%3F");
                break;
            case '@':
                escaped.append("%40");
                break;
            case '[':
                escaped.append("%5B");
                break;
            case ']':
                escaped.append("%5D");
                break;
            default:
                escaped.push_back(character);
                break;
        }
    }

    return escaped;
}

ScopedFlushToZero::ScopedFlushToZero() noexcept {
    old_ftz_mode_ = _MM_GET_FLUSH_ZERO_MODE();
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
}

ScopedFlushToZero::~ScopedFlushToZero() noexcept {
    if (old_ftz_mode_) {
        _MM_SET_FLUSH_ZERO_MODE(*old_ftz_mode_);
    }
}

ScopedFlushToZero::ScopedFlushToZero(ScopedFlushToZero&& o) noexcept
    : old_ftz_mode_(std::move(o.old_ftz_mode_)) {
    o.old_ftz_mode_.reset();
}

ScopedFlushToZero& ScopedFlushToZero::operator=(
    ScopedFlushToZero&& o) noexcept {
    old_ftz_mode_ = std::move(o.old_ftz_mode_);
    o.old_ftz_mode_.reset();

    return *this;
}

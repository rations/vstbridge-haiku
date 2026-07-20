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

// Haiku-only. Carries plugin editor frames from the Wine host process to the
// native plugin, which paints them into a `BView` inside the host's editor
// window.
//
// This exists because a plugin editor cannot be embedded into the host's window
// on Haiku: each application has its own view token space and app_server
// rejects a parent view token owned by another team, so there is no equivalent
// of X11 reparenting. Pixels are copied across the process boundary instead.
//
// The layout is a small header, then a ring of input events, then two equally
// sized frame buffers. Frames travel from Wine to the native side; input events
// travel the other way. The frame writer fills whichever buffer is not
// currently published and then publishes it, so a reader is never looking at a
// buffer being written. There are no locks: a `BWindow` looper must never block
// waiting on the Wine process, which may be slow, stopped in a debugger or
// dead.
//
// Input deliberately does not go over the control socket. That socket is
// request/response, so every event would block the host's looper on a round
// trip to the Wine process -- a mouse drag produces these faster than a busy
// plugin could answer them, and the DAW's UI would stutter. The ring is
// wait-free on the producing side and is simply drained on the Wine side's
// existing timer.
//
// Nothing here is reachable from the audio thread. It is a GUI path throughout.

#include <atomic>
#include <cstdint>
#include <string>

#include <sys/mman.h>

/**
 * What an `EditorInputEvent` describes. Kept as an explicit `uint32_t` rather
 * than a plain enum because these values live in shared memory and are read
 * back by the other process.
 */
enum class EditorInputType : uint32_t {
    mouse_move = 1,
    mouse_down = 2,
    mouse_up = 3,
    mouse_wheel = 4,
    key_down = 5,
    key_up = 6,
};

/**
 * Which mouse button an event refers to. These are our own values, translated
 * to BeAPI constants on one side and Win32 ones on the other, so neither
 * platform's numbering leaks into the shared layout.
 */
enum class EditorMouseButton : uint32_t {
    none = 0,
    left = 1,
    right = 2,
    middle = 3,
};

/**
 * Modifier keys, as a bit mask. Plugins use these for fine-grained knob
 * dragging and for reset-to-default clicks, so they have to survive the trip.
 */
enum class EditorModifier : uint32_t {
    shift = 1u << 0,
    control = 1u << 1,
    alt = 1u << 2,
};

/**
 * A single input event on its way from the host's editor view to the plugin.
 *
 * This is a fixed-size POD living in shared memory, so it must stay trivially
 * copyable and must not grow a pointer or a `std::string`. Both sides are built
 * by the same compiler for the same architecture, which is what makes sharing
 * the layout safe.
 */
struct EditorInputEvent {
    /**
     * An `EditorInputType`, stored as its underlying type. Read back from
     * shared memory, so it is validated rather than cast blindly.
     */
    uint32_t type;

    /**
     * Where the event happened, relative to the top left of the editor's
     * client area -- which is exactly the view the frames are painted into, so
     * no coordinate system conversion is needed beyond offsetting to the Wine
     * window's position.
     */
    int32_t x;
    int32_t y;

    /**
     * An `EditorMouseButton` for button events, otherwise `none`.
     *
     * For key events this carries Haiku's raw character code instead, and the
     * Wine side translates it to a Win32 virtual key code. The translation
     * deliberately lives there rather than here: that is the only side where
     * the `VK_*` constants exist as real symbols, so the mapping can be
     * written against the Windows headers instead of copying their numeric
     * values into a file that has no way to check them. They are not
     * interchangeable -- `B_RETURN` is 0x0a where `VK_RETURN` is 0x0d.
     */
    uint32_t button;

    /**
     * Which modifier keys were held, as a mask of `EditorModifier` values.
     */
    uint32_t modifiers;

    /**
     * Wheel movement in notches, positive being away from the user. Only
     * meaningful for `mouse_wheel`.
     */
    int32_t wheel;
};

/**
 * A double-buffered editor frame in shared memory.
 *
 * The Wine host creates the object and sends the resulting `Config` to the
 * native plugin, which maps the same region. Either side destroying the object
 * unlinks it, matching how `AudioShmBuffer` avoids leaking a buffer when a
 * process dies.
 */
class EditorShmSurface {
   public:
    /**
     * Everything the native side needs in order to map the same region. Created
     * on the Wine side and sent over the socket.
     */
    struct Config {
        /**
         * The unique identifier for this shared memory object.
         */
        std::string name;

        /**
         * The size of the shared memory object **in bytes**, covering the
         * header and both frame buffers.
         */
        uint32_t size = 0;

        /**
         * The size of the frames this object is currently laid out for. The
         * frames themselves also carry their dimensions in the header, but the
         * mapping has to be sized before anything can be read out of it.
         */
        uint32_t width = 0;
        uint32_t height = 0;

        template <typename S>
        void serialize(S& s) {
            s.text1b(name, 1024);
            s.value4b(size);
            s.value4b(width);
            s.value4b(height);
        }
    };

    /**
     * The largest frame we will map, per dimension. A plugin editor is never
     * anywhere near this, so anything larger means the dimensions came from a
     * window in a bad state or from a corrupted message, and acting on it would
     * mean a huge allocation driven by untrusted input.
     */
    static constexpr uint32_t max_dimension = 8192;

    /**
     * Build the configuration for a frame size, without creating anything. Used
     * on the Wine side to work out how large the object has to be.
     *
     * @return Whether the dimensions were in range. The config is left
     *   untouched if they were not.
     */
    static bool config_for(const std::string& name,
                           uint32_t width,
                           uint32_t height,
                           Config& config) noexcept;

    /**
     * Create or connect to the shared memory object described by `config` and
     * map it.
     *
     * @throw std::invalid_argument If the config is not self-consistent, which
     *   for the reading side means the sending side cannot be trusted.
     * @throw std::system_error If the object could not be created or mapped.
     */
    EditorShmSurface(const Config& config, bool create);

    ~EditorShmSurface() noexcept;

    EditorShmSurface(const EditorShmSurface&) = delete;
    EditorShmSurface& operator=(const EditorShmSurface&) = delete;
    EditorShmSurface(EditorShmSurface&&) noexcept;
    EditorShmSurface& operator=(EditorShmSurface&&) noexcept;

    /**
     * Lay the object out for a new frame size, growing the mapping if needed.
     * The name has to stay the same. Only called on the writing side; the
     * reading side is given a fresh `Config` over the socket instead.
     *
     * @throw std::invalid_argument If the config is for a different object or
     *   its dimensions are out of range.
     * @throw std::system_error If the object could not be mapped.
     */
    void resize(const Config& new_config);

    /**
     * Copy a frame in and publish it, so the next `read_frame()` sees it.
     *
     * The frame is expected to be tightly packed 32 bits per pixel, top-down,
     * which is what `EditorCapture` produces.
     *
     * @return Whether the frame was published. A frame whose size does not
     *   match what this object is laid out for is refused rather than
     *   truncated; the caller is expected to `resize()` first.
     */
    bool write_frame(const uint8_t* pixels,
                     uint32_t width,
                     uint32_t height) noexcept;

    /**
     * Copy the most recently published frame out, if it is newer than the one
     * the caller last saw.
     *
     * @param destination Where to copy the frame to. Must have room for
     *   `frame_size_bytes()` bytes.
     * @param destination_size The capacity of `destination`, checked before
     *   anything is written.
     * @param last_sequence In/out. Pass the value from the previous call, or
     *   zero initially. It is updated to the sequence number of the frame that
     *   was read.
     *
     * @return Whether a new frame was copied out. `false` means nothing has
     *   changed since `last_sequence`, which is the common case at idle.
     */
    bool read_frame(uint8_t* destination,
                    size_t destination_size,
                    uint64_t& last_sequence) const noexcept;

    /**
     * Push an input event towards the plugin. Called on the native side, from
     * the host window's looper thread.
     *
     * Wait-free: if the ring is full the event is dropped rather than waiting
     * for the Wine side to catch up, because the alternative is stalling the
     * DAW's UI on a process that may be wedged or dead.
     *
     * @return Whether the event was queued. `false` means the ring was full,
     *   which in practice means the Wine host has stopped draining it.
     */
    bool push_input(const EditorInputEvent& event) noexcept;

    /**
     * Take the oldest queued input event. Called on the Wine side, from the
     * thread that owns the editor window.
     *
     * @return Whether an event was returned. `false` means the ring is empty.
     */
    bool pop_input(EditorInputEvent& event) noexcept;

    /**
     * The dimensions this object is currently laid out for.
     */
    uint32_t width() const noexcept { return config_.width; }
    uint32_t height() const noexcept { return config_.height; }

    /**
     * The size of a single frame in bytes.
     */
    uint32_t frame_size_bytes() const noexcept {
        return config_.width * config_.height * 4;
    }

    const Config& config() const noexcept { return config_; }

   private:
    /**
     * The bookkeeping at the start of the mapping. Both processes write to
     * this, so every field a reader depends on is atomic.
     *
     * This is only ever accessed through a pointer into the mapping, never
     * copied, so it must stay trivially layout-compatible across both sides of
     * the bridge. Both are built by the same compiler for the same
     * architecture, which is what makes that safe.
     */
    struct Header {
        /**
         * Identifies the mapping as ours and versions the layout. A reader that
         * finds anything else refuses to read rather than interpreting
         * arbitrary bytes as pixels.
         */
        uint32_t magic;
        uint32_t version;

        /**
         * The dimensions of the published frame. Written before the sequence
         * number is bumped, so a reader that has seen a sequence number can
         * trust these.
         */
        std::atomic<uint32_t> width;
        std::atomic<uint32_t> height;

        /**
         * Which of the two buffers holds the most recently published frame.
         */
        std::atomic<uint32_t> front;

        /**
         * Bumped every time a frame is published. A reader compares this
         * against what it last saw to avoid repainting an unchanged editor.
         * Starts at zero, which no published frame ever uses.
         */
        std::atomic<uint64_t> sequence;

        /**
         * The input ring's indices, in units of events rather than bytes. Only
         * the native side advances `input_write` and only the Wine side
         * advances `input_read`, which is what makes the ring safe without a
         * lock. Both grow without bound and are taken modulo the capacity, so
         * a full ring is distinguishable from an empty one.
         */
        std::atomic<uint32_t> input_write;
        std::atomic<uint32_t> input_read;
    };

    static constexpr uint32_t header_magic = 0x56534248;  // "VSBH"

    /**
     * Bumped when the layout changes. Version 2 added the input ring, which
     * moved the frame buffers.
     */
    static constexpr uint32_t header_version = 2;

    /**
     * How many input events the ring holds. A mouse drag produces events at
     * roughly the display's refresh rate and the Wine side drains the ring on
     * its own timer at a comparable rate, so this is around four seconds of
     * headroom -- far more than a momentary hiccup needs, and still tiny next
     * to a single frame.
     */
    static constexpr uint32_t input_capacity = 256;

    /**
     * The input ring starts here. Kept well clear of the header so the events
     * are never in the same cache line as the atomics above.
     */
    static constexpr uint32_t input_offset = 128;

    static_assert(sizeof(Header) <= input_offset,
                  "the header has outgrown the space reserved before the "
                  "input ring");

    /**
     * The frame buffers start after the input ring.
     */
    static constexpr uint32_t buffers_offset =
        input_offset + (input_capacity * sizeof(EditorInputEvent));

    /**
     * Map the object at its configured size, replacing any existing mapping.
     * Haiku has no `mremap()`, so this unmaps before mapping again, exactly as
     * `AudioShmBuffer` does.
     *
     * @throw std::system_error If the object could not be mapped.
     */
    void setup_mapping();

    /**
     * Check that a config is self-consistent, i.e. that its size actually
     * covers a header and two frames of its stated dimensions. Everything
     * arriving over the socket is untrusted even though both ends are ours.
     */
    static bool config_is_sane(const Config& config) noexcept;

    Header* header() const noexcept {
        return reinterpret_cast<Header*>(shm_bytes_);
    }

    uint8_t* buffer(uint32_t index) const noexcept {
        return shm_bytes_ + buffers_offset + (index * frame_size_bytes());
    }

    EditorInputEvent* input_slot(uint32_t index) const noexcept {
        return reinterpret_cast<EditorInputEvent*>(shm_bytes_ + input_offset) +
               (index % input_capacity);
    }

    Config config_;

    int shm_fd_ = -1;
    uint8_t* shm_bytes_ = nullptr;
    size_t shm_size_ = 0;

    /**
     * Whether this side created the object, and so should initialize the
     * header.
     */
    bool created_ = false;

    bool is_moved_ = false;
};

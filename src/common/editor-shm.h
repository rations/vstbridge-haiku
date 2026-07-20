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
// The layout is a small header followed by two equally sized frame buffers. The
// writer fills whichever buffer is not currently published and then publishes
// it, so a reader is never looking at a buffer being written. There are no
// locks: a `BWindow` looper must never block waiting on the Wine process, which
// may be slow, stopped in a debugger or dead.
//
// Nothing here is reachable from the audio thread. It is a GUI path throughout.

#include <atomic>
#include <cstdint>
#include <string>

#include <sys/mman.h>

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
    };

    static constexpr uint32_t header_magic = 0x56534248;  // "VSBH"
    static constexpr uint32_t header_version = 1;

    /**
     * The frame buffers start here. Kept well clear of the header so the two
     * are never in the same cache line as the atomics above.
     */
    static constexpr uint32_t buffers_offset = 128;

    static_assert(sizeof(Header) <= buffers_offset,
                  "the header has outgrown the space reserved before the "
                  "frame buffers");

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

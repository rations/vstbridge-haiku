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

#include "editor-shm.h"

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

bool EditorShmSurface::config_for(const std::string& name,
                                  uint32_t width,
                                  uint32_t height,
                                  Config& config) noexcept {
    if (width == 0 || height == 0 || width > max_dimension ||
        height > max_dimension) {
        return false;
    }

    // Both dimensions are bounded above, so this cannot overflow, but the
    // assertion documents the reason the multiplication below is safe
    static_assert(
        uint64_t{max_dimension} * max_dimension * 4 * 2 + buffers_offset <
            UINT32_MAX,
        "two frames at the maximum size no longer fit in the 32-bit "
        "size this config is serialized with");

    config.name = name;
    config.width = width;
    config.height = height;
    config.size = buffers_offset + (width * height * 4 * 2);

    return true;
}

bool EditorShmSurface::config_is_sane(const Config& config) noexcept {
    if (config.name.empty() || config.width == 0 || config.height == 0 ||
        config.width > max_dimension || config.height > max_dimension) {
        return false;
    }

    // Recomputing the size rather than trusting it is the point: a size that
    // does not match the dimensions would otherwise let a malformed message
    // produce reads past the end of the mapping
    const uint64_t expected = uint64_t{buffers_offset} +
                              (uint64_t{config.width} * config.height * 4 * 2);

    return config.size == expected;
}

EditorShmSurface::EditorShmSurface(const Config& config, bool create)
    : config_(config), created_(create) {
    if (!config_is_sane(config_)) {
        throw std::invalid_argument(
            "Invalid editor surface configuration for \"" + config_.name +
            "\"");
    }

    shm_fd_ = shm_open(config_.name.c_str(),
                       create ? (O_RDWR | O_CREAT) : O_RDWR, 0600);
    if (shm_fd_ == -1) {
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "Could not open shared memory object " + config_.name);
    }

    setup_mapping();

    if (created_) {
        Header* const h = header();

        h->magic = header_magic;
        h->version = header_version;
        h->width.store(0, std::memory_order_relaxed);
        h->height.store(0, std::memory_order_relaxed);
        h->front.store(0, std::memory_order_relaxed);
        h->input_write.store(0, std::memory_order_relaxed);
        h->input_read.store(0, std::memory_order_relaxed);
        // Published frames start at 1, so a reader that has seen nothing can
        // start from 0 and still be told about the very first frame
        h->sequence.store(0, std::memory_order_release);
    }
}

EditorShmSurface::~EditorShmSurface() noexcept {
    if (is_moved_) {
        return;
    }

    if (shm_bytes_) {
        munmap(shm_bytes_, shm_size_);
    }
    if (shm_fd_ != -1) {
        close(shm_fd_);
    }

    // As with the audio buffers, either side dropping the object unlinks it, so
    // a process dying does not leave the name behind
    shm_unlink(config_.name.c_str());
}

EditorShmSurface::EditorShmSurface(EditorShmSurface&& o) noexcept
    : config_(std::move(o.config_)),
      shm_fd_(o.shm_fd_),
      shm_bytes_(o.shm_bytes_),
      shm_size_(o.shm_size_),
      created_(o.created_) {
    o.is_moved_ = true;
}

EditorShmSurface& EditorShmSurface::operator=(EditorShmSurface&& o) noexcept {
    config_ = std::move(o.config_);
    shm_fd_ = o.shm_fd_;
    shm_bytes_ = o.shm_bytes_;
    shm_size_ = o.shm_size_;
    created_ = o.created_;
    o.is_moved_ = true;

    return *this;
}

void EditorShmSurface::setup_mapping() {
    if (ftruncate(shm_fd_, config_.size) != 0) {
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "Could not size shared memory object " + config_.name);
    }

    // Haiku has neither `mremap()` nor `MAP_LOCKED`, so the old mapping is
    // dropped by hand before a new one is made. This only runs when the editor
    // is created or resized, never per frame.
    if (shm_bytes_) {
        munmap(shm_bytes_, shm_size_);
        shm_bytes_ = nullptr;
    }

    shm_bytes_ = static_cast<uint8_t*>(mmap(
        nullptr, config_.size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0));
    if (shm_bytes_ == MAP_FAILED) {
        shm_bytes_ = nullptr;
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "Could not map shared memory for " + config_.name);
    }

    shm_size_ = config_.size;
}

void EditorShmSurface::resize(const Config& new_config) {
    if (new_config.name != config_.name) {
        throw std::invalid_argument("Expected editor surface \"" +
                                    config_.name + "\", got \"" +
                                    new_config.name + "\"");
    }
    if (!config_is_sane(new_config)) {
        throw std::invalid_argument(
            "Invalid editor surface configuration for "
            "\"" +
            new_config.name + "\"");
    }

    config_ = new_config;
    setup_mapping();
}

bool EditorShmSurface::write_frame(const uint8_t* pixels,
                                   uint32_t width,
                                   uint32_t height) noexcept {
    if (!shm_bytes_ || !pixels || width != config_.width ||
        height != config_.height) {
        return false;
    }

    Header* const h = header();

    // The buffer that is not published is the one nobody can be reading, so it
    // is safe to write to without any locking
    const uint32_t back = 1 - h->front.load(std::memory_order_relaxed);
    std::memcpy(buffer(back), pixels, frame_size_bytes());

    h->width.store(width, std::memory_order_relaxed);
    h->height.store(height, std::memory_order_relaxed);

    // Release ordering on both stores so that a reader which observes the new
    // sequence number is guaranteed to also see the pixels and the front index
    h->front.store(back, std::memory_order_release);
    h->sequence.fetch_add(1, std::memory_order_release);

    return true;
}

bool EditorShmSurface::read_frame(uint8_t* destination,
                                  size_t destination_size,
                                  uint64_t& last_sequence) const noexcept {
    if (!shm_bytes_ || !destination || destination_size < frame_size_bytes()) {
        return false;
    }

    const Header* const h = header();

    // Everything in the mapping is untrusted, even though the writer is also
    // ours. A header that is not ours means the object was never initialized or
    // belongs to something else, and its contents must not be treated as pixels
    if (h->magic != header_magic || h->version != header_version) {
        return false;
    }

    const uint64_t sequence = h->sequence.load(std::memory_order_acquire);
    if (sequence == 0 || sequence == last_sequence) {
        return false;
    }

    // The frame is only readable at the size this side is mapped for. A
    // mismatch means the writer has resized and the new configuration has not
    // arrived over the socket yet, so this frame is skipped rather than being
    // read at the wrong size.
    if (h->width.load(std::memory_order_relaxed) != config_.width ||
        h->height.load(std::memory_order_relaxed) != config_.height) {
        return false;
    }

    const uint32_t front = h->front.load(std::memory_order_acquire);
    if (front > 1) {
        return false;
    }

    std::memcpy(destination, buffer(front), frame_size_bytes());

    // With two buffers the writer only comes back around to the buffer just
    // read after publishing one more frame. If it published two or more while
    // the copy was in progress then it may have started overwriting this
    // buffer, so the copy is discarded rather than shown torn. At a 60 Hz
    // editor this needs the copy to take longer than two frames, so it should
    // essentially never happen.
    const uint64_t sequence_after = h->sequence.load(std::memory_order_acquire);
    if (sequence_after - sequence >= 2) {
        return false;
    }

    last_sequence = sequence;

    return true;
}

bool EditorShmSurface::push_input(const EditorInputEvent& event) noexcept {
    if (!shm_bytes_) {
        return false;
    }

    Header* const h = header();
    if (h->magic != header_magic || h->version != header_version) {
        return false;
    }

    // Only this side advances the write index, so it can be read cheaply. The
    // read index is owned by the Wine side and needs an acquire to be sure the
    // slot it released is really free.
    const uint32_t write = h->input_write.load(std::memory_order_relaxed);
    const uint32_t read = h->input_read.load(std::memory_order_acquire);

    // Unsigned arithmetic, so this stays correct when the indices wrap
    if (write - read >= input_capacity) {
        return false;
    }

    *input_slot(write) = event;

    // Release, so the Wine side cannot observe the new index before the event
    // it points at
    h->input_write.store(write + 1, std::memory_order_release);

    return true;
}

bool EditorShmSurface::pop_input(EditorInputEvent& event) noexcept {
    if (!shm_bytes_) {
        return false;
    }

    Header* const h = header();
    if (h->magic != header_magic || h->version != header_version) {
        return false;
    }

    const uint32_t read = h->input_read.load(std::memory_order_relaxed);
    const uint32_t write = h->input_write.load(std::memory_order_acquire);

    if (read == write) {
        return false;
    }

    // A write index that has run further ahead than the ring is long means the
    // other side is not playing by these rules, so nothing here can be trusted
    if (write - read > input_capacity) {
        return false;
    }

    event = *input_slot(read);

    h->input_read.store(read + 1, std::memory_order_release);

    return true;
}

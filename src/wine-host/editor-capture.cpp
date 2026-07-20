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

// See `editor-capture.h` for why the editor is copied rather than embedded.

#include "editor-capture.h"

#include <cstdio>
#include <vector>

namespace {

/**
 * The largest editor we are willing to allocate a buffer for, per dimension. No
 * plugin editor is anywhere near this, so a larger value means the window
 * dimensions have been read from a window that is being torn down or is
 * otherwise not in a sane state, and allocating from it would be a mistake.
 */
constexpr LONG max_editor_dimension = 16384;

}  // namespace

EditorCapture::EditorCapture(Logger& logger, HWND window) noexcept
    : logger_(logger), window_(window) {}

EditorCapture::~EditorCapture() noexcept {
    release_buffer();
}

void EditorCapture::release_buffer() noexcept {
    if (memory_dc_) {
        if (previous_bitmap_) {
            SelectObject(memory_dc_, previous_bitmap_);
            previous_bitmap_ = nullptr;
        }

        DeleteDC(memory_dc_);
        memory_dc_ = nullptr;
    }

    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }

    pixels_ = nullptr;
    width_ = 0;
    height_ = 0;
}

void EditorCapture::reset_buffer(uint16_t width, uint16_t height) noexcept {
    release_buffer();

    // A negative height asks for a top-down DIB, which matches how both the
    // native side and every image format we might hand these pixels to expect
    // to read them. Wine's own offscreen surfaces are laid out exactly like
    // this, so no conversion happens anywhere along the way.
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        logger_.log("Editor capture: could not get a screen DC");
        return;
    }

    void* bits = nullptr;
    dib_ = CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &bits,
                            nullptr, 0);
    ReleaseDC(nullptr, screen_dc);

    if (!dib_ || !bits) {
        logger_.log("Editor capture: could not allocate a " +
                    std::to_string(width) + "x" + std::to_string(height) +
                    " capture buffer");
        release_buffer();
        return;
    }

    memory_dc_ = CreateCompatibleDC(nullptr);
    if (!memory_dc_) {
        logger_.log("Editor capture: could not create a memory DC");
        release_buffer();
        return;
    }

    previous_bitmap_ = SelectObject(memory_dc_, dib_);

    pixels_ = static_cast<uint8_t*>(bits);
    width_ = width;
    height_ = height;
}

bool EditorCapture::capture() noexcept {
    if (!window_ || !IsWindow(window_)) {
        return false;
    }

    // Taking the size from the window rather than from the plugin's last
    // reported editor size means a resize the plugin performed on its own
    // cannot leave us blitting through a stale buffer size
    RECT client_rect{};
    if (!GetClientRect(window_, &client_rect)) {
        return false;
    }

    const LONG client_width = client_rect.right - client_rect.left;
    const LONG client_height = client_rect.bottom - client_rect.top;
    if (client_width <= 0 || client_height <= 0 ||
        client_width > max_editor_dimension ||
        client_height > max_editor_dimension) {
        return false;
    }

    if (client_width != width_ || client_height != height_) {
        reset_buffer(static_cast<uint16_t>(client_width),
                     static_cast<uint16_t>(client_height));
        if (!pixels_) {
            return false;
        }
    }

    HDC window_dc = GetDC(window_);
    if (!window_dc) {
        return false;
    }

    const BOOL blitted =
        BitBlt(memory_dc_, 0, 0, width_, height_, window_dc, 0, 0, SRCCOPY);
    ReleaseDC(window_, window_dc);

    if (!blitted) {
        // A window that cannot be blitted at all would otherwise log once per
        // frame, so only the first failure is reported
        if (!logged_capture_failure_) {
            logger_.log("Editor capture: BitBlt() failed, the editor will not");
            logger_.log("                be visible in the host");
            logged_capture_failure_ = true;
        }

        return false;
    }

    logged_capture_failure_ = false;

    // GDI batches drawing commands, and the batch is per-thread rather than per
    // DC, so without this the bits behind `pixels_` may not have been written
    // yet when we go on to read them
    GdiFlush();

    return true;
}

bool EditorCapture::dump_bmp(const std::string& path) const noexcept {
    if (!pixels_ || width_ == 0 || height_ == 0) {
        return false;
    }

    // The captured DIB is top-down and its fourth byte is undefined, neither of
    // which every image viewer copes with. Since this is a diagnostic path and
    // the cost does not matter, the rows are flipped into the bottom-up order a
    // plain BMP expects and the alpha byte is forced opaque. A viewer showing
    // nothing at all would otherwise be indistinguishable from a failed
    // capture, which is the one conclusion this function exists to rule out.
    const uint32_t row_bytes = stride();
    const uint32_t pixel_bytes = size_bytes();

    std::vector<uint8_t> flipped(pixel_bytes);
    for (uint16_t y = 0; y < height_; y++) {
        const uint8_t* source = pixels_ + (size_t{y} * row_bytes);
        uint8_t* destination =
            flipped.data() + (size_t{height_ - 1 - y} * row_bytes);

        for (uint16_t x = 0; x < width_; x++) {
            destination[(x * 4) + 0] = source[(x * 4) + 0];
            destination[(x * 4) + 1] = source[(x * 4) + 1];
            destination[(x * 4) + 2] = source[(x * 4) + 2];
            destination[(x * 4) + 3] = 0xff;
        }
    }

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};

    const uint32_t headers_size = sizeof(file_header) + sizeof(info_header);
    file_header.bfType = 0x4d42;  // "BM"
    file_header.bfOffBits = headers_size;
    file_header.bfSize = headers_size + pixel_bytes;

    info_header.biSize = sizeof(info_header);
    info_header.biWidth = width_;
    info_header.biHeight = height_;
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = pixel_bytes;

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        logger_.log("Editor capture: could not open '" + path +
                    "' for writing");
        return false;
    }

    const bool written =
        std::fwrite(&file_header, sizeof(file_header), 1, file) == 1 &&
        std::fwrite(&info_header, sizeof(info_header), 1, file) == 1 &&
        std::fwrite(flipped.data(), pixel_bytes, 1, file) == 1;
    std::fclose(file);

    if (!written) {
        logger_.log("Editor capture: could not write '" + path + "'");
        return false;
    }

    logger_.log("Editor capture: wrote a " + std::to_string(width_) + "x" +
                std::to_string(height_) + " frame to '" + path + "'");

    return true;
}

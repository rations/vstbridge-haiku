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

// See `editor-view-haiku.h` for why the editor is painted here rather than
// embedded into the host's window, and why the `BView` is defined in this file
// rather than in that header.
//
// Everything in `EditorViewImpl` runs on the host window's looper thread. That
// is a BeAPI requirement rather than a preference: a `BView` may only be
// touched with its window locked, and the looper already holds that lock while
// dispatching to the hooks below. The methods reached from elsewhere, on
// `BridgedEditorView`, take the lock themselves.

#include "editor-view-haiku.h"

#include <cstring>
#include <vector>

#include <Bitmap.h>
#include <MessageRunner.h>
#include <View.h>
#include <Window.h>

namespace {

/**
 * The `what` code for the message that drives the frame timer. Only ever sent
 * to this view, so it only has to be distinct from the BeAPI's own codes.
 */
constexpr uint32 pull_frame_message = 'vbEF';

/**
 * How often frames are pulled out of shared memory, in microseconds. This is
 * the rate the editor appears to update at, and is deliberately independent of
 * the Wine side's capture rate: the sequence number means pulling faster than
 * frames are published simply finds nothing new.
 */
constexpr bigtime_t frame_interval = 1000000 / 60;

}  // namespace

/**
 * The view the editor's frames are painted into.
 */
class EditorViewImpl : public BView {
   public:
    EditorViewImpl(BRect frame,
                   Logger& logger,
                   std::function<void()> request_surface)
        // `B_WILL_DRAW` because we paint every pixel ourselves. The view colour
        // is transparent so app_server does not erase to a background colour
        // first, which would show as a flicker between frames.
        : BView(frame,
                "vstbridge editor",
                B_FOLLOW_ALL_SIDES,
                B_WILL_DRAW | B_FRAME_EVENTS),
          logger_(logger),
          request_surface_(std::move(request_surface)) {
        SetViewColor(B_TRANSPARENT_COLOR);
    }

    void AttachedToWindow() override {
        BView::AttachedToWindow();

        // A `BMessenger` for a view is only valid once it has a looper, so the
        // timer cannot be started any earlier than this
        BMessage message(pull_frame_message);
        frame_runner_ = std::make_unique<BMessageRunner>(
            BMessenger(this), &message, frame_interval);

        if (frame_runner_->InitCheck() != B_OK) {
            logger_.log(
                "Editor view: could not start the frame timer, so the editor "
                "will not update");
            frame_runner_.reset();
        }
    }

    void DetachedFromWindow() override {
        // The runner targets this view, so it must not outlive the attachment
        frame_runner_.reset();

        BView::DetachedFromWindow();
    }

    void MessageReceived(BMessage* message) override {
        if (message->what == pull_frame_message) {
            pull_frame();
            return;
        }

        BView::MessageReceived(message);
    }

    void Draw(BRect update_rect) override {
        if (!bitmap_ || bitmap_->InitCheck() != B_OK) {
            // Nothing has arrived from the Wine side yet. Filling the view
            // rather than leaving it undefined avoids showing whatever happened
            // to be underneath it.
            SetHighColor(0, 0, 0);
            FillRect(update_rect);
            return;
        }

        DrawBitmap(bitmap_.get(), BPoint(0, 0));
    }

    bool has_surface_for(
        const EditorShmSurface::Config& config) const noexcept {
        return surface_ && surface_->config().name == config.name &&
               surface_->width() == config.width &&
               surface_->height() == config.height;
    }

    bool set_surface(const EditorShmSurface::Config& config) {
        try {
            // The Wine side created the object, so this side only maps it
            auto surface = std::make_unique<EditorShmSurface>(config, false);

            // `BRect` is inclusive on both edges, hence the -1s
            auto bitmap = std::make_unique<BBitmap>(
                BRect(0, 0, static_cast<float>(config.width) - 1,
                      static_cast<float>(config.height) - 1),
                B_RGB32);
            if (bitmap->InitCheck() != B_OK) {
                logger_.log("Editor view: could not allocate a " +
                            std::to_string(config.width) + "x" +
                            std::to_string(config.height) + " bitmap");
                return false;
            }

            surface_ = std::move(surface);
            bitmap_ = std::move(bitmap);
            last_sequence_ = 0;
            logged_read_failure_ = false;

            // Resizing is a window operation and needs the lock. Before the
            // view has been attached there is no window, and the size is picked
            // up when it is added to one.
            if (BWindow* const window = Window(); window && window->Lock()) {
                ResizeTo(static_cast<float>(config.width),
                         static_cast<float>(config.height));
                window->Unlock();
            }

            logger_.log("Editor view: showing " + std::to_string(config.width) +
                        "x" + std::to_string(config.height) + " frames from '" +
                        config.name + "'");

            return true;
        } catch (const std::exception& error) {
            logger_.log("Editor view: could not map the editor surface: " +
                        std::string(error.what()));
            return false;
        }
    }

   private:
    /**
     * Copy the newest published frame into `bitmap_` and ask for a redraw if
     * there was one. Runs on the looper thread, on the frame timer.
     */
    void pull_frame() {
        if (!surface_ || !bitmap_ || bitmap_->InitCheck() != B_OK) {
            // No surface to read yet, so keep asking for one
            maybe_request_surface();
            return;
        }

        auto* const bits = static_cast<uint8_t*>(bitmap_->Bits());
        const int32 bytes_per_row = bitmap_->BytesPerRow();
        const uint32_t frame_stride = surface_->width() * 4;

        if (!bits || bytes_per_row < static_cast<int32>(frame_stride)) {
            // Rows narrower than the frame would mean writing past the end of
            // each one
            if (!logged_read_failure_) {
                logger_.log(
                    "Editor view: the bitmap's layout does not fit the "
                    "editor's frames, so the editor will not be shown");
                logged_read_failure_ = true;
            }

            return;
        }

        // app_server may pad rows, in which case the frame has to be copied in
        // row by row. These are 32 bits per pixel and so already a multiple of
        // four bytes wide, which makes the unpadded case the common one.
        if (static_cast<uint32_t>(bytes_per_row) == frame_stride) {
            if (!surface_->read_frame(bits, bitmap_->BitsLength(),
                                      last_sequence_)) {
                maybe_request_surface();
                return;
            }
        } else {
            row_scratch_.resize(surface_->frame_size_bytes());
            if (!surface_->read_frame(row_scratch_.data(), row_scratch_.size(),
                                      last_sequence_)) {
                maybe_request_surface();
                return;
            }

            for (uint32_t y = 0; y < surface_->height(); y++) {
                std::memcpy(bits + (static_cast<size_t>(y) * bytes_per_row),
                            row_scratch_.data() +
                                (static_cast<size_t>(y) * frame_stride),
                            frame_stride);
            }
        }

        // A frame came through, so the surface is current
        ticks_without_frame_ = 0;

        // Already on the looper thread, so this needs no locking
        Invalidate();
    }

    /**
     * Ask for a fresh surface, but only after a full second of finding nothing
     * readable.
     *
     * An editor that is simply not being repainted -- the common case, since
     * most plugin GUIs are static until something moves -- looks exactly the
     * same from here as one whose surface has been re-laid-out by a resize.
     * Rather than trying to tell them apart, this asks periodically and cheaply
     * and lets the answer settle it: `set_surface()` is only called when the
     * configuration has actually changed.
     */
    void maybe_request_surface() {
        if (!request_surface_) {
            return;
        }

        // At 60 ticks a second this is one request per second at most
        if (++ticks_without_frame_ < 60) {
            return;
        }

        ticks_without_frame_ = 0;
        request_surface_();
    }

    Logger& logger_;

    /**
     * Asks whoever owns this view to fetch a current surface from the Wine
     * host. See `maybe_request_surface()`.
     */
    std::function<void()> request_surface_;

    /**
     * How many timer ticks have passed without a readable frame, used to keep
     * `maybe_request_surface()` down to one request a second.
     */
    uint32_t ticks_without_frame_ = 0;

    /**
     * The shared memory object the Wine host publishes frames to, and the
     * bitmap they are copied into. Both are null until `set_surface()`
     * succeeds, and their sizes always agree.
     */
    std::unique_ptr<EditorShmSurface> surface_;
    std::unique_ptr<BBitmap> bitmap_;

    /**
     * Drives `pull_frame()` while attached to a window.
     */
    std::unique_ptr<BMessageRunner> frame_runner_;

    /**
     * The sequence number of the last frame copied out, so an editor that is
     * not changing costs nothing beyond the timer itself.
     */
    uint64_t last_sequence_ = 0;

    /**
     * Scratch space used only when app_server pads the bitmap's rows. Stays
     * empty otherwise.
     */
    std::vector<uint8_t> row_scratch_;

    /**
     * Set once a read failure has been logged, so a broken surface cannot
     * flood the log at the frame rate.
     */
    bool logged_read_failure_ = false;
};

BridgedEditorView::BridgedEditorView(Logger& logger,
                                     std::function<void()> request_surface)
    // The real size arrives with the first surface; until then the view is
    // empty rather than wrongly sized
    : impl_(std::make_unique<EditorViewImpl>(BRect(0, 0, 0, 0),
                                             logger,
                                             std::move(request_surface))) {}

BridgedEditorView::~BridgedEditorView() {
    detach();
}

bool BridgedEditorView::attach_to(void* parent) {
    auto* const parent_view = static_cast<BView*>(parent);
    if (!parent_view || !impl_) {
        return false;
    }

    BWindow* const window = parent_view->Window();
    if (!window) {
        return false;
    }

    // This runs on whichever thread the host called `IPlugView::attached()`
    // from, which is not the looper, so the window is locked by hand
    if (!window->Lock()) {
        return false;
    }

    parent_view->AddChild(impl_.get());
    window->Unlock();

    return true;
}

void BridgedEditorView::detach() {
    if (!impl_) {
        return;
    }

    BWindow* const window = impl_->Window();
    if (!window) {
        // Never attached, or the host has already torn its window down
        return;
    }

    if (!window->Lock()) {
        return;
    }

    impl_->RemoveSelf();
    window->Unlock();
}

bool BridgedEditorView::set_surface(const EditorShmSurface::Config& config) {
    return impl_ && impl_->set_surface(config);
}

bool BridgedEditorView::has_surface_for(
    const EditorShmSurface::Config& config) const noexcept {
    return impl_ && impl_->has_surface_for(config);
}

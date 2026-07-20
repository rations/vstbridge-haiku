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

// Haiku-only. The native half of the editor bridge: a `BView` that lives inside
// the host's editor window and paints the frames the Wine host publishes.
//
// The plugin's editor runs in another process and cannot be embedded here --
// app_server rejects a parent view token owned by another team, so there is no
// equivalent of X11 reparenting. The Wine side copies the editor's pixels into
// shared memory (see `common/editor-shm.h`) and this paints them.
//
// **This header deliberately pulls in no BeAPI headers at all, and the `BView`
// itself is defined in the corresponding `.cpp`.** The Interface Kit and the
// VST3 SDK cannot coexist in one translation unit: `SupportDefs.h` and the
// SDK's `pluginterfaces/base/ftypes.h` both declare `int8`, `int16` and
// friends, and the compiler rejects the second one it sees. Everything that
// talks to the VST3 API therefore reaches the view through this class, whose
// interface names no BeAPI type -- the parent view arrives as a `void*`. This
// is the same split the Haiku Wine graphics driver uses between its C and C++
// halves, for the same reason.

#include <functional>
#include <memory>

#include "../common/editor-shm.h"
#include "../common/logging/common.h"

/**
 * The `BView` and its state. Defined in `editor-view-haiku.cpp`, where BeAPI
 * headers are safe to include.
 */
class EditorViewImpl;

/**
 * Paints a bridged plugin editor inside a view belonging to the host.
 */
class BridgedEditorView {
   public:
    /**
     * Create the view. It draws nothing until it has been attached and given a
     * surface to read.
     *
     * @param logger A logger instance.
     * @param request_surface Called when the view has gone a while without a
     *   readable frame, so that whoever owns it can ask the Wine host for a
     *   current surface and hand it back through `set_surface()`. This covers
     *   both the editor not having drawn yet when the view was created and the
     *   editor having been resized, which lays out a new object that this side
     *   does not otherwise learn about.
     *
     *   Called from the host window's looper thread, at most once a second, so
     *   an editor that never produces frames cannot turn into a stream of
     *   requests to the Wine host.
     */
    BridgedEditorView(Logger& logger, std::function<void()> request_surface);

    /**
     * Removes the view from the host's window if it is still attached.
     */
    ~BridgedEditorView();

    BridgedEditorView(const BridgedEditorView&) = delete;
    BridgedEditorView& operator=(const BridgedEditorView&) = delete;
    BridgedEditorView(BridgedEditorView&&) = delete;
    BridgedEditorView& operator=(BridgedEditorView&&) = delete;

    /**
     * Add the view to a parent view belonging to the host, locking the host's
     * window around the change.
     *
     * @param parent The `BView*` the host passed to `IPlugView::attached()`,
     *   untyped here so this header stays free of BeAPI types.
     *
     * @return Whether the view could be attached. A parent that does not belong
     *   to a window cannot be attached to.
     */
    bool attach_to(void* parent);

    /**
     * Remove the view from its parent again. Safe to call when it was never
     * attached.
     */
    void detach();

    /**
     * Start reading frames from this shared memory object, replacing any
     * previous one, and resize the view to match.
     *
     * @return Whether the object could be mapped.
     */
    bool set_surface(const EditorShmSurface::Config& config);

    /**
     * Whether a surface is mapped whose name and dimensions match `config`.
     * Used to avoid remapping when nothing has changed.
     */
    bool has_surface_for(const EditorShmSurface::Config& config) const noexcept;

   private:
    /**
     * Owned here rather than by the parent view, so that this object's lifetime
     * governs the view's. `detach()` takes it out of the host's window first.
     */
    std::unique_ptr<EditorViewImpl> impl_;
};

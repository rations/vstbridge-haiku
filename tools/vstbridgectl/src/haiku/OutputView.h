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

#include <ScrollView.h>

class BTextView;

// Read-only, fixed-font, scrolling transcript of what an action printed. Add
// this view to a layout; it wraps its own BTextView and scroll bars.
//
// Every method must be called from the looper thread, like any other view.
class OutputView : public BScrollView {
   public:
    explicit OutputView(const char* name);

    void Clear();
    // `text` need not be NUL-terminated; `length` decides.
    void Append(const char* text, int32 length);

   private:
    BTextView* m_text;
};

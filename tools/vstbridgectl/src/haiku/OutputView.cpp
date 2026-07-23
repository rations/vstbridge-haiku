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

#include "OutputView.h"

#include <Font.h>
#include <TextView.h>

OutputView::OutputView(const char* name)
    // The scrolled view has to exist before BScrollView's constructor runs;
    // members are initialised after the base, so Target() is already valid by
    // the time m_text is set up.
    : BScrollView(name,
                  new BTextView("transcript"),
                  0,
                  /*horizontal=*/true,
                  /*vertical=*/true,
                  B_FANCY_BORDER),
      m_text(dynamic_cast<BTextView*>(Target())) {
    m_text->MakeEditable(false);
    m_text->SetStylable(false);
    m_text->SetFontAndColor(be_fixed_font);
    // The actions align their output in columns, so wrapping would only
    // scramble it; the horizontal scroll bar covers long lines instead.
    m_text->SetWordWrap(false);
}

void OutputView::Clear() {
    m_text->SetText("");
}

void OutputView::Append(const char* text, int32 length) {
    m_text->Insert(m_text->TextLength(), text, length);
    m_text->ScrollToOffset(m_text->TextLength());
}

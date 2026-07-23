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

#include <Messenger.h>

#include <functional>
#include <string>
#include <thread>

// Runs one of the vstbridgectl actions (sync, status, ...) off the window's
// looper thread and streams whatever it prints into the GUI.
//
// The actions are the same functions the CLI calls and they report progress on
// stdout/stderr, so the only way to show that in a window is to capture those
// two descriptors. Two threads are involved:
//
//   - the worker runs the action, then restores stdout/stderr, which drops the
//     last write end of the pipe;
//   - the reader drains the pipe into MSG_ACTION_OUTPUT messages and, on EOF,
//     joins the worker and posts MSG_ACTION_DONE with the error text (empty if
//     the action succeeded).
//
// Neither thread touches the Interface Kit. They only call
// BMessenger::SendMessage, which is a port write and safe from any thread; all
// widget updates happen on the looper thread that receives those messages.
//
// The redirection is process-wide, so only one runner may be alive at a time.
// MainWindow enforces that with a single owning pointer. Anything else in the
// process that writes to stdout during a run lands in the same text view; that
// is the same tradeoff the GTK front-end makes.
class ActionRunner {
   public:
    // Starts the action immediately. `target` receives MSG_ACTION_OUTPUT and
    // exactly one MSG_ACTION_DONE.
    ActionRunner(BMessenger target, std::function<void()> action);
    ~ActionRunner();

    ActionRunner(const ActionRunner&) = delete;
    ActionRunner& operator=(const ActionRunner&) = delete;

   private:
    void Worker(std::function<void()> action);
    void Reader();

    BMessenger m_target;
    int m_read_fd = -1;
    int m_saved_stdout = -1;
    int m_saved_stderr = -1;
    std::ios_base::fmtflags m_saved_cout_flags{};
    // Written by the worker, read by the reader after joining it.
    std::string m_error;
    std::thread m_worker;
    std::thread m_reader;
};

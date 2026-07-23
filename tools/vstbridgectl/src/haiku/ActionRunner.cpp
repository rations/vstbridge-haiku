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

#include "ActionRunner.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <exception>
#include <iostream>
#include <utility>

#include <Message.h>

#include "Messages.h"

ActionRunner::ActionRunner(BMessenger target, std::function<void()> action)
    : m_target(target) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        // Running the action anyway would do real work with its progress and
        // warnings going nowhere, so report the failure instead. The window is
        // still owed exactly one done message.
        m_reader = std::thread([this]() {
            BMessage done(MSG_ACTION_DONE);
            done.AddString(
                "error",
                "Could not create a pipe to capture the command output");
            m_target.SendMessage(&done);
        });

        return;
    }

    m_read_fd = pipe_fds[0];
    m_saved_stdout = dup(STDOUT_FILENO);
    m_saved_stderr = dup(STDERR_FILENO);
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[1]);

    // std::cout picks full buffering because it is now writing to a pipe, which
    // would hold a long-running action's progress back in 4 KB blocks and make
    // the window look frozen. Flush on every insertion instead, so the
    // transcript keeps pace with the work. std::cerr is already unit-buffered.
    // Set before the worker starts, so the worker never races this.
    m_saved_cout_flags = std::cout.flags();
    std::cout << std::unitbuf;

    m_worker = std::thread(&ActionRunner::Worker, this, std::move(action));
    m_reader = std::thread(&ActionRunner::Reader, this);
}

ActionRunner::~ActionRunner() {
    // MainWindow only destroys the runner after MSG_ACTION_DONE, which the
    // reader posts as its very last act, so both joins return immediately.
    if (m_worker.joinable())
        m_worker.join();
    if (m_reader.joinable())
        m_reader.join();
}

void ActionRunner::Worker(std::function<void()> action) {
    try {
        action();
    } catch (const std::exception& e) {
        m_error = e.what();
    } catch (...) {
        m_error = "Unknown error";
    }

    // Flush before restoring, or buffered output ends up on the real stdout
    // after the pipe is gone.
    std::cout.flush();
    std::cout.flags(m_saved_cout_flags);
    fflush(stdout);
    fflush(stderr);

    // Restoring drops the last write end of the pipe, which is what lets the
    // reader see EOF and finish.
    dup2(m_saved_stdout, STDOUT_FILENO);
    dup2(m_saved_stderr, STDERR_FILENO);
    close(m_saved_stdout);
    close(m_saved_stderr);
    m_saved_stdout = -1;
    m_saved_stderr = -1;
}

void ActionRunner::Reader() {
    char buffer[4096];
    while (true) {
        ssize_t bytes_read = read(m_read_fd, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            // B_RAW_TYPE, and `isFixedSize` false: the chunks vary in length
            // and are not NUL-terminated, so the receiver reads them back with
            // FindData rather than FindString.
            BMessage output(MSG_ACTION_OUTPUT);
            output.AddData("text", B_RAW_TYPE, buffer, bytes_read, false);
            m_target.SendMessage(&output);
            continue;
        }

        if (bytes_read < 0 && errno == EINTR)
            continue;

        break;
    }

    close(m_read_fd);
    m_read_fd = -1;

    // The action has stopped writing, but it may not have returned yet.
    m_worker.join();

    BMessage done(MSG_ACTION_DONE);
    done.AddString("error", m_error.c_str());
    m_target.SendMessage(&done);
}

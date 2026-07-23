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

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "../config.h"
#include "VstbridgeApp.h"

int main() {
    // Append vstbridge's data directory to PATH so vstbridge-host.exe is found
    // there as well as in the packaged bin directory, matching the CLI.
    try {
        auto data_home = vstbridge_data_home();
        std::string path_env = data_home.string();
        if (const char* existing = std::getenv("PATH"))
            path_env = std::string(existing) + ":" + path_env;
        setenv("PATH", path_env.c_str(), 1);
    } catch (...) {
    }

    // Some actions offer to delete leftover files, and decide whether to ask by
    // testing whether stdin is a terminal. Launched from a terminal that test
    // passes, and the prompt would then block a thread with no way to answer
    // it, so detach stdin up front: there is no console to prompt on here.
    if (isatty(STDIN_FILENO)) {
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
    }

    VstbridgeApp app;
    app.Run();

    return 0;
}

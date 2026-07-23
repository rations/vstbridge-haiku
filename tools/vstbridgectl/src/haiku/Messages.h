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

// BMessage `what` constants for the native Haiku GUI. One flat namespace so
// MainWindow::MessageReceived can route everything; grouped by source.
enum {
    // Directories tab. The *_CHOSEN constants are what the file panels send
    // back instead of B_REFS_RECEIVED, so one window can host several panels.
    MSG_DIR_BROWSE = 'dbrw',
    MSG_DIR_CHOSEN = 'dchs',
    MSG_DIR_ADD_TYPED = 'dadd',
    MSG_DIR_REMOVE = 'drem',

    // Sync tab.
    MSG_SYNC_RUN = 'sync',

    // Status tab.
    MSG_STATUS_REFRESH = 'stat',

    // Settings tab.
    MSG_SET_BROWSE = 'sbrw',
    MSG_SET_CHOSEN = 'schs',
    MSG_SET_AUTO = 'sato',
    MSG_SET_APPLY = 'sapl',

    // Blacklist tab.
    MSG_BL_BROWSE = 'bbrw',
    MSG_BL_CHOSEN = 'bchs',
    MSG_BL_ADD_TYPED = 'badd',
    MSG_BL_REMOVE = 'brem',
    MSG_BL_CLEAR = 'bclr',

    // Posted by ActionRunner's background threads (see ActionRunner.h).
    MSG_ACTION_OUTPUT = 'aout',  // carries a "text" string
    MSG_ACTION_DONE = 'adon',    // carries an "error" string, empty on success
};

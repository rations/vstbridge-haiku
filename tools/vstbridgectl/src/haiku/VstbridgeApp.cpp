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

#include "VstbridgeApp.h"

#include "MainWindow.h"

VstbridgeApp::VstbridgeApp() : BApplication(VSTBRIDGE_APP_SIGNATURE) {}

void VstbridgeApp::ReadyToRun() {
    // The window quits the app itself once it is allowed to close.
    (new MainWindow())->Show();
}

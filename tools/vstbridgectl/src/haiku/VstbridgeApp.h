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

#include <Application.h>

// The signature is also declared in VSTBridge.rdef, which is what the registrar
// reads; keep the two in step.
static constexpr const char* VSTBRIDGE_APP_SIGNATURE =
    "application/x-vnd.vstbridge";

class VstbridgeApp : public BApplication {
   public:
    VstbridgeApp();

    void ReadyToRun() override;
};

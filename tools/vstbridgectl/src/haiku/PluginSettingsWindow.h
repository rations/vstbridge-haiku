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
#include <Window.h>

#include "../plugin_config.h"

class BCheckBox;
class BTextControl;
class BTextView;

// The settings of one plug-in, as stored in its section of vstbridge.toml.
// Opened from the Plugins tab; tells `target` with MSG_PLUGIN_SETTINGS_CHANGED
// (carrying the "index" it was opened with) after saving or resetting.
//
// Only the options that do something on Haiku are shown. XEmbed, the editor
// coordinate hack and REAPER's drag-and-drop fix are X11 editor features that
// the Haiku editor bridge doesn't use; their values in the file are kept as
// they are.
class PluginSettingsWindow : public BWindow {
   public:
    // `target_config` and `options` come from locate_plugin_config() and
    // read_plugin_options(), which the caller runs first so that a file that
    // can't be parsed is reported before a window opens.
    PluginSettingsWindow(BWindow* owner,
                         BMessenger target,
                         int32 index,
                         const char* plugin_name,
                         bool is_vst3,
                         PluginConfigTarget target_config,
                         PluginOptions options);

    void MessageReceived(BMessage* message) override;

   private:
    // Copies the controls into m_options. Returns false, after telling the
    // user why, when a value can't be saved.
    bool CollectOptions();
    void ShowError(const char* context, const char* message);
    void NotifyChanged();

    BMessenger m_target;
    int32 m_index;
    PluginConfigTarget m_target_config;
    PluginOptions m_options;

    BTextControl* m_group;
    BCheckBox* m_popup_fix;
    BCheckBox* m_disable_host_scaling;
    BCheckBox* m_hide_daw;
    BCheckBox* m_prefer_32bit;  // NULL unless the plug-in is a VST3
    BCheckBox* m_frame_rate_enabled;
    BTextControl* m_frame_rate;
    BTextView* m_environment;
};

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

#include <String.h>
#include <Window.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../config.h"
#include "../files.h"

class ActionRunner;
class BButton;
class BCheckBox;
class BFilePanel;
class BListView;
class BStringView;
class BTextControl;
class OutputView;

namespace fs = std::filesystem;

// One plug-in in the Plugins tab
struct PluginRow {
    Plugin plugin;
    std::string name;
    // Whether the plug-in has its own section in vstbridge.toml
    bool customized;
};

// The whole GUI: one window with a tab per vstbridgectl subcommand.
//
// Quick, silent operations (adding a directory, applying a setting, editing the
// blacklist) run straight on the looper thread and report failure with an
// alert. The ones that print progress — sync, status, and the leftover-file
// report from removing a directory — go through an ActionRunner instead, which
// streams their output into that tab's transcript. Only one of those may run at
// a time; see ActionRunner.h for why.
class MainWindow : public BWindow {
   public:
    MainWindow();
    ~MainWindow() override;

    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;

   private:
    BView* BuildDirectoriesTab();
    BView* BuildSyncTab();
    BView* BuildStatusTab();
    BView* BuildSettingsTab();
    BView* BuildBlacklistTab();
    BView* BuildPluginsTab();

    void RefreshDirectoryList();
    void RefreshBlacklistList();
    void RefreshSettings();

    // Lists the plug-ins in the plugin directories on a background thread,
    // since that reads every plug-in file. Does nothing while a scan runs.
    void StartPluginScan();
    void FinishPluginScan(const BMessage* message);
    void UpdatePluginItem(int32 index);
    void ConfigurePlugin(int32 index);

    // Reloads the config from disk, reporting a read failure with an alert.
    void ReloadConfig();
    void ShowError(const char* context, const char* message);

    // Runs `action` in the background, disabling `button` and streaming
    // whatever it prints into `output`. Does nothing if an action is already
    // running.
    void StartAction(OutputView* output,
                     BButton* button,
                     const char* busy_label,
                     std::function<void()> action);
    void FinishAction(const BMessage* message);

    // Path of the selected row, or an empty path when nothing is selected.
    fs::path SelectedPath(BListView* list) const;
    // Pulls the first "refs" entry out of a file panel reply.
    fs::path RefPath(const BMessage* message) const;

    Config m_config;

    // Directories tab.
    BListView* m_dir_list;
    BTextControl* m_dir_path;
    BButton* m_dir_remove;
    BFilePanel* m_dir_panel;
    OutputView* m_dir_output;

    // Sync tab.
    BCheckBox* m_sync_force;
    BCheckBox* m_sync_prune;
    BCheckBox* m_sync_verbose;
    BCheckBox* m_sync_no_verify;
    BButton* m_sync_button;
    OutputView* m_sync_output;

    // Status tab.
    BButton* m_status_button;
    OutputView* m_status_output;

    // Settings tab.
    BTextControl* m_settings_path;
    BFilePanel* m_settings_panel;
    BStringView* m_location_vst2;
    BStringView* m_location_vst3;
    BStringView* m_location_clap;
    BStringView* m_location_data;
    BStringView* m_location_config;

    // Blacklist tab.
    BListView* m_blacklist_list;
    BTextControl* m_blacklist_path;
    BFilePanel* m_blacklist_panel;

    // Plugins tab. The rows are filled in by the scanning thread's message.
    BView* m_plugins_tab;
    BListView* m_plugin_list;
    BButton* m_plugin_refresh;
    BStringView* m_plugin_status;
    std::vector<PluginRow> m_plugins;
    bool m_plugins_scanning;
    bool m_plugins_loaded;

    // Non-null exactly while a background action is running, along with the
    // button to re-enable and the transcript to append to when it finishes.
    ActionRunner* m_runner;
    BButton* m_busy_button;
    BString m_busy_button_label;
    OutputView* m_busy_output;
};

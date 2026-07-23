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

#include "MainWindow.h"

#include <Alert.h>
#include <Alignment.h>
#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <FilePanel.h>
#include <GroupView.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Messenger.h>
#include <Path.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringItem.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>

#include <exception>
#include <string>

#include "../actions.h"
#include "../actions/blacklist.h"
#include "ActionRunner.h"
#include "Messages.h"
#include "OutputView.h"

// Shows the path a lookup would use, or the reason it could not be worked out.
// The location helpers throw when find_directory() fails, which is worth
// surfacing in the window rather than swallowing.
static BString describe_location(fs::path (*locate)()) {
    try {
        return BString(locate().string().c_str());
    } catch (const std::exception& e) {
        BString message("(unavailable: ");
        message << e.what() << ")";
        return message;
    }
}

static BStringView* make_location_row(const char* name) {
    BStringView* view = new BStringView(name, "");
    view->SetExplicitAlignment(
        BAlignment(B_ALIGN_LEFT, B_ALIGN_VERTICAL_CENTER));
    return view;
}

MainWindow::MainWindow()
    : BWindow(BRect(100, 100, 820, 620),
              "VSTBridge",
              B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS),
      m_dir_list(NULL),
      m_dir_path(NULL),
      m_dir_remove(NULL),
      m_dir_panel(NULL),
      m_dir_output(NULL),
      m_sync_force(NULL),
      m_sync_prune(NULL),
      m_sync_verbose(NULL),
      m_sync_no_verify(NULL),
      m_sync_button(NULL),
      m_sync_output(NULL),
      m_status_button(NULL),
      m_status_output(NULL),
      m_settings_path(NULL),
      m_settings_panel(NULL),
      m_location_vst2(NULL),
      m_location_vst3(NULL),
      m_location_clap(NULL),
      m_location_data(NULL),
      m_location_config(NULL),
      m_blacklist_list(NULL),
      m_blacklist_path(NULL),
      m_blacklist_panel(NULL),
      m_runner(NULL),
      m_busy_button(NULL),
      m_busy_output(NULL) {
    ReloadConfig();

    BTabView* tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
    tabs->SetBorder(B_NO_BORDER);
    // BTab takes its label from the view's name (TabView.cpp, BTab::BTab).
    tabs->AddTab(BuildDirectoriesTab());
    tabs->AddTab(BuildSyncTab());
    tabs->AddTab(BuildStatusTab());
    tabs->AddTab(BuildSettingsTab());
    tabs->AddTab(BuildBlacklistTab());

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(tabs);

    RefreshDirectoryList();
    RefreshBlacklistList();
    RefreshSettings();
}

MainWindow::~MainWindow() {
    // The runner's destructor joins its threads, and those threads hold a
    // messenger to this window. Nothing else may run first.
    delete m_runner;
    delete m_dir_panel;
    delete m_settings_panel;
    delete m_blacklist_panel;
}

// ─── Tab builders ───────────────────────────────────────────────────────────

BView* MainWindow::BuildDirectoriesTab() {
    BGroupView* root = new BGroupView("Directories", B_VERTICAL);

    BStringView* hint = new BStringView(
        "hint",
        "Directories to search for Windows VST2, VST3 and CLAP plug-ins.");

    m_dir_list = new BListView("directories", B_SINGLE_SELECTION_LIST);
    BScrollView* list_scroll = new BScrollView("directories_scroll", m_dir_list,
                                               0, false, true, B_FANCY_BORDER);

    // Tracker's file panel hides dot-directories unless the user has changed
    // that setting, and Windows plug-ins normally live somewhere under
    // ~/.wine/drive_c, so a typed path has to be possible too.
    m_dir_path = new BTextControl("directory_path", "Path:", "",
                                  new BMessage(MSG_DIR_ADD_TYPED));

    BButton* add_typed =
        new BButton("add_typed", "Add", new BMessage(MSG_DIR_ADD_TYPED));
    BButton* browse = new BButton("browse", "Browse" B_UTF8_ELLIPSIS,
                                  new BMessage(MSG_DIR_BROWSE));
    m_dir_remove =
        new BButton("remove", "Remove", new BMessage(MSG_DIR_REMOVE));

    m_dir_output = new OutputView("directory_output");

    BLayoutBuilder::Group<>(root)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(hint)
        .Add(list_scroll, 3.0f)
        .AddGroup(B_HORIZONTAL)
        .Add(m_dir_path)
        .Add(add_typed)
        .End()
        .AddGroup(B_HORIZONTAL)
        .Add(browse)
        .Add(m_dir_remove)
        .AddGlue()
        .End()
        .Add(m_dir_output, 1.0f);

    return root;
}

BView* MainWindow::BuildSyncTab() {
    BGroupView* root = new BGroupView("Sync", B_VERTICAL);

    BStringView* hint = new BStringView(
        "hint",
        "Set up or update a native plug-in for every Windows plug-in found.");

    m_sync_force = new BCheckBox("force", "Force", NULL);
    m_sync_prune = new BCheckBox("prune", "Prune", NULL);
    m_sync_verbose = new BCheckBox("verbose", "Verbose", NULL);
    m_sync_no_verify = new BCheckBox("no_verify", "Skip verify", NULL);

    m_sync_button = new BButton("sync", "Sync now", new BMessage(MSG_SYNC_RUN));
    m_sync_output = new OutputView("sync_output");

    BLayoutBuilder::Group<>(root)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(hint)
        .AddGroup(B_HORIZONTAL)
        .Add(m_sync_force)
        .Add(m_sync_prune)
        .Add(m_sync_verbose)
        .Add(m_sync_no_verify)
        .AddGlue()
        .End()
        .AddGroup(B_HORIZONTAL)
        .Add(m_sync_button)
        .AddGlue()
        .End()
        .Add(m_sync_output);

    return root;
}

BView* MainWindow::BuildStatusTab() {
    BGroupView* root = new BGroupView("Status", B_VERTICAL);

    m_status_button =
        new BButton("refresh", "Refresh", new BMessage(MSG_STATUS_REFRESH));
    m_status_output = new OutputView("status_output");

    BLayoutBuilder::Group<>(root)
        .SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL)
        .Add(m_status_button)
        .AddGlue()
        .End()
        .Add(m_status_output);

    return root;
}

BView* MainWindow::BuildSettingsTab() {
    BGroupView* root = new BGroupView("Settings", B_VERTICAL);

    m_settings_path = new BTextControl("vstbridge_path", "vstbridge path:", "",
                                       new BMessage(MSG_SET_APPLY));

    BButton* browse = new BButton("browse", "Browse" B_UTF8_ELLIPSIS,
                                  new BMessage(MSG_SET_BROWSE));
    BButton* automatic =
        new BButton("auto", "Auto", new BMessage(MSG_SET_AUTO));
    automatic->SetToolTip(
        "Search the standard locations for vstbridge's files");
    BButton* apply = new BButton("apply", "Apply", new BMessage(MSG_SET_APPLY));

    BStringView* locations_heading =
        new BStringView("locations_heading", "Install locations");

    m_location_vst2 = make_location_row("location_vst2");
    m_location_vst3 = make_location_row("location_vst3");
    m_location_clap = make_location_row("location_clap");
    m_location_data = make_location_row("location_data");
    m_location_config = make_location_row("location_config");

    BLayoutBuilder::Group<>(root)
        .SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL)
        .Add(m_settings_path)
        .Add(browse)
        .Add(automatic)
        .Add(apply)
        .End()
        .Add(new BSeparatorView(B_HORIZONTAL))
        .Add(locations_heading)
        .AddGrid(B_USE_DEFAULT_SPACING, 0.0f)
        .Add(new BStringView("", "VST2:"), 0, 0)
        .Add(m_location_vst2, 1, 0)
        .Add(new BStringView("", "VST3:"), 0, 1)
        .Add(m_location_vst3, 1, 1)
        .Add(new BStringView("", "CLAP:"), 0, 2)
        .Add(m_location_clap, 1, 2)
        .Add(new BStringView("", "Data:"), 0, 3)
        .Add(m_location_data, 1, 3)
        .Add(new BStringView("", "Settings:"), 0, 4)
        .Add(m_location_config, 1, 4)
        .End()
        .AddGlue();

    return root;
}

BView* MainWindow::BuildBlacklistTab() {
    BGroupView* root = new BGroupView("Blacklist", B_VERTICAL);

    BStringView* hint = new BStringView(
        "hint", "Files and directories to skip when indexing plug-ins.");

    m_blacklist_list = new BListView("blacklist", B_SINGLE_SELECTION_LIST);
    BScrollView* list_scroll = new BScrollView(
        "blacklist_scroll", m_blacklist_list, 0, false, true, B_FANCY_BORDER);

    m_blacklist_path = new BTextControl("blacklist_path", "Path:", "",
                                        new BMessage(MSG_BL_ADD_TYPED));

    BButton* add_typed =
        new BButton("add_typed", "Add", new BMessage(MSG_BL_ADD_TYPED));
    BButton* browse = new BButton("browse", "Browse" B_UTF8_ELLIPSIS,
                                  new BMessage(MSG_BL_BROWSE));
    BButton* remove =
        new BButton("remove", "Remove", new BMessage(MSG_BL_REMOVE));
    BButton* clear =
        new BButton("clear", "Clear all", new BMessage(MSG_BL_CLEAR));

    BLayoutBuilder::Group<>(root)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(hint)
        .Add(list_scroll)
        .AddGroup(B_HORIZONTAL)
        .Add(m_blacklist_path)
        .Add(add_typed)
        .End()
        .AddGroup(B_HORIZONTAL)
        .Add(browse)
        .Add(remove)
        .Add(clear)
        .AddGlue()
        .End();

    return root;
}

// ─── Refreshing ─────────────────────────────────────────────────────────────

void MainWindow::RefreshDirectoryList() {
    m_dir_list->MakeEmpty();
    for (const auto& dir : m_config.plugin_dirs)
        m_dir_list->AddItem(new BStringItem(dir.string().c_str()));
}

void MainWindow::RefreshBlacklistList() {
    m_blacklist_list->MakeEmpty();
    for (const auto& path : m_config.blacklist)
        m_blacklist_list->AddItem(new BStringItem(path.string().c_str()));
}

void MainWindow::RefreshSettings() {
    m_settings_path->SetText(m_config.vstbridge_home
                                 ? m_config.vstbridge_home->string().c_str()
                                 : "");

    // Full Haiku paths, not ~-abbreviated: on this system the interesting part
    // is which of the config/non-packaged trees a location lives in.
    m_location_vst2->SetText(describe_location(vstbridge_vst2_home));
    m_location_vst3->SetText(describe_location(vstbridge_vst3_home));
    m_location_clap->SetText(describe_location(vstbridge_clap_home));
    m_location_data->SetText(describe_location(vstbridge_data_home));
    m_location_config->SetText(describe_location(vstbridgectl_config_dir));
}

void MainWindow::ReloadConfig() {
    try {
        m_config = Config::read();
    } catch (const std::exception& e) {
        ShowError("Could not read the configuration", e.what());
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────────

void MainWindow::ShowError(const char* context, const char* message) {
    BString text(context);
    text << ":\n\n" << message;

    BAlert* alert = new BAlert("vstbridge", text.String(), "OK", NULL, NULL,
                               B_WIDTH_AS_USUAL, B_STOP_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    alert->Go();
}

fs::path MainWindow::SelectedPath(BListView* list) const {
    int32 index = list->CurrentSelection();
    if (index < 0)
        return fs::path();

    BStringItem* item = dynamic_cast<BStringItem*>(list->ItemAt(index));
    if (item == NULL || item->Text() == NULL)
        return fs::path();

    return fs::path(item->Text());
}

fs::path MainWindow::RefPath(const BMessage* message) const {
    entry_ref ref;
    if (message->FindRef("refs", &ref) != B_OK)
        return fs::path();

    BEntry entry(&ref);
    BPath path;
    if (entry.GetPath(&path) != B_OK || path.Path() == NULL)
        return fs::path();

    return fs::path(path.Path());
}

void MainWindow::StartAction(OutputView* output,
                             BButton* button,
                             const char* busy_label,
                             std::function<void()> action) {
    if (m_runner != NULL)
        return;

    output->Clear();

    m_busy_output = output;
    m_busy_button = button;
    m_busy_button_label = button->Label();
    button->SetLabel(busy_label);
    button->SetEnabled(false);

    m_runner = new ActionRunner(BMessenger(this), std::move(action));
}

void MainWindow::FinishAction(const BMessage* message) {
    if (m_runner == NULL)
        return;

    // Joins both of the runner's threads; they have already finished by the
    // time this message arrives.
    delete m_runner;
    m_runner = NULL;

    if (m_busy_button != NULL) {
        m_busy_button->SetLabel(m_busy_button_label.String());
        m_busy_button->SetEnabled(true);
        m_busy_button = NULL;
    }
    m_busy_output = NULL;

    // The action may have rewritten the config file (sync updates the recorded
    // Wine version, removing a directory deletes leftovers).
    ReloadConfig();
    RefreshDirectoryList();
    RefreshBlacklistList();
    RefreshSettings();

    const char* error = NULL;
    if (message->FindString("error", &error) == B_OK && error != NULL &&
        error[0] != '\0') {
        ShowError("The command failed", error);
    }
}

// ─── Message routing ────────────────────────────────────────────────────────

// Whether handling this message reads or rewrites the Config object. A running
// action owns that object from its own thread, so these have to wait; the ones
// left out only move text between a file panel and a text field.
static bool touches_config(uint32 what) {
    switch (what) {
        case MSG_DIR_ADD_TYPED:
        case MSG_DIR_REMOVE:
        case MSG_SYNC_RUN:
        case MSG_STATUS_REFRESH:
        case MSG_SET_AUTO:
        case MSG_SET_APPLY:
        case MSG_BL_ADD_TYPED:
        case MSG_BL_REMOVE:
        case MSG_BL_CLEAR:
            return true;
        default:
            return false;
    }
}

void MainWindow::MessageReceived(BMessage* message) {
    // MSG_DIR_CHOSEN and MSG_BL_CHOSEN fall through into their _ADD_TYPED
    // cases, so they have to be caught here as well.
    if (m_runner != NULL &&
        (touches_config(message->what) || message->what == MSG_DIR_CHOSEN ||
         message->what == MSG_BL_CHOSEN)) {
        BAlert* alert = new BAlert(
            "vstbridge", "A command is still running. Wait for it to finish.",
            "OK", NULL, NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
        alert->Go();
        return;
    }

    switch (message->what) {
        case MSG_DIR_BROWSE: {
            if (m_dir_panel == NULL) {
                m_dir_panel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
                                             NULL, B_DIRECTORY_NODE,
                                             /*allowMultipleSelection=*/false,
                                             new BMessage(MSG_DIR_CHOSEN));
                m_dir_panel->Window()->SetTitle("Add a plug-in directory");
            }
            m_dir_panel->Show();
            break;
        }

        case MSG_DIR_CHOSEN: {
            fs::path path = RefPath(message);
            if (!path.empty())
                m_dir_path->SetText(path.string().c_str());
        }
            // Picking a directory in the panel adds it straight away; the
            // typed-path handler below does the rest of the work.
            [[fallthrough]];
        case MSG_DIR_ADD_TYPED: {
            fs::path path(m_dir_path->Text());
            if (path.empty())
                break;

            std::error_code ec;
            if (!fs::is_directory(path, ec)) {
                ShowError("Not a directory", path.string().c_str());
                break;
            }

            try {
                add_directory(m_config, path);
                m_dir_path->SetText("");
                ReloadConfig();
                RefreshDirectoryList();
            } catch (const std::exception& e) {
                ShowError("Could not add the directory", e.what());
            }
            break;
        }

        case MSG_DIR_REMOVE: {
            fs::path path = SelectedPath(m_dir_list);
            if (path.empty())
                break;

            // This one reports any leftover .so files it finds, so it runs
            // through the transcript like sync and status do.
            StartAction(m_dir_output, m_dir_remove, "Removing" B_UTF8_ELLIPSIS,
                        [this, path]() { remove_directory(m_config, path); });
            break;
        }

        case MSG_SYNC_RUN: {
            SyncOptions options{};
            options.force = m_sync_force->Value() == B_CONTROL_ON;
            options.prune = m_sync_prune->Value() == B_CONTROL_ON;
            options.verbose = m_sync_verbose->Value() == B_CONTROL_ON;
            options.no_verify = m_sync_no_verify->Value() == B_CONTROL_ON;

            StartAction(m_sync_output, m_sync_button, "Syncing" B_UTF8_ELLIPSIS,
                        [this, options]() { do_sync(m_config, options); });
            break;
        }

        case MSG_STATUS_REFRESH: {
            StartAction(m_status_output, m_status_button,
                        "Refreshing" B_UTF8_ELLIPSIS,
                        [this]() { show_status(m_config); });
            break;
        }

        case MSG_SET_BROWSE: {
            if (m_settings_panel == NULL) {
                m_settings_panel = new BFilePanel(
                    B_OPEN_PANEL, new BMessenger(this), NULL, B_DIRECTORY_NODE,
                    /*allowMultipleSelection=*/false,
                    new BMessage(MSG_SET_CHOSEN));
                m_settings_panel->Window()->SetTitle(
                    "Select the vstbridge directory");
            }
            m_settings_panel->Show();
            break;
        }

        case MSG_SET_CHOSEN: {
            fs::path path = RefPath(message);
            if (!path.empty())
                m_settings_path->SetText(path.string().c_str());
            break;
        }

        case MSG_SET_AUTO: {
            SetOptions options{};
            options.path_auto = true;
            try {
                set_settings(m_config, options);
                ReloadConfig();
                RefreshSettings();
            } catch (const std::exception& e) {
                ShowError("Could not update the settings", e.what());
            }
            break;
        }

        case MSG_SET_APPLY: {
            SetOptions options{};
            const char* text = m_settings_path->Text();
            if (text != NULL && text[0] != '\0')
                options.path = fs::path(text);
            else
                options.path_auto = true;

            try {
                set_settings(m_config, options);
                ReloadConfig();
                RefreshSettings();
            } catch (const std::exception& e) {
                ShowError("Could not update the settings", e.what());
            }
            break;
        }

        case MSG_BL_BROWSE: {
            if (m_blacklist_panel == NULL) {
                m_blacklist_panel =
                    new BFilePanel(B_OPEN_PANEL, new BMessenger(this), NULL,
                                   B_FILE_NODE | B_DIRECTORY_NODE,
                                   /*allowMultipleSelection=*/false,
                                   new BMessage(MSG_BL_CHOSEN));
                m_blacklist_panel->Window()->SetTitle("Blacklist a path");
            }
            m_blacklist_panel->Show();
            break;
        }

        case MSG_BL_CHOSEN: {
            fs::path path = RefPath(message);
            if (!path.empty())
                m_blacklist_path->SetText(path.string().c_str());
        }
            [[fallthrough]];
        case MSG_BL_ADD_TYPED: {
            fs::path path(m_blacklist_path->Text());
            if (path.empty())
                break;

            try {
                blacklist::add_path(m_config, path);
                m_blacklist_path->SetText("");
                ReloadConfig();
                RefreshBlacklistList();
            } catch (const std::exception& e) {
                ShowError("Could not blacklist the path", e.what());
            }
            break;
        }

        case MSG_BL_REMOVE: {
            fs::path path = SelectedPath(m_blacklist_list);
            if (path.empty())
                break;

            try {
                blacklist::remove_path(m_config, path);
                ReloadConfig();
                RefreshBlacklistList();
            } catch (const std::exception& e) {
                ShowError("Could not remove the path", e.what());
            }
            break;
        }

        case MSG_BL_CLEAR: {
            BAlert* confirm =
                new BAlert("vstbridge", "Clear the entire blacklist?", "Cancel",
                           "Clear", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            confirm->SetShortcut(0, B_ESCAPE);
            if (confirm->Go() != 1)
                break;

            try {
                blacklist::clear(m_config);
                ReloadConfig();
                RefreshBlacklistList();
            } catch (const std::exception& e) {
                ShowError("Could not clear the blacklist", e.what());
            }
            break;
        }

        case MSG_ACTION_OUTPUT: {
            if (m_busy_output == NULL)
                break;

            const void* text = NULL;
            ssize_t length = 0;
            if (message->FindData("text", B_RAW_TYPE, &text, &length) == B_OK &&
                length > 0) {
                m_busy_output->Append(static_cast<const char*>(text),
                                      static_cast<int32>(length));
            }
            break;
        }

        case MSG_ACTION_DONE:
            FinishAction(message);
            break;

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool MainWindow::QuitRequested() {
    // An action holds a reference to this window's messenger and writes to the
    // config file; tearing the window down underneath it would be a crash.
    if (m_runner != NULL) {
        BAlert* alert =
            new BAlert("vstbridge",
                       "A command is still running. Wait for it to finish "
                       "before quitting.",
                       "OK", NULL, NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
        alert->Go();
        return false;
    }

    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

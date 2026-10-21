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

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "../actions.h"
#include "../actions/blacklist.h"
#include "../plugin_config.h"
#include "ActionRunner.h"
#include "Messages.h"
#include "OutputView.h"
#include "PluginSettingsWindow.h"

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

// Tells the window when the Plugins tab is selected, so the plug-in list is
// only scanned once someone looks at it. Clicks, the back/forward mouse
// buttons and the keyboard all select through the virtual Select(int32)
// (TabView.cpp, BTabView::MouseDown and BTabView::KeyDown).
class PluginsAwareTabView : public BTabView {
   public:
    explicit PluginsAwareTabView(const char* name)
        : BTabView(name, B_WIDTH_FROM_LABEL), m_watched(NULL) {}

    void SetWatchedView(BView* view) { m_watched = view; }

    void Select(int32 index) override {
        BTabView::Select(index);

        BTab* tab = TabAt(Selection());
        if (m_watched != NULL && tab != NULL && tab->View() == m_watched &&
            Window() != NULL)
            Window()->PostMessage(MSG_PLUGINS_TAB_SHOWN);
    }

   private:
    BView* m_watched;
};

static BString plugin_item_text(const PluginRow& row) {
    BString text(row.name.c_str());
    text << "  (" << plugin_format_name(row.plugin) << ")";
    if (row.customized)
        text << "  \xe2\x80\x94  custom settings";
    return text;
}

static const VstbridgeFiles* try_get_files(
    const Config& config,
    std::optional<VstbridgeFiles>& storage) {
    try {
        storage = config.files();
        return &*storage;
    } catch (const std::exception&) {
        return NULL;
    }
}

static bool has_custom_settings(const Plugin& plugin,
                                const Config& config,
                                const VstbridgeFiles* files) {
    try {
        return locate_plugin_config(plugin, config, files).has_own_section;
    } catch (const std::exception&) {
        return false;
    }
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
      m_plugins_tab(NULL),
      m_plugin_list(NULL),
      m_plugin_refresh(NULL),
      m_plugin_status(NULL),
      m_plugins_scanning(false),
      m_plugins_loaded(false),
      m_runner(NULL),
      m_busy_button(NULL),
      m_busy_output(NULL) {
    ReloadConfig();

    PluginsAwareTabView* tabs = new PluginsAwareTabView("tabs");
    tabs->SetBorder(B_NO_BORDER);
    // BTab takes its label from the view's name (TabView.cpp, BTab::BTab).
    tabs->AddTab(BuildDirectoriesTab());
    tabs->AddTab(BuildSyncTab());
    tabs->AddTab(BuildStatusTab());
    tabs->AddTab(BuildSettingsTab());
    tabs->AddTab(BuildBlacklistTab());
    tabs->AddTab(BuildPluginsTab());
    tabs->SetWatchedView(m_plugins_tab);

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

BView* MainWindow::BuildPluginsTab() {
    BGroupView* root = new BGroupView("Plugins", B_VERTICAL);

    BStringView* hint = new BStringView(
        "hint",
        "Per-plug-in settings such as Wine DLL overrides, stored in "
        "vstbridge.toml.");

    m_plugin_list = new BListView("plugins", B_SINGLE_SELECTION_LIST);
    // Double-click and Enter invoke the list; BListView::AttachedToWindow
    // makes the window the target.
    m_plugin_list->SetInvocationMessage(new BMessage(MSG_PLUGIN_CONFIGURE));
    BScrollView* list_scroll = new BScrollView(
        "plugins_scroll", m_plugin_list, 0, false, true, B_FANCY_BORDER);

    BButton* configure =
        new BButton("configure", "Configure" B_UTF8_ELLIPSIS,
                    new BMessage(MSG_PLUGIN_CONFIGURE));
    m_plugin_refresh =
        new BButton("refresh", "Refresh", new BMessage(MSG_PLUGINS_REFRESH));
    m_plugin_status = new BStringView("plugin_status", "");

    BLayoutBuilder::Group<>(root)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(hint)
        .Add(list_scroll)
        .AddGroup(B_HORIZONTAL)
        .Add(configure)
        .Add(m_plugin_refresh)
        .Add(m_plugin_status)
        .AddGlue()
        .End();

    m_plugins_tab = root;
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

void MainWindow::StartPluginScan() {
    if (m_plugins_scanning)
        return;

    m_plugins_scanning = true;
    m_plugins_loaded = true;
    m_plugin_refresh->SetEnabled(false);
    m_plugin_status->SetText("Scanning" B_UTF8_ELLIPSIS);

    // The thread works on its own copy of the config, so the window can keep
    // going. If the window is gone by the time it finishes, SendMessage()
    // fails and the rows are freed here; a reply that arrives while the window
    // quits is lost along with the window.
    std::thread([target = BMessenger(this), config = m_config]() {
        auto rows = std::make_unique<std::vector<PluginRow>>();
        std::string error;
        try {
            std::optional<VstbridgeFiles> files_storage;
            const VstbridgeFiles* files = try_get_files(config, files_storage);

            for (const auto& [dir, results] : config.search_directories()) {
                for (const auto& plugin : results.plugins) {
                    rows->push_back(PluginRow{
                        plugin, plugin_display_name(plugin),
                        has_custom_settings(plugin, config, files)});
                }
            }

            std::sort(rows->begin(), rows->end(),
                      [](const PluginRow& a, const PluginRow& b) {
                          return std::lexicographical_compare(
                              a.name.begin(), a.name.end(), b.name.begin(),
                              b.name.end(), [](unsigned char x, unsigned char y) {
                                  return std::tolower(x) < std::tolower(y);
                              });
                      });
        } catch (const std::exception& e) {
            error = e.what();
        }

        BMessage message(MSG_PLUGINS_SCANNED);
        message.AddPointer("rows", rows.get());
        message.AddString("error", error.c_str());
        if (target.SendMessage(&message) == B_OK)
            rows.release();
    }).detach();
}

void MainWindow::FinishPluginScan(const BMessage* message) {
    std::unique_ptr<std::vector<PluginRow>> rows;
    void* pointer = NULL;
    if (message->FindPointer("rows", &pointer) == B_OK && pointer != NULL)
        rows.reset(static_cast<std::vector<PluginRow>*>(pointer));

    m_plugins_scanning = false;
    m_plugin_refresh->SetEnabled(true);

    if (rows)
        m_plugins = std::move(*rows);

    // MakeEmpty() doesn't free the items (ListView.cpp), so remove them one
    // by one
    for (int32 i = m_plugin_list->CountItems() - 1; i >= 0; i--)
        delete m_plugin_list->RemoveItem(i);
    for (const auto& row : m_plugins)
        m_plugin_list->AddItem(new BStringItem(plugin_item_text(row).String()));

    BString status;
    if (m_plugins.empty())
        status << "No plug-ins found in the plug-in directories.";
    else
        status << m_plugins.size()
               << (m_plugins.size() == 1 ? " plug-in" : " plug-ins");
    m_plugin_status->SetText(status.String());

    const char* error = NULL;
    if (message->FindString("error", &error) == B_OK && error != NULL &&
        error[0] != '\0') {
        ShowError("Could not list the plug-ins", error);
    }
}

void MainWindow::UpdatePluginItem(int32 index) {
    if (index < 0 || static_cast<size_t>(index) >= m_plugins.size())
        return;

    std::optional<VstbridgeFiles> files_storage;
    const VstbridgeFiles* files = try_get_files(m_config, files_storage);
    PluginRow& row = m_plugins[index];
    row.customized = has_custom_settings(row.plugin, m_config, files);

    BStringItem* item =
        dynamic_cast<BStringItem*>(m_plugin_list->ItemAt(index));
    if (item != NULL) {
        item->SetText(plugin_item_text(row).String());
        m_plugin_list->InvalidateItem(index);
    }
}

void MainWindow::ConfigurePlugin(int32 index) {
    if (index < 0 || static_cast<size_t>(index) >= m_plugins.size())
        return;

    const PluginRow& row = m_plugins[index];
    std::optional<VstbridgeFiles> files_storage;
    const VstbridgeFiles* files = try_get_files(m_config, files_storage);

    // Reading the file here, before a window opens, means a vstbridge.toml
    // that can't be parsed is reported instead of being overwritten
    PluginConfigTarget target;
    PluginOptions options;
    try {
        target = locate_plugin_config(row.plugin, m_config, files);
        options = read_plugin_options(target);
    } catch (const std::exception& e) {
        ShowError("Could not read the plug-in's settings", e.what());
        return;
    }

    PluginSettingsWindow* window = new PluginSettingsWindow(
        this, BMessenger(this), index, row.name.c_str(),
        std::holds_alternative<Vst3Module>(row.plugin), std::move(target),
        std::move(options));
    window->Show();
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
        case MSG_PLUGINS_REFRESH:
        case MSG_PLUGIN_CONFIGURE:
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

        case MSG_PLUGINS_TAB_SHOWN:
            // Reading the config while an action owns it isn't safe, so the
            // first scan waits until the user presses Refresh afterwards
            if (!m_plugins_loaded && m_runner == NULL)
                StartPluginScan();
            else if (!m_plugins_loaded)
                m_plugin_status->SetText(
                    "Press Refresh once the running command has finished.");
            break;

        case MSG_PLUGINS_REFRESH:
            StartPluginScan();
            break;

        case MSG_PLUGINS_SCANNED:
            FinishPluginScan(message);
            break;

        case MSG_PLUGIN_CONFIGURE:
            ConfigurePlugin(m_plugin_list->CurrentSelection());
            break;

        case MSG_PLUGIN_SETTINGS_CHANGED: {
            // The next Refresh shows it if an action owns the config now
            int32 index = -1;
            if (m_runner == NULL && message->FindInt32("index", &index) == B_OK)
                UpdatePluginItem(index);
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

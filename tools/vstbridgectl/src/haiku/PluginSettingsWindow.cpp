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

#include "PluginSettingsWindow.h"

#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <Font.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <Size.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>

#include "Messages.h"

// Frame rates past this are almost certainly a typo, and would only burn CPU
static constexpr double max_frame_rate = 1000.0;

// A dimmed line of text. An empty one is hidden, which takes it out of the
// layout (BGroupLayout only lays out visible items).
static BStringView* make_note(const char* name, const char* text) {
    BStringView* view = new BStringView(name, text);
    view->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);
    if (text == NULL || text[0] == '\0')
        view->Hide();
    return view;
}

static BCheckBox* make_check(const char* name,
                             const char* label,
                             const char* tooltip,
                             bool value) {
    BCheckBox* check = new BCheckBox(name, label, NULL);
    check->SetToolTip(tooltip);
    check->SetValue(value ? B_CONTROL_ON : B_CONTROL_OFF);
    return check;
}

PluginSettingsWindow::PluginSettingsWindow(BWindow* owner,
                                           BMessenger target,
                                           int32 index,
                                           const char* plugin_name,
                                           bool is_vst3,
                                           PluginConfigTarget target_config,
                                           PluginOptions options)
    : BWindow(BRect(0, 0, 520, 480),
              (BString(plugin_name) << " settings").String(),
              B_TITLED_WINDOW_LOOK,
              B_MODAL_SUBSET_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
      m_target(target),
      m_index(index),
      m_target_config(std::move(target_config)),
      m_options(std::move(options)),
      m_prefer_32bit(NULL) {
    // Where the settings go, and what applies right now
    BString file_line("File: ");
    file_line << m_target_config.toml_file.string().c_str();
    BString section_line("Section: [\"");
    section_line << m_target_config.section_key.c_str() << "\"]";
    BString state_line;
    const bool shadowed = m_target_config.matching_section &&
                          *m_target_config.matching_section !=
                              m_target_config.section_key;
    if (shadowed && m_target_config.has_own_section) {
        state_line << "The earlier [\"" << m_target_config.matching_section->c_str()
                   << "\"] section overrides it. Saving moves it to the top.";
    } else if (shadowed) {
        state_line << "This plug-in uses the [\""
                   << m_target_config.matching_section->c_str()
                   << "\"] section now. The values below start from it.";
    }

    m_group = new BTextControl("group", "Plug-in group:", "", NULL);
    m_group->SetToolTip(
        "Plug-ins with the same group name are hosted in one process, so they "
        "can communicate with each other. They also share that process's "
        "environment variables. Leave empty to host this plug-in on its own.");
    if (m_options.group)
        m_group->SetText(m_options.group->c_str());

    m_popup_fix = make_check(
        "popup_fix", "Fix popup menus that close immediately",
        "For JUCE plug-ins whose menus open and close again right away, such "
        "as Superior Drummer 3, EZdrummer 3 and Hofa plug-ins. The editor "
        "bridge normally captures menus itself, so this only matters with a "
        "visible graphics driver in the Wine prefix.",
        m_options.editor_popup_fix);
    m_disable_host_scaling = make_check(
        "disable_host_scaling", "Disable host HiDPI scaling (VST3)",
        "Don't pass the host's interface scale on to the plug-in. Try this if "
        "the editor draws at the wrong size.",
        m_options.editor_disable_host_scaling);
    m_hide_daw = make_check(
        "hide_daw", "Hide the DAW's name from the plug-in",
        "Report a different host name to the plug-in. Some plug-ins behave "
        "differently in specific DAWs.",
        m_options.hide_daw);
    // Only VST3 bundles can hold both versions, so for anything else the
    // checkbox is hidden and its value left alone
    BCheckBox* prefer_32bit = make_check(
        "prefer_32bit", "Prefer the 32-bit version",
        "Use the 32-bit version of this VST3 plug-in when the bundle has both "
        "a 32-bit and a 64-bit version.",
        m_options.vst3_prefer_32bit);
    if (is_vst3)
        m_prefer_32bit = prefer_32bit;
    else
        prefer_32bit->Hide();

    m_frame_rate_enabled = new BCheckBox(
        "frame_rate_enabled", "Editor frame rate:",
        new BMessage(MSG_PLUGIN_FRAME_RATE_TOGGLED));
    m_frame_rate_enabled->SetToolTip(
        "How often the plug-in's Win32 events are handled, which is usually "
        "also the editor's refresh rate. Defaults to 60.");
    m_frame_rate_enabled->SetValue(m_options.frame_rate ? B_CONTROL_ON
                                                        : B_CONTROL_OFF);
    std::ostringstream frame_rate;
    frame_rate << m_options.frame_rate.value_or(60.0);
    m_frame_rate = new BTextControl("frame_rate", NULL,
                                    frame_rate.str().c_str(), NULL);
    m_frame_rate->SetEnabled(m_options.frame_rate.has_value());

    m_environment = new BTextView("environment");
    m_environment->SetStylable(false);
    m_environment->SetFontAndColor(be_fixed_font);
    m_environment->SetWordWrap(false);
    std::string environment;
    for (const auto& entry : m_options.environment)
        environment += entry + "\n";
    m_environment->SetText(environment.c_str());
    BScrollView* environment_scroll =
        new BScrollView("environment_scroll", m_environment, 0,
                        /*horizontal=*/true, /*vertical=*/true, B_FANCY_BORDER);
    environment_scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 90));

    BString kept;
    for (const auto& [key, value] : m_options.other) {
        kept << (kept.IsEmpty() ? "Also kept as they are: " : ", ")
             << key.c_str();
    }

    BButton* reset = new BButton("reset", "Reset to defaults",
                                 new BMessage(MSG_PLUGIN_SETTINGS_RESET));
    reset->SetEnabled(m_target_config.has_own_section);
    reset->SetToolTip("Remove this plug-in's section from vstbridge.toml");
    BButton* cancel = new BButton("cancel", "Cancel",
                                  new BMessage(MSG_PLUGIN_SETTINGS_CANCEL));
    BButton* save =
        new BButton("save", "Save", new BMessage(MSG_PLUGIN_SETTINGS_SAVE));

    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(make_note("file", file_line.String()))
        .Add(make_note("section", section_line.String()))
        .Add(make_note("state", state_line.String()))
        .Add(new BSeparatorView(B_HORIZONTAL))
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
        .AddTextControl(m_group, 0, 0)
        .End()
        .Add(m_popup_fix)
        .Add(m_disable_host_scaling)
        .Add(m_hide_daw)
        .Add(prefer_32bit)
        .AddGroup(B_HORIZONTAL)
        .Add(m_frame_rate_enabled)
        .Add(m_frame_rate)
        .AddGlue()
        .End()
        .Add(new BStringView("environment_label",
                             "Wine environment variables:"))
        .Add(environment_scroll, 1.0f)
        .Add(make_note("environment_hint",
                       "One KEY=VALUE per line, for example "
                       "WINEDLLOVERRIDES=d3d11,dxgi=n,b"))
        .Add(make_note("kept", kept.String()))
        .Add(make_note("applies",
                       "Changes apply the next time the plug-in is loaded."))
        .AddGroup(B_HORIZONTAL)
        .Add(reset)
        .AddGlue()
        .Add(cancel)
        .Add(save)
        .End();

    SetDefaultButton(save);
    AddToSubset(owner);
    CenterIn(owner->Frame());
}

void PluginSettingsWindow::MessageReceived(BMessage* message) {
    switch (message->what) {
        case MSG_PLUGIN_FRAME_RATE_TOGGLED:
            m_frame_rate->SetEnabled(m_frame_rate_enabled->Value() ==
                                     B_CONTROL_ON);
            break;

        case MSG_PLUGIN_SETTINGS_SAVE:
            if (!CollectOptions())
                break;
            try {
                write_plugin_options(m_target_config, m_options);
            } catch (const std::exception& e) {
                ShowError("Could not save the settings", e.what());
                break;
            }
            NotifyChanged();
            PostMessage(B_QUIT_REQUESTED);
            break;

        case MSG_PLUGIN_SETTINGS_RESET:
            try {
                remove_plugin_options(m_target_config);
            } catch (const std::exception& e) {
                ShowError("Could not reset the settings", e.what());
                break;
            }
            NotifyChanged();
            PostMessage(B_QUIT_REQUESTED);
            break;

        case MSG_PLUGIN_SETTINGS_CANCEL:
            PostMessage(B_QUIT_REQUESTED);
            break;

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool PluginSettingsWindow::CollectOptions() {
    const std::string group = m_group->Text() != NULL ? m_group->Text() : "";
    m_options.group =
        group.empty() ? std::nullopt : std::optional<std::string>(group);

    m_options.editor_popup_fix = m_popup_fix->Value() == B_CONTROL_ON;
    m_options.editor_disable_host_scaling =
        m_disable_host_scaling->Value() == B_CONTROL_ON;
    m_options.hide_daw = m_hide_daw->Value() == B_CONTROL_ON;
    if (m_prefer_32bit != NULL)
        m_options.vst3_prefer_32bit = m_prefer_32bit->Value() == B_CONTROL_ON;

    if (m_frame_rate_enabled->Value() == B_CONTROL_ON) {
        const char* text = m_frame_rate->Text();
        char* end = NULL;
        const double value = text != NULL ? std::strtod(text, &end) : 0.0;
        if (text == NULL || end == text || *end != '\0' || !std::isfinite(value) ||
            value <= 0.0 || value > max_frame_rate) {
            ShowError("Not a usable frame rate",
                      "Enter a number of frames per second, such as 60.");
            return false;
        }
        m_options.frame_rate = value;
    } else {
        m_options.frame_rate = std::nullopt;
    }

    m_options.environment.clear();
    std::istringstream lines(m_environment->Text() != NULL
                                 ? m_environment->Text()
                                 : "");
    for (std::string line; std::getline(lines, line);) {
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
            continue;
        line = line.substr(first, line.find_last_not_of(" \t") - first + 1);
        if (const auto error = validate_environment_entry(line)) {
            ShowError("Not a valid environment variable", error->c_str());
            return false;
        }
        m_options.environment.push_back(line);
    }

    return true;
}

void PluginSettingsWindow::ShowError(const char* context, const char* message) {
    BString text(context);
    text << ":\n\n" << message;

    BAlert* alert = new BAlert("vstbridge", text.String(), "OK", NULL, NULL,
                               B_WIDTH_AS_USUAL, B_STOP_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    alert->Go();
}

void PluginSettingsWindow::NotifyChanged() {
    BMessage changed(MSG_PLUGIN_SETTINGS_CHANGED);
    changed.AddInt32("index", m_index);
    m_target.SendMessage(&changed);
}

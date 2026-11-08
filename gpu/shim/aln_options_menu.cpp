// SPDX-License-Identifier: GPL-2.0-or-later
#include "aln_options_ui.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include "aln_accent.h"
#include "aln_options_menu.h"
#include "localization/ui_text.h"
#include "imgui.h"

namespace AlnOptionsUi {

namespace {

using AlnOptions::Page;

constexpr uint32_t kChordButtons = (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK);

// The menu's text height as a share of the window height, so it reads the same from the couch at any size.
constexpr float kFontShareOfHeight = 1.0f / 46.0f;
constexpr float kWidthEm = 40.0f;
constexpr int kMinVisibleRows = 4;
// The footer reserves a line each for the restart banner and the status message, plus the hints line.
constexpr float kFooterOptionalLines = 2.0f;
constexpr char kArrowLeft[] = "\xE2\x80\xB9";
constexpr char kArrowRight[] = "\xE2\x80\xBA";

const ImVec4 kPanel(0.03f, 0.04f, 0.09f, 0.94f);
const ImVec4 kDim(0.64f, 0.66f, 0.76f, 1.0f);
const ImVec4 kShade(0.0f, 0.0f, 0.0f, 0.55f);

struct PadChord {
    uint32_t buttons = 0;
    aln::menu::Chord chord{kChordButtons};
};
std::map<uint32_t, PadChord> pad_chords;

enum class Kind { Setting, Info };

struct RowRef {
    Kind kind;
    size_t index;
};

struct Info {
    std::string label;
    std::string value;
};

struct State {
    Page page = Page::Display;
    int row = 0;
    int first = 0;
    bool focused = false;
    std::string status;
};
State state;

AlnOptions::Options& Model() {
    static AlnOptions::Options model{AlnSettings::Get()};
    return model;
}

std::string KeyName(int scancode) {
    const char* name = SDL_GetScancodeName(SDL_Scancode(scancode));
    return name && *name ? name : "-";
}

std::vector<Info> Hotkeys() {
    const auto& settings = AlnSettings::Get();
    return {{aln::Tr("Options menu"), KeyName(AlnOptions::kKey) + "  /  L3 + R3"},
            {aln::Tr("Display mode"), KeyName(settings.window_mode_key)},
            {aln::Tr("Frame counter"), KeyName(AlnOptions::kFrameCounterKey)}};
}

bool Offered(const AlnOptions::Setting& setting) {
    return AlnOptions::Available(setting, AlnSettings::Get());
}

std::vector<RowRef> RowsOf(Page page) {
    std::vector<RowRef> rows;
    if (page == Page::Controls) {
        for (const AlnOptions::Setting& setting : AlnOptions::kSettings) {
            if (setting.page == page && Offered(setting)) {
                rows.push_back({Kind::Setting, size_t(setting.id)});
            }
        }
        const size_t hotkeys = Hotkeys().size();
        for (size_t i = 0; i < hotkeys; ++i) {
            rows.push_back({Kind::Info, i});
        }
    } else {
        for (const AlnOptions::Setting& setting : AlnOptions::kSettings) {
            if (setting.page == page && Offered(setting)) {
                rows.push_back({Kind::Setting, size_t(setting.id)});
            }
        }
    }
    return rows;
}

std::string Label(const RowRef& row) {
    switch (row.kind) {
    case Kind::Setting: return aln::Tr(AlnOptions::kSettings[row.index].label);
    case Kind::Info: return Hotkeys()[row.index].label;
    }
    return {};
}

std::string Help(const RowRef& row) {
    switch (row.kind) {
    case Kind::Setting: {
        const AlnOptions::Setting& setting = AlnOptions::kSettings[row.index];
        return setting.apply == AlnOptions::Apply::Restart
                   ? std::string(aln::Tr(setting.help)) + " " + aln::Tr("Applies after a restart.")
                   : std::string(aln::Tr(setting.help));
    }
    case Kind::Info: return {};
    }
    return {};
}

std::string NoteOf(Page page) {
    return aln::Tr(AlnOptions::PageNote(page));
}

float RowHeight(float em) {
    return ImGui::GetFrameHeight() + em * 0.3f;
}

float WrappedHeight(const std::string& text, float wrap_width) {
    return text.empty() ? 0.0f : ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap_width).y;
}

void Change(const RowRef& row, int direction) {
    switch (row.kind) {
    case Kind::Setting: {
        const AlnOptions::Id id = AlnOptions::Id(row.index);
        const AlnOptions::Outcome outcome = direction == 0 ? Model().Cycle(id) : Model().Step(id, direction);
        state.status = outcome.changed && !outcome.saved ? "Could not save the settings." : std::string();
        break;
    }
    case Kind::Info: break;
    }
}

void SelectPage(Page page) {
    state.page = page;
    state.row = 0;
    state.first = 0;
}

// Returns true when the menu should close.
bool HandleInput(const AlnOptions::PageList& pages, int page_index, int row_count) {
    const auto pressed = [](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    if (pressed(ImGuiKey_GamepadFaceRight)) {
        return true;
    }
    if (pressed(ImGuiKey_GamepadL1) || pressed(ImGuiKey_Q)) {
        SelectPage(pages.pages[size_t(aln::menu::WrapIndex(page_index, int(pages.count), -1))]);
        return false;
    }
    if (pressed(ImGuiKey_GamepadR1) || pressed(ImGuiKey_E) || pressed(ImGuiKey_Tab)) {
        SelectPage(pages.pages[size_t(aln::menu::WrapIndex(page_index, int(pages.count), 1))]);
        return false;
    }
    if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_GamepadLStickUp, true) || pressed(ImGuiKey_UpArrow, true)) {
        state.row = aln::menu::WrapIndex(state.row, row_count, -1);
    }
    if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_GamepadLStickDown, true) ||
        pressed(ImGuiKey_DownArrow, true)) {
        state.row = aln::menu::WrapIndex(state.row, row_count, 1);
    }
    const std::vector<RowRef> rows = RowsOf(state.page);
    if (rows.empty()) {
        return false;
    }
    const RowRef& row = rows[size_t(std::clamp(state.row, 0, int(rows.size()) - 1))];
    if (pressed(ImGuiKey_GamepadDpadLeft, true) || pressed(ImGuiKey_GamepadLStickLeft, true) ||
        pressed(ImGuiKey_LeftArrow, true)) {
        Change(row, -1);
    }
    if (pressed(ImGuiKey_GamepadDpadRight, true) || pressed(ImGuiKey_GamepadLStickRight, true) ||
        pressed(ImGuiKey_RightArrow, true)) {
        Change(row, 1);
    }
    if (pressed(ImGuiKey_GamepadFaceDown) || pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) ||
        pressed(ImGuiKey_Space)) {
        Change(row, 0);
    }
    return false;
}

void DrawTabs(const AlnOptions::PageList& pages, int page_index, const ImVec4& accent, float em) {
    ImGui::TextColored(kDim, "%s[LB]", kArrowLeft);
    for (size_t i = 0; i < pages.count; ++i) {
        ImGui::SameLine(0.0f, em * 0.9f);
        const bool selected = int(i) == page_index;
        const char* name = aln::Tr(AlnOptions::PageName(pages.pages[i]));
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? accent : kDim);
        ImGui::PushID(int(i));
        if (ImGui::Selectable(name, false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(name)) && !selected) {
            SelectPage(pages.pages[i]);
        }
        if (selected) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y + 2.0f), ImVec2(max.x, max.y + 2.0f),
                                                ImGui::GetColorU32(accent), 2.0f);
        }
        ImGui::PopID();
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0.0f, em * 0.9f);
    ImGui::TextColored(kDim, "[RB]%s", kArrowRight);
}

void DrawRows(const std::vector<RowRef>& rows, int visible, const ImVec4& accent, const ImVec4& focus, float em) {
    const float height = RowHeight(em);
    const float pad = em * 0.5f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const int last = std::min(state.first + visible, int(rows.size()));
    for (int i = state.first; i < last; ++i) {
        const RowRef& row = rows[size_t(i)];
        ImGui::PushID(i);
        const bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, height));
        if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            state.row = i;
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const bool selected = i == state.row;
        if (selected) {
            draw->AddRectFilled(min, max, ImGui::GetColorU32(focus), 3.0f);
        }
        const float y = min.y + (height - ImGui::GetFontSize()) * 0.5f;
        std::string value;
        const bool lit = selected;
        bool steppable = false;
        const std::string label = Label(row);
        draw->AddText(ImVec2(min.x + pad, y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        switch (row.kind) {
        case Kind::Setting: {
            const AlnOptions::Setting& setting = AlnOptions::kSettings[row.index];
            const std::string shown = Model().Shown(AlnOptions::Id(row.index));
            const int index = AlnOptions::ChoiceIndex(setting, shown);
            value = index >= 0 ? std::string(kArrowLeft) + "  " + std::string(aln::Tr(std::string(setting.choices[size_t(index)].label))) +
                                     "  " + kArrowRight
                               : shown;
            steppable = true;
            if (setting.apply == AlnOptions::Apply::Restart) {
                const float label_w = ImGui::CalcTextSize(label.c_str()).x;
                draw->AddText(ImVec2(min.x + pad + label_w + em * 0.5f, y), ImGui::GetColorU32(kDim), aln::Tr("(restart)"));
            }
            break;
        }
        case Kind::Info: value = Hotkeys()[row.index].value; break;
        }
        const float value_w = ImGui::CalcTextSize(value.c_str()).x;
        draw->AddText(ImVec2(max.x - pad - value_w, y), ImGui::GetColorU32(lit ? accent : kDim),
                      value.c_str());
        ImGui::PopID();
        if (clicked) {
            state.row = i;
            const float x = io.MousePos.x;
            const int direction = !steppable ? 0
                                  : x >= max.x - pad - value_w && x < max.x - pad - value_w * 0.5f ? -1
                                  : x >= max.x - pad - value_w * 0.5f                                ? 1
                                                                                                     : 0;
            Change(row, direction);
        }
    }
    if (int(rows.size()) > visible) {
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float total = float(visible) * height + float(visible - 1) * spacing;
        const float x = origin.x + width + em * 0.3f;
        const float thumb = total * float(visible) / float(rows.size());
        const float offset = (total - thumb) * float(state.first) / float(int(rows.size()) - visible);
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + 3.0f, origin.y + total), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f)));
        draw->AddRectFilled(ImVec2(x, origin.y + offset), ImVec2(x + 3.0f, origin.y + offset + thumb),
                            ImGui::GetColorU32(accent));
    }
}

// Draws into the space reserved for `capacity` and pads out the rest, so what follows starts at the same Y whatever
// the page or the row is.
void ReserveSpace(float capacity, float used) {
    if (capacity > used) {
        ImGui::Dummy(ImVec2(0.0f, capacity - used));
    }
}

void DrawWrapped(const std::string& text, float capacity) {
    const float start_y = ImGui::GetCursorPosY();
    if (!text.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX());
        ImGui::TextColored(kDim, "%s", text.c_str());
        ImGui::PopTextWrapPos();
    }
    ReserveSpace(capacity, ImGui::GetCursorPosY() - start_y);
}

void DrawFooter(const ImVec4& accent, float line) {
    const float start_y = ImGui::GetCursorPosY();
    if (Model().RestartPending()) {
        ImGui::TextColored(accent, "%s", aln::Tr(AlnOptions::kRestartNote));
    }
    if (!state.status.empty()) {
        ImGui::TextColored(accent, "%s", aln::Tr(state.status).c_str());
    }
    ReserveSpace(line * kFooterOptionalLines, ImGui::GetCursorPosY() - start_y);
    const std::string hints = std::string(aln::Tr("D-pad / Arrows  Select")) + "     " + kArrowLeft + " " +
                              kArrowRight + " / A  " + aln::Tr("Change") + "     [LB] [RB] / Q E  " +
                              aln::Tr("Page") + "     B / Esc  " + aln::Tr("Back");
    ImGui::TextColored(kDim, "%s", hints.c_str());
}

void Draw(const AlnOptions::PageList& pages, int page_index, float em, float scales) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 accent(aln::kAccentRed / 255.0f, aln::kAccentGreen / 255.0f, aln::kAccentBlue / 255.0f, 1.0f);
    const ImVec4 focus(accent.x, accent.y, accent.z, 0.38f);

    const float width = std::min(em * kWidthEm, io.DisplaySize.x - em * 2.0f);
    const float wrap_width = width - em * 2.0f;
    const Page page = state.page;

    float note_capacity = 0.0f, help_capacity = 0.0f;
    size_t max_rows = 0;
    for (size_t i = 0; i < pages.count; ++i) {
        const Page candidate = pages.pages[i];
        const std::vector<RowRef> rows = RowsOf(candidate);
        max_rows = std::max(max_rows, rows.size());
        note_capacity = std::max(note_capacity, WrappedHeight(NoteOf(candidate), wrap_width));
        for (const RowRef& row : rows) {
            help_capacity = std::max(help_capacity, WrappedHeight(Help(row), wrap_width));
        }
    }
    note_capacity += em * 0.2f;
    help_capacity += em * 0.2f;

    ImGui::PushFont(nullptr, em * 1.2f / scales);
    const float title_height = ImGui::GetTextLineHeightWithSpacing();
    ImGui::PopFont();
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float separator = style.ItemSpacing.y * 2.0f + 1.0f;
    const float row_height = RowHeight(em);
    const float chrome = em * 0.8f * 2.0f + title_height + line + separator + note_capacity + separator +
                         help_capacity + line * (kFooterOptionalLines + 1.0f);
    const int fit = int((io.DisplaySize.y - em * 2.0f - chrome) / (row_height + style.ItemSpacing.y));
    const int visible = std::clamp(fit, kMinVisibleRows, int(max_rows));
    const float height = chrome + float(visible) * row_height + style.ItemSpacing.y * float(visible - 1);

    const std::vector<RowRef> rows = RowsOf(page);
    state.row = std::clamp(state.row, 0, std::max(int(rows.size()) - 1, 0));
    state.first = aln::menu::ScrollToShow(state.row, state.first, visible, int(rows.size()));

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, ImGui::GetColorU32(kShade));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    if (!state.focused) {
        state.focused = true;
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, focus);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, focus);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.0f, em * 0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##aln_options_menu", nullptr, flags)) {
        ImGui::PushFont(nullptr, em * 1.2f / scales);
        ImGui::TextColored(accent, "%s", aln::Tr(AlnOptions::kTitle));
        ImGui::PopFont();
        DrawTabs(pages, page_index, accent, em);
        ImGui::Separator();
        DrawRows(rows, visible, accent, focus, em);
        if (visible > int(rows.size())) {
            ImGui::Dummy(ImVec2(0.0f, float(visible - int(rows.size())) * (row_height + style.ItemSpacing.y)));
        }
        DrawWrapped(NoteOf(page), note_capacity);
        ImGui::Separator();
        const std::string help = rows.empty() ? std::string() : Help(rows[size_t(state.row)]);
        DrawWrapped(help, help_capacity);
        DrawFooter(accent, line);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

} // namespace

void Reset() {
    state = {};
}

Result Frame() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float scales = std::max(style.FontScaleMain * style.FontScaleDpi, 0.01f);
    const float em = std::max(ImGui::GetFontSize(), io.DisplaySize.y * kFontShareOfHeight);
    const AlnOptions::PageList pages = AlnOptions::VisiblePages();
    const int recovered = AlnOptions::RecoverPage(pages, state.page);
    if (pages.pages[size_t(recovered)] != state.page) {
        SelectPage(pages.pages[size_t(recovered)]);
    }
    const int row_count = int(RowsOf(state.page).size());
    if (HandleInput(pages, recovered, row_count)) {
        return Result::Close;
    }
    ImGui::PushFont(nullptr, em / scales);
    Draw(pages, AlnOptions::PageIndex(pages, state.page), em, scales);
    ImGui::PopFont();
    return Result::Stay;
}

bool TrackChord(uint32_t pad, int sdl_button, bool down) {
    if (sdl_button < 0 || sdl_button >= 32) {
        return false;
    }
    PadChord& chord = pad_chords[pad];
    const uint32_t bit = 1u << sdl_button;
    chord.buttons = down ? chord.buttons | bit : chord.buttons & ~bit;
    return chord.chord.Poll(chord.buttons);
}

void ForgetPad(uint32_t pad) {
    pad_chords.erase(pad);
}

} // namespace AlnOptionsUi

#pragma once

// Form pieces shared by the MealKit tools (the same file in MkAudio, MkUI, MkRender and MkQA tool/Form.h; engine docs/TOOL_DESIGN.md 3.2).
// They replace the habits that make a tool read as a stock Dear ImGui window:
//   label()   the name in a left column and the control filling the rest, so values line up ("Volume [====  0.90]",
//             not ImGui's "[0.90] Volume" with the name after the field)
//   slider()  a bar that fills up to the value instead of a knob drawn over the number
//   section() a title with space above instead of ImGui's "--- Title -------" separator line
//   chip()    a pill that toggles a filter instead of a row of checkboxes
// Keep the copies the same: change one, copy it to the other three.
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace mktool::form {

inline float dpi() { return ImGui::GetStyle().FontScaleDpi; }

// Accent of TOOL_DESIGN 3.1 (Spectrum 2 blue 800/900)
inline ImVec4 accent(float alpha = 1.0f) { return ImVec4(0x3B / 255.0f, 0x63 / 255.0f, 0xFB / 255.0f, alpha); }

// Left column: under a third of the panel, 96..170 px at 100%
inline float labelWidth() { return std::clamp(ImGui::GetContentRegionAvail().x * 0.3f, 96.0f * dpi(), 170.0f * dpi()); }

// Starts a row: the name in the left column (cut at the column edge, whole in a tooltip), then the cursor in the value column
// with the next item as wide as what is left. Returns that width, for rows that put two controls side by side
inline float label(const char* text, const char* tooltip = nullptr) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float column = labelWidth();
    const float x0 = ImGui::GetCursorPosX();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    const float room = column - style.ItemSpacing.x;
    ImGui::Dummy(ImVec2(room, height));
    const char* end = ImGui::FindRenderedTextEnd(text);
    const bool cut = ImGui::CalcTextSize(text, end).x > room;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(pos, ImVec2(pos.x + room, pos.y + height), true);
    // Names a step dimmer than values and section titles (gray 11, the secondary text step)
    draw->AddText(ImVec2(pos.x, pos.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), text, end);
    draw->PopClipRect();
    if ((cut || tooltip) && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        if (cut && tooltip) ImGui::SetTooltip("%.*s\n%s", static_cast<int>(end - text), text, tooltip);
        else if (cut) ImGui::SetTooltip("%.*s", static_cast<int>(end - text), text);
        else ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SameLine(x0 + column);
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(width);
    return width;
}

// A row with only text in the value column (read-only values, hints next to a name)
inline void labelText(const char* name, const char* value) {
    label(name);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", value);
}

// Section title: a step brighter and larger than the rows, space above it, no line
inline void section(const char* title) {
    const float d = dpi();
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight()) ImGui::Dummy(ImVec2(0.0f, 10.0f * d));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.07f); // unscaled size: ImGui applies the DPI scale
    ImGui::TextUnformatted(title, ImGui::FindRenderedTextEnd(title));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 2.0f * d));
}

namespace detail {
// Where along [lo, hi] the value sits, 0..1 (logarithmic like ImGui when asked; 0 and below count as the start)
inline float fraction(float v, float lo, float hi, bool logarithmic) {
    if (hi == lo) return 0.0f;
    if (logarithmic && lo >= 0.0f && hi > 0.0f) {
        const float base = std::max(lo, hi * 1e-4f);
        if (v <= base) return 0.0f;
        return std::clamp(std::log(v / base) / std::log(hi / base), 0.0f, 1.0f);
    }
    return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
}

// Draws the field and its fill where the slider will be, then hides ImGui's own frame and knob for the slider call
inline void beginFill(const char* id, float t) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + ImGui::CalcItemWidth(), a.y + ImGui::GetFrameHeight());
    const ImGuiID itemId = ImGui::GetID(id);
    const bool active = ImGui::GetActiveID() == itemId;
    const bool hovered = !active && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(a, b, ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), style.FrameRounding);
    if (t > 0.0f) {
        draw->PushClipRect(a, ImVec2(a.x + (b.x - a.x) * t, b.y), true);
        draw->AddRectFilled(a, b, ImGui::GetColorU32(accent(active ? 0.75f : hovered ? 0.6f : 0.5f)), style.FrameRounding);
        draw->PopClipRect();
    }
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clear);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, clear);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, clear);
}
inline void endFill() { ImGui::PopStyleColor(5); }
} // namespace detail

// A slider drawn as a bar filled up to the value, the number on top always readable
inline bool slider(const char* id, float* v, float lo, float hi, const char* format = "%.2f", ImGuiSliderFlags flags = 0) {
    detail::beginFill(id, detail::fraction(*v, lo, hi, (flags & ImGuiSliderFlags_Logarithmic) != 0));
    const bool changed = ImGui::SliderFloat(id, v, lo, hi, format, flags);
    detail::endFill();
    return changed;
}

inline bool slider(const char* id, int* v, int lo, int hi, const char* format = "%d", ImGuiSliderFlags flags = 0) {
    detail::beginFill(id, detail::fraction(static_cast<float>(*v), static_cast<float>(lo), static_cast<float>(hi), false));
    const bool changed = ImGui::SliderInt(id, v, lo, hi, format, flags);
    detail::endFill();
    return changed;
}

// One choice of a segmented control (pages of one window, like MealKit Manager's [Projects | Tools]): call between
// beginSegments and endSegments, one per choice. The picked one is a raised gray pill, the rest plain text. True when clicked
inline void beginSegments() {
    const float height = ImGui::GetFrameHeight();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSplit(2); // the track goes under the pills once their total width is known
    draw->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, height * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * dpi(), ImGui::GetStyle().ItemSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(height * 0.6f, ImGui::GetStyle().FramePadding.y));
}
inline bool segment(const char* text, bool selected) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? style.Colors[ImGuiCol_ButtonActive] : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? style.Colors[ImGuiCol_ButtonActive] : style.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, style.Colors[ImGuiCol_ButtonActive]);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? style.Colors[ImGuiCol_Text] : style.Colors[ImGuiCol_TextDisabled]);
    const bool pressed = ImGui::Button(text);
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    return pressed;
}
inline void endSegments() {
    ImGui::PopStyleVar(3);
    ImGui::NewLine();
    ImGui::EndGroup();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSetCurrent(0);
    const float pad = 3.0f * dpi();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    draw->AddRectFilled(ImVec2(a.x - pad, a.y - pad), ImVec2(b.x + pad, a.y + ImGui::GetFrameHeight() + pad),
                        ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetFrameHeight() * 0.5f + pad);
    draw->ChannelsMerge();
}

// A filter pill: on = accent fill, off = an outline. Returns true when clicked (the value is already flipped)
inline bool chip(const char* text, bool* on) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, height * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(height * 0.5f, style.FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_Button, *on ? accent() : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, *on ? accent(0.85f) : style.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, *on ? accent(0.7f) : style.Colors[ImGuiCol_FrameBgActive]);
    const bool pressed = ImGui::Button(text);
    ImGui::PopStyleColor(3);
    if (!*on) {
        const float width = std::max(1.0f, dpi());
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Border), height * 0.5f, 0, width);
    }
    ImGui::PopStyleVar(2);
    if (pressed) *on = !*on;
    return pressed;
}

} // namespace mktool::form

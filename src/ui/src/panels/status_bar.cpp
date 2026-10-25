// Status bar and shortcut cheat sheet.
#include <ct/ui/panels/status_bar.h>

#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/shortcuts.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ct::ui {

namespace {

void Dot(ImDrawList* dl) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    dl->AddCircleFilled(ImVec2(p.x + Dp(4), p.y + h * 0.5f), Dp(1.5f), col::text_faint);
    ImGui::Dummy(ImVec2(Dp(8), h));
    ImGui::SameLine(0, Dp(8));
}

void Dim(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

} // namespace

StatusBarClick DrawStatusBar(const EditorState& state, const StatusInfo& info) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(12, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg0));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
    StatusBarClick clicked = StatusBarClick::None;
    if (ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, Dp(kStatusBarHeight), flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        dl->AddLine(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y), col::border);
        FontScope f(FontRole::Label);
        ImGui::SetCursorPosY((Dp(kStatusBarHeight) - ImGui::GetTextLineHeight()) * 0.5f);
        char buf[96];
        const bool dirty = state.IsDirty();
        dl->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + Dp(4), ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f), Dp(3.5f),
                            dirty ? col::warn : col::success);
        ImGui::Dummy(ImVec2(Dp(10), ImGui::GetTextLineHeight()));
        ImGui::SameLine(0, Dp(6));
        if (info.autosave_age_s >= 0) std::snprintf(buf, sizeof(buf), str::kAutosavedAgo, static_cast<int>(info.autosave_age_s));
        else std::snprintf(buf, sizeof(buf), "%s", dirty ? str::kUnsaved : str::kSaved);
        Dim(buf);
        ImGui::SameLine(0, Dp(8));
        Dot(dl);
        const auto& cv = state.project.canvas;
        if (cv.fps_den == 1) std::snprintf(buf, sizeof(buf), "%d fps", cv.fps_num);
        else std::snprintf(buf, sizeof(buf), "%.3f fps", static_cast<double>(cv.fps_num) / cv.fps_den);
        Dim(buf);
        ImGui::SameLine(0, Dp(8));
        Dot(dl);
        std::snprintf(buf, sizeof(buf), "%d\xC3\x97%d", cv.width, cv.height);
        Dim(buf);
        ImGui::SameLine(0, Dp(8));
        Dot(dl);
        if (info.cache_mb >= 0) std::snprintf(buf, sizeof(buf), "cache %.0f/%.0f MB", info.cache_mb, info.cache_budget_mb);
        else std::snprintf(buf, sizeof(buf), "%s", str::kCacheNA);
        Dim(buf);
        if (!state.last_error.empty()) {
            ImGui::SameLine(0, Dp(8));
            Dot(dl);
            ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::warn));
            ImGui::TextUnformatted(state.last_error.c_str());
            ImGui::PopStyleColor();
        }

        // Right side: export progress (while one runs), warnings, GPU time.
        char gpu[32];
        if (info.gpu_ms >= 0) std::snprintf(gpu, sizeof(gpu), "GPU %.1f ms", info.gpu_ms);
        else std::snprintf(gpu, sizeof(gpu), "%s", str::kGpuNA);
        char warn[16];
        std::snprintf(warn, sizeof(warn), "%d", info.warnings);
        const bool exporting = info.export_fraction >= 0;
        char exp[48] = "";
        const float bar_w = Dp(80);
        if (exporting) {
            const double pct = std::clamp(info.export_fraction, 0.0, 1.0) * 100.0;
            std::snprintf(exp, sizeof(exp), str::kStatusExportingFmt, static_cast<int>(pct));
        }
        const float exp_w = exporting ? ImGui::CalcTextSize(exp).x + bar_w + Dp(8 + 20) : 0;
        const float right_w = ImGui::CalcTextSize(gpu).x + ImGui::CalcTextSize(warn).x + Dp(64) + exp_w;
        ImGui::SameLine(ImGui::GetWindowWidth() - right_w);
        if (exporting) {
            // One clickable item: label + a small progress bar; click shows the export progress window again.
            const float h = ImGui::GetTextLineHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float tw = ImGui::CalcTextSize(exp).x;
            const ImVec2 size(tw + Dp(8) + bar_w, h);
            if (ImGui::InvisibleButton("##exportpill", size)) clicked = StatusBarClick::Export;
            const bool hov = ImGui::IsItemHovered();
            dl->AddText(p, hov ? col::text : col::text_dim, exp);
            const ImVec2 b0(p.x + tw + Dp(8), p.y + h * 0.5f - Dp(3));
            const ImVec2 b1(b0.x + bar_w, b0.y + Dp(6));
            dl->AddRectFilled(b0, b1, col::bg3, Dp(3));
            const float fw = static_cast<float>(std::clamp(info.export_fraction, 0.0, 1.0)) * bar_w;
            if (fw > 0) dl->AddRectFilled(b0, ImVec2(b0.x + fw, b1.y), col::accent, Dp(3));
            ItemTooltip(str::kStatusExportTooltip);
            ImGui::SameLine(0, Dp(20));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(info.warnings ? col::warn : col::text_faint));
        {
            FontScope icon(FontRole::Label);
            ImGui::TextUnformatted(ICON_WARNING);
        }
        if (ImGui::IsItemClicked()) clicked = StatusBarClick::Warnings;
        ImGui::SameLine(0, Dp(4));
        ImGui::TextUnformatted(warn);
        if (ImGui::IsItemClicked()) clicked = StatusBarClick::Warnings;
        ImGui::PopStyleColor();
        ImGui::SameLine(0, Dp(14));
        Dim(gpu);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return clicked;
}

void DrawShortcutSheet(bool* open) {
    if (!*open) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(FitViewport(Dp(760, 560)), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(20, 16));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg1));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin(str::kCheatSheetTitle, open, flags)) {
        {
            FontScope t(FontRole::Title);
            ImGui::TextUnformatted(str::kCheatSheetTitle);
        }
        Dim(str::kCheatSheetHint);
        ImGui::Dummy(Dp(0, 6));
        size_t n = 0;
        const ShortcutDef* table = Shortcuts::Table(&n);
        if (ImGui::BeginTable("##groups", 2, ImGuiTableFlags_SizingStretchSame)) {
            const ShortcutGroup order[] = {ShortcutGroup::Playback, ShortcutGroup::Editing, ShortcutGroup::Timeline, ShortcutGroup::File, ShortcutGroup::View};
            for (int gi = 0; gi < 5; ++gi) {
                if (gi == 0 || gi == 2) ImGui::TableNextColumn();
                const ShortcutGroup g = order[gi];
                {
                    FontScope s(FontRole::Strong);
                    ImGui::TextUnformatted(Shortcuts::GroupName(g));
                }
                ImGui::Dummy(Dp(0, 2));
                const char* last_desc = nullptr;
                for (size_t i = 0; i < n; ++i) {
                    const ShortcutDef& d = table[i];
                    if (d.group != g) continue;
                    // Entries sharing a description (Delete / Backspace) share a row.
                    if (last_desc && std::strcmp(last_desc, d.description) == 0) continue;
                    last_desc = d.description;
                    char keys[64];
                    std::snprintf(keys, sizeof(keys), "%s", d.keys);
                    for (size_t j = i + 1; j < n; ++j)
                        if (table[j].group == g && std::strcmp(table[j].description, d.description) == 0) {
                            const size_t len = std::strlen(keys);
                            std::snprintf(keys + len, sizeof(keys) - len, " / %s", table[j].keys);
                        }
                    // Key cap
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    FontScope mono(FontRole::Mono);
                    const ImVec2 ts = ImGui::CalcTextSize(keys);
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(p, ImVec2(p.x + ts.x + Dp(12), p.y + ts.y + Dp(6)), col::bg3, Dp(5));
                    dl->AddText(ImVec2(p.x + Dp(6), p.y + Dp(3)), col::text, keys);
                    ImGui::Dummy(ImVec2(Dp(130), ts.y + Dp(6)));
                    ImGui::SameLine(0, 0);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(3));
                    FontScope body(FontRole::Body);
                    Dim(d.description);
                }
                ImGui::Dummy(Dp(0, 10));
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace ct::ui

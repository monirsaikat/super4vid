// Error banners at the top of a panel (docs/05 section 4).
#include <ct/ui/editor_banners.h>

#include <ct/core/log.h>
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui.h>

#include <algorithm>

namespace ct::ui {

using namespace metrics;

int BannerStack::Show(BannerPanel panel, std::string message, std::string details) {
    for (Banner& b : m_banners)
        if (b.panel == panel && b.message == message) {
            b.details = std::move(details);
            return b.id;
        }
    const int id = m_next++;
    m_banners.push_back(Banner{id, panel, std::move(message), std::move(details)});
    if (m_banners.size() > 16) m_banners.erase(m_banners.begin());
    return id;
}

void BannerStack::Dismiss(int id) {
    m_banners.erase(std::remove_if(m_banners.begin(), m_banners.end(), [&](const Banner& b) { return b.id == id; }), m_banners.end());
}

void BannerStack::Clear(BannerPanel panel) {
    m_banners.erase(std::remove_if(m_banners.begin(), m_banners.end(), [&](const Banner& b) { return b.panel == panel; }), m_banners.end());
}

size_t BannerStack::Count(BannerPanel panel) const {
    return static_cast<size_t>(std::count_if(m_banners.begin(), m_banners.end(), [&](const Banner& b) { return b.panel == panel; }));
}

void BannerStack::Draw(BannerPanel panel) {
    int dismiss = 0;
    for (Banner& b : m_banners) {
        if (b.panel != panel) continue;
        ImGui::PushID(b.id);
        const float w = ImGui::GetContentRegionAvail().x;
        const float pad = Dp(10);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float text_w = std::max(Dp(40), w - pad * 2 - Dp(56));
        const ImVec2 ms = ImGui::CalcTextSize(b.message.c_str(), nullptr, false, text_w);
        float h = std::max(Dp(32), ms.y + pad * 2);
        const bool has_details = !b.details.empty() || Log::FilePathUtf8()[0];
        std::string det;
        if (b.details_open) {
            det = b.details;
            if (Log::FilePathUtf8()[0]) {
                if (!det.empty()) det += "\n";
                det += "Log: ";
                det += Log::FilePathUtf8();
            }
            h += ImGui::CalcTextSize(det.c_str(), nullptr, false, text_w).y + Dp(6);
        }
        dl->AddRectFilled(p, p + ImVec2(w, h), WithAlpha(col::danger, 0.16f), Dp(kRadiusControl));
        dl->AddRect(p, p + ImVec2(w, h), WithAlpha(col::danger, 0.55f), Dp(kRadiusControl));
        {
            FontScope f(FontRole::Icon);
            dl->AddText(p + ImVec2(pad, pad), col::danger, ICON_ERROR);
        }
        ImGui::SetCursorScreenPos(p + ImVec2(pad + Dp(22), pad - Dp(1)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
        ImGui::TextUnformatted(b.message.c_str());
        if (b.details_open) {
            ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
            ImGui::TextUnformatted(det.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        // Details toggle + close, top right.
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - Dp(54), p.y + Dp(4)));
        if (has_details && IconButton("det", b.details_open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, "Details", b.details_open, nullptr, Dp(24, 24)))
            b.details_open = !b.details_open;
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - Dp(28), p.y + Dp(4)));
        if (IconButton("close", ICON_CLOSE, "Dismiss", false, nullptr, Dp(24, 24))) dismiss = b.id;
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + Dp(6)));
        ImGui::Dummy(ImVec2(w, 0));
        ImGui::PopID();
    }
    if (dismiss) Dismiss(dismiss);
}

} // namespace ct::ui

// Error banners at the top of the editor panels: one P11a ErrorBanner per panel (docs/05 section 4).
#include <ct/ui/editor_banners.h>

namespace ct::ui {

int BannerStack::Show(BannerPanel panel, std::string message, std::string details) {
    const int p = static_cast<int>(panel);
    m_banners[p].Show(std::move(message), 0, std::move(details));
    m_ids[p] = m_next++;
    return m_ids[p];
}

void BannerStack::Dismiss(int id) {
    for (int p = 0; p < kPanels; ++p)
        if (m_ids[p] == id) m_banners[p].Clear();
}

void BannerStack::Clear(BannerPanel panel) { m_banners[static_cast<int>(panel)].Clear(); }

size_t BannerStack::Count(BannerPanel panel) const { return m_banners[static_cast<int>(panel)].Visible() ? 1 : 0; }

const std::string* BannerStack::Message(BannerPanel panel) const {
    const ErrorBanner& b = m_banners[static_cast<int>(panel)];
    return b.Visible() ? &b.Message() : nullptr;
}

void BannerStack::Draw(BannerPanel panel) {
    static const char* const kIds[kPanels] = {"##banner_media", "##banner_preview", "##banner_props", "##banner_timeline"};
    const int p = static_cast<int>(panel);
    m_banners[p].Draw(kIds[p]);
}

} // namespace ct::ui

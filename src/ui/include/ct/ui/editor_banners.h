#pragma once
// ct/ui/editor_banners.h
// The editor's per-panel error banners (docs/05 section 4: "a non-modal red banner at the top of the panel
// concerned, with the plain-language message and a Details disclosure"): one P11a ErrorBanner (error_ux.h) per
// panel; a new error on a panel replaces the previous one.
// Thread model: UI thread, inside a frame.

#include <ct/ui/error_ux.h>

#include <cstddef>
#include <string>

namespace ct::ui {

enum class BannerPanel { Media, Preview, Properties, Timeline, Count };

class BannerStack {
public:
    // Shows `message` on the panel (details: free-form extra lines, e.g. the path). Returns an id for Dismiss.
    int Show(BannerPanel panel, std::string message, std::string details = {});
    void Dismiss(int id);
    void Clear(BannerPanel panel);
    // Draws the panel's banner at the cursor (full content width). Call first inside the panel's window.
    void Draw(BannerPanel panel);
    size_t Count(BannerPanel panel) const;

private:
    static constexpr int kPanels = static_cast<int>(BannerPanel::Count);
    ErrorBanner m_banners[kPanels];
    int m_ids[kPanels] = {};
    int m_next = 1;
};

} // namespace ct::ui

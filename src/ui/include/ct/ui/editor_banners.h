#pragma once
// ct/ui/editor_banners.h
// Minimal error banners of docs/05 section 4 ("a non-modal red banner at the top of the panel concerned, with the
// plain-language message and a Details disclosure showing the HRESULT string and log path"), until P11a's shared
// error-UX helper is merged; the editor then switches to it.
// Thread model: UI thread, inside a frame.

#include <string>
#include <vector>

namespace ct::ui {

enum class BannerPanel { Media, Preview, Properties, Timeline, Count };

class BannerStack {
public:
    // Shows (or refreshes) a banner; an identical message on the same panel is not duplicated. Returns its id.
    int Show(BannerPanel panel, std::string message, std::string details = {});
    void Dismiss(int id);
    void Clear(BannerPanel panel);
    // Draws the panel's banners at the cursor (full content width). Call first inside the panel's window.
    void Draw(BannerPanel panel);
    size_t Count(BannerPanel panel) const;

private:
    struct Banner {
        int id;
        BannerPanel panel;
        std::string message, details;
        bool details_open = false;
    };
    std::vector<Banner> m_banners;
    int m_next = 1;
};

} // namespace ct::ui

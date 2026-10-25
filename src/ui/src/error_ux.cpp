// Error messages and the red error banner (P11 11.5, docs/05 section 4).
#include <algorithm>
#include <ct/ui/error_ux.h>

#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/platform/paths.h>
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <audioclient.h>
#include <dxgi.h>
#include <mferror.h>

#include <cmath>
#include <cstdio>

namespace ct::ui {

std::string FillPlaceholder(std::string_view fmt, std::string_view arg) {
    std::string out(fmt);
    const size_t at = out.find("%s");
    if (at != std::string::npos) out.replace(at, 2, arg);
    return out;
}

std::string DriveOf(std::string_view p) {
    if (p.size() >= 4 && (p.substr(0, 4) == "\\\\?\\" || p.substr(0, 4) == "//?/")) {
        p.remove_prefix(4);
        if (p.size() >= 4 && (p.substr(0, 4) == "UNC\\" || p.substr(0, 4) == "UNC/")) {
            // \\?\UNC\server\share -> \\server\share
            std::string rest = "\\\\" + std::string(p.substr(4));
            return DriveOf(rest);
        }
    }
    auto is_sep = [](char c) { return c == '\\' || c == '/'; };
    if (p.size() >= 2 && p[1] == ':' && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'))) {
        std::string d(1, static_cast<char>(p[0] >= 'a' ? p[0] - 'a' + 'A' : p[0]));
        d += ':';
        return d;
    }
    if (p.size() > 2 && is_sep(p[0]) && is_sep(p[1])) {
        // \\server\share[\...]
        size_t i = 2;
        while (i < p.size() && !is_sep(p[i])) ++i;   // server
        if (i == 2 || i >= p.size()) return {};
        size_t j = i + 1;
        while (j < p.size() && !is_sep(p[j])) ++j;   // share
        if (j == i + 1) return {};
        std::string out = "\\\\";
        out.append(p.substr(2, i - 2));
        out += '\\';
        out.append(p.substr(i + 1, j - i - 1));
        return out;
    }
    return {};
}

std::string ErrorMessage(ErrorKind kind, std::string_view arg) {
    switch (kind) {
    case ErrorKind::DiskFull: {
        const std::string drive = DriveOf(arg);
        return FillPlaceholder(str::kErrDiskFull, drive.empty() ? std::string_view(str::kErrUnknownDrive) : drive);
    }
    case ErrorKind::NoEncoder: return str::kErrNoEncoder;
    case ErrorKind::MicBusy: return str::kErrMicBusy;
    case ErrorKind::DeviceLost: return str::kErrDeviceLost;
    case ErrorKind::AccessLost: return str::kErrAccessLost;
    case ErrorKind::MediaMissing:
        return FillPlaceholder(str::kErrMediaMissing, arg.empty() ? std::string_view(str::kErrUnknownPath) : arg);
    case ErrorKind::DiskFullNotStarted:
    case ErrorKind::ExportDiskFull: {
        const std::string drive = DriveOf(arg);
        return FillPlaceholder(kind == ErrorKind::ExportDiskFull ? str::kErrExportDiskFull : str::kErrDiskFullNotStarted,
                               drive.empty() ? std::string_view(str::kErrUnknownDrive) : drive);
    }
    case ErrorKind::OutputFolder:
    case ErrorKind::ExportFolder: {
        std::string_view shown = arg;   // users never see the \\?\ long-path prefix
        const bool unc = shown.size() >= 8 && shown.substr(0, 8) == "\\\\?\\UNC\\";
        if (unc) shown.remove_prefix(8);
        else if (shown.size() >= 4 && shown.substr(0, 4) == "\\\\?\\") shown.remove_prefix(4);
        std::string fixed(shown);
        if (unc) fixed.insert(0, "\\\\");   // \\?\UNC\server\share -> \\server\share
        return FillPlaceholder(kind == ErrorKind::OutputFolder ? str::kErrOutputFolder : str::kErrExportFolder,
                               fixed.empty() ? std::string_view(str::kErrUnknownPath) : std::string_view(fixed));
    }
    case ErrorKind::MicLost: return str::kErrMicLost;
    case ErrorKind::DeviceLostNotSaved: return str::kErrDeviceLostNotSaved;
    case ErrorKind::ExportDeviceLost: return str::kErrExportDeviceLost;
    }
    return {};
}

bool ErrorKindForHr(long hr, ErrorKind* out) {
    ErrorKind k;
    switch (static_cast<unsigned long>(hr)) {
    case 0x80070070ul:   // HRESULT_FROM_WIN32(ERROR_DISK_FULL)
    case 0x80070027ul:   // HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL)
        k = ErrorKind::DiskFull;
        break;
    case static_cast<unsigned long>(DXGI_ERROR_DEVICE_REMOVED):
    case static_cast<unsigned long>(DXGI_ERROR_DEVICE_RESET):
    case static_cast<unsigned long>(DXGI_ERROR_DEVICE_HUNG):
        k = ErrorKind::DeviceLost;
        break;
    case static_cast<unsigned long>(DXGI_ERROR_ACCESS_LOST): k = ErrorKind::AccessLost; break;
    case static_cast<unsigned long>(AUDCLNT_E_DEVICE_IN_USE):
    case static_cast<unsigned long>(AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED):
        k = ErrorKind::MicBusy;
        break;
    case static_cast<unsigned long>(AUDCLNT_E_DEVICE_INVALIDATED): k = ErrorKind::MicLost; break;
    case static_cast<unsigned long>(MF_E_TOPO_CODEC_NOT_FOUND): k = ErrorKind::NoEncoder; break;
    default: return false;
    }
    if (out) *out = k;
    return true;
}

std::string FormatErrorDetails(long hr, std::string_view log_dir, std::string_view extra) {
    std::string out;
    auto line = [&out](std::string_view label, std::string_view value) {
        if (!out.empty()) out += '\n';
        if (!label.empty()) {
            out.append(label);
            out += ": ";
        }
        out.append(value);
    };
    if (hr != 0) line(str::kErrCode, HrToString(hr));
    if (!log_dir.empty()) line(str::kErrLog, log_dir);
    if (!extra.empty()) line({}, extra);
    return out;
}

void OpenFolderAsync(std::string folder) {
    if (folder.empty()) return;
    Async::Run([f = std::move(folder)] { fs::OpenWithShell(f); });
}

// ---- ErrorBanner ------------------------------------------------------------------------------------------------

void ErrorBanner::Show(std::string message, long hr, std::string extra) {
    m_message = std::move(message);
    m_hr = hr;
    m_extra = std::move(extra);
    m_visible = true;
    m_details_open = false;
    m_actions.clear();
    m_clicked = 0;
}

void ErrorBanner::AddAction(int id, std::string label, bool primary) {
    Action a;
    a.id = id;
    a.label = std::move(label);
    a.primary = primary;
    m_actions.push_back(std::move(a));
}

int ErrorBanner::TakeAction() {
    const int id = m_clicked;
    m_clicked = 0;
    return id;
}

void ErrorBanner::Show(ErrorKind kind, std::string_view arg, long hr, std::string extra) {
    Show(ErrorMessage(kind, arg), hr, std::move(extra));
}

void ErrorBanner::Clear() {
    m_visible = false;
    m_details_open = false;
    m_message.clear();
    m_extra.clear();
    m_hr = 0;
    m_actions.clear();
}

std::string ErrorBanner::DetailsText() const {
    const std::string& logs = m_log_dir.empty() ? platform::Paths::LogsDir() : m_log_dir;
    return FormatErrorDetails(m_hr, logs, m_extra);
}

namespace {
constexpr float kPad = 12, kIconGap = 10, kStripe = 3;
// Below this much room for the message text the banner stacks its buttons instead of keeping them on the message row.
constexpr float kMinWrap = 170;
}

bool ErrorBanner::Draw(const char* id) {
    if (!m_visible) return false;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return true;
    ImGui::PushID(id);
    ImDrawList* dl = window->DrawList;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float pad = Dp(kPad);
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);

    // Row: icon, message (wrapped), Details toggle, dismiss.
    const float frame_h = ImGui::GetFrameHeight();
    const char* toggle_label = m_details_open ? str::kErrHideDetails : str::kErrDetails;
    const float toggle_w = ImGui::CalcTextSize(str::kErrHideDetails).x + Dp(28);
    const float buttons_w = toggle_w + Dp(4) + frame_h;
    const float icon_w = Dp(16);
    const float text_x = origin.x + pad + icon_w + Dp(kIconGap);
    // In a narrow panel (the 260 DIP media bin) the message and both buttons on one row leave a column so thin that
    // the text wraps one word per line. Below kMinWrap only the dismiss button stays on the message row and the
    // Details toggle moves under the message.
    const float wide_wrap = (origin.x + width - pad - buttons_w - Dp(12)) - text_x;
    const bool narrow = wide_wrap < Dp(kMinWrap);
    const float wrap_w = narrow ? std::max(Dp(50), (origin.x + width - pad - frame_h - Dp(8)) - text_x)
                                : std::max(Dp(50), wide_wrap);
    const ImVec2 msg_size = ImGui::CalcTextSize(m_message.c_str(), nullptr, false, wrap_w);
    const float row_h = msg_size.y > frame_h ? msg_size.y : frame_h;
    const float top = origin.y + Dp(8);
    {
        FontScope f(FontRole::Icon);
        const float fs = ImGui::GetFontSize();
        dl->AddText(ImVec2(origin.x + pad, std::floor(top + (frame_h - fs) * 0.5f)), col::danger, ICON_ERROR);
    }
    ImGui::SetCursorScreenPos(ImVec2(text_x, top + (msg_size.y < frame_h ? (frame_h - msg_size.y) * 0.5f : 0)));
    ImGui::PushTextWrapPos(text_x + wrap_w - window->Pos.x + window->Scroll.x);
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text));
    ImGui::TextUnformatted(m_message.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    float bottom = top + row_h;
    auto details_toggle = [&] {
        ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(WithAlpha(col::danger, 0.0f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(WithAlpha(col::danger, 0.18f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(WithAlpha(col::danger, 0.28f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(Rgb(0xFF9A9B)));
        char toggle_id[64];
        std::snprintf(toggle_id, sizeof(toggle_id), "%s###details", toggle_label);
        if (ImGui::Button(toggle_id, ImVec2(toggle_w - Dp(8), frame_h))) m_details_open = !m_details_open;
        ImGui::PopStyleColor(4);
    };
    bool dismissed = false;
    if (narrow) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - pad - frame_h, top));
        dismissed = IconButton("##dismiss", ICON_CANCEL, str::kErrDismiss, false, nullptr, ImVec2(frame_h, frame_h));
        ImGui::SetCursorScreenPos(ImVec2(text_x, bottom + Dp(6)));
        details_toggle();
        bottom = ImGui::GetItemRectMax().y;
    } else {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - pad - buttons_w + Dp(8), top));
        details_toggle();
        ImGui::SameLine(0, Dp(4));
        dismissed = IconButton("##dismiss", ICON_CANCEL, str::kErrDismiss, false, nullptr, ImVec2(frame_h, frame_h));
    }
    if (!m_actions.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(text_x, bottom + Dp(8)));
        for (size_t i = 0; i < m_actions.size(); ++i) {
            const Action& a = m_actions[i];
            if (i > 0) ImGui::SameLine(0, Dp(6));
            ImGui::PushID(a.id);
            if (Button(a.label.c_str(), a.primary ? ButtonKind::Primary : ButtonKind::Normal)) m_clicked = a.id;
            ImGui::PopID();
        }
        bottom = ImGui::GetItemRectMax().y;
    }
    if (m_details_open) {
        const std::string details = DetailsText();
        ImGui::SetCursorScreenPos(ImVec2(text_x, bottom + Dp(8)));
        {
            FontScope f(FontRole::Mono);
            ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
            ImGui::PushTextWrapPos(origin.x + width - pad - window->Pos.x + window->Scroll.x);
            ImGui::TextUnformatted(details.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorScreenPos(ImVec2(text_x, ImGui::GetCursorScreenPos().y + Dp(4)));
        if (IconButton("##openlog", ICON_FOLDER_OPEN, nullptr, false, str::kErrOpenLogFolder)) {
            OpenFolderAsync(m_log_dir.empty() ? platform::Paths::LogsDir() : m_log_dir);
        }
        ImGui::SameLine(0, Dp(6));
        if (IconButton("##copy", ICON_COPY, nullptr, false, str::kErrCopyDetails)) {
            const std::string all = m_message + "\n" + details;
            ImGui::SetClipboardText(all.c_str());
        }
        bottom = ImGui::GetItemRectMax().y;
    }
    const ImVec2 max(origin.x + width, bottom + Dp(8));

    dl->ChannelsSetCurrent(0);
    const float r = Dp(metrics::kRadiusCard);
    dl->AddRectFilled(origin, max, Mix(col::bg1, col::danger, 0.14f), r);
    dl->AddRect(origin, max, WithAlpha(col::danger, 0.45f), r, Dp(1));
    dl->AddRectFilled(origin, ImVec2(origin.x + Dp(kStripe), max.y), col::danger, r, ImDrawFlags_RoundLeft);
    dl->ChannelsMerge();

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, max.y - origin.y));
    ImGui::Dummy(ImVec2(0, Dp(4)));
    ImGui::PopID();
    if (dismissed) Clear();
    return m_visible;
}

} // namespace ct::ui

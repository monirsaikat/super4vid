// Media panel shell: cards, badges, drag sources, effects presets.
#include <ct/ui/panels/media_panel.h>

#include <ct/project/edit_ops.h>
#include <ct/ui/editor_commands.h>
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/panels/timeline_widget.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ct::ui {

using namespace ct::project;
using namespace metrics;

namespace {

const char* KindIcon(const Media& m) {
    switch (m.kind) {
    case Media::Kind::Recording: return ICON_MONITOR;
    case Media::Kind::Audio: return ICON_VOLUME;
    case Media::Kind::Image: return ICON_PICTURE;
    default: return ICON_VIDEO;
    }
}

ImU32 KindColor(const Media& m) {
    switch (m.kind) {
    case Media::Kind::Recording: return col::clip_video;
    case Media::Kind::Audio: return col::clip_audio;
    case Media::Kind::Image: return col::clip_annotation;
    default: return col::clip_camera;
    }
}

float Badge(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 fg, ImU32 bg) {
    ImFont* f = Fonts::Get(FontRole::Label);
    const float fs = Dp(10.5f);
    const ImVec2 ts = f ? f->CalcTextSizeA(fs, FLT_MAX, 0, text) : ImVec2(0, 0);
    const ImVec2 a = pos, b(pos.x + ts.x + Dp(10), pos.y + fs + Dp(5));
    dl->AddRectFilled(a, b, bg, Dp(4));
    dl->AddText(f, fs, ImVec2(a.x + Dp(5), a.y + Dp(2)), fg, text);
    return b.x - a.x;
}

} // namespace

void MediaPanel::Draw(EditorState& state) {
    if (ImGui::BeginTabBar("##mediatabs")) {
        if (ImGui::BeginTabItem(str::kTabMedia)) {
            DrawMediaTab(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(str::kTabEffects)) {
            DrawEffectsTab(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    // Remove confirmation for media used by clips.
    if (!m_confirm.empty() && !ImGui::IsPopupOpen("Remove media?")) {
        ImGui::OpenPopup("Remove media?");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Remove media?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const Media* m = state.project.FindMedia(m_confirm);
        ImGui::Text("%s is used by %d clip%s.", m ? cmd::MediaDisplayName(*m) : "This media", m_confirm_clips, m_confirm_clips == 1 ? "" : "s");
        ImGui::TextDisabled("Removing it also removes those clips (Undo brings them back).");
        ImGui::Dummy(Dp(0, 6));
        if (Button(str::kRemoveUsed, ButtonKind::Danger, Dp(200, 30))) {
            const Id id = m_confirm;
            Err err;
            if (state.Edit(str::kUndoRemoveMedia, [&](Project& pp) { return ops::RemoveMedia(pp, id, true, &err); })) state.PruneSelection();
            m_confirm.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (Button("Cancel", ButtonKind::Normal, Dp(100, 30)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_confirm.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (!m_confirm.empty() && !ImGui::IsPopupOpen("Remove media?")) {
        m_confirm.clear();
    }
}

void MediaPanel::DrawMediaTab(EditorState& state) {
    Project& p = state.project;
    ImGui::Dummy(Dp(0, 2));
    ImGui::BeginDisabled(!on_import);
    if (IconButton("import", ICON_ADD, str::kTipImport, false, str::kImport, ImVec2(-FLT_MIN, Dp(kControlDefault))) && on_import) on_import();
    ImGui::EndDisabled();
    ImGui::Dummy(Dp(0, 4));

    if (p.media.empty()) {
        ImGui::Dummy(Dp(0, 24));
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
        ImGui::TextUnformatted(str::kNoMedia);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_faint));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(str::kNoMediaHint);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        return;
    }

    // Usage counts for the "used" indicator and remove prompt.
    ImGui::BeginChild("##cards", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = Dp(10);
    const int cols = std::max(1, static_cast<int>((avail + gap) / (Dp(110) + gap)));
    const float card_w = std::floor((avail - gap * (cols - 1)) / cols);
    const float thumb_h = std::floor(card_w * 9.0f / 16.0f);
    const float card_h = thumb_h + Dp(46);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < p.media.size(); ++i) {
        const Media& m = p.media[i];
        ImGui::PushID(m.id.c_str());
        if (i % static_cast<size_t>(cols) != 0) ImGui::SameLine(0, gap);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##card", ImVec2(card_w, card_h));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 a = pos, b(pos.x + card_w, pos.y + card_h);
        dl->AddRectFilled(a, b, hovered ? col::bg3 : col::bg2, Dp(kRadiusCard));
        // Thumbnail area.
        const ImVec2 ta(a.x + Dp(6), a.y + Dp(6)), tb(b.x - Dp(6), a.y + Dp(6) + thumb_h - Dp(6));
        ImTextureID tex = ImTextureID_Invalid;
        ImVec2 uv0(0, 0), uv1(1, 1);
        const MediaStatus st = m.kind == Media::Kind::Audio ? MediaStatus::Unavailable : m_media->Thumbnail(m.id, m.duration_us / 4, tex, uv0, uv1);
        if (st == MediaStatus::Ready && tex != ImTextureID_Invalid) {
            dl->AddImageRounded(ImTextureRef(tex), ta, tb, uv0, uv1, IM_COL32_WHITE, Dp(kRadiusControl));
        } else {
            const ImU32 kc = KindColor(m);
            const int vs = dl->VtxBuffer.Size;
            dl->AddRectFilled(ta, tb, kc, Dp(kRadiusControl));
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, vs, dl->VtxBuffer.Size, ta, tb, Mix(kc, col::bg0, 0.35f), Mix(kc, col::bg0, 0.75f));
            ImFont* icf = Fonts::Get(FontRole::IconLarge);
            const float is = Dp(22);
            const char* icon = KindIcon(m);
            const ImVec2 isz = icf ? icf->CalcTextSizeA(is, FLT_MAX, 0, icon) : ImVec2(is, is);
            dl->AddText(icf, is, ImVec2(std::floor((ta.x + tb.x - isz.x) * 0.5f), std::floor((ta.y + tb.y - isz.y) * 0.5f)), WithAlpha(col::text, 0.75f), icon);
        }
        // Badges (top-left of the thumbnail).
        // Badges (top-left of the thumbnail; the ones that do not fit are dropped, OFFLINE first in priority).
        float bx = ta.x + Dp(5);
        const float by = ta.y + Dp(5);
        dl->PushClipRect(ta, tb, true);
        struct B { bool on; const char* text; ImU32 bg; };
        const B badges[] = {
            {m.missing, str::kOffline, WithAlpha(col::danger, 0.9f)},
            {m.kind == Media::Kind::Recording, str::kBadgeRec, WithAlpha(col::record, 0.85f)},
            {m.has_camera, str::kBadgeCam, WithAlpha(col::clip_camera, 0.95f)},
            {m.has_events, str::kBadgeEvt, WithAlpha(col::bg0, 0.75f)},
        };
        for (const B& bd : badges) {
            if (!bd.on) continue;
            ImFont* bf = Fonts::Get(FontRole::Label);
            const float bw = (bf ? bf->CalcTextSizeA(Dp(10.5f), FLT_MAX, 0, bd.text).x : 0) + Dp(10);
            if (bx + bw > tb.x - Dp(4)) break;
            bx += Badge(dl, ImVec2(bx, by), bd.text, Rgb(0xFFFFFF), bd.bg) + Dp(3);
        }
        dl->PopClipRect();
        // Duration (bottom-right of the thumbnail).
        if (m.duration_us > 0) {
            char d[24];
            FormatDuration(d, sizeof(d), m.duration_us);
            ImFont* f = Fonts::Get(FontRole::Label);
            const float fs = Dp(11);
            const ImVec2 ts = f ? f->CalcTextSizeA(fs, FLT_MAX, 0, d) : ImVec2(0, 0);
            const ImVec2 da(tb.x - ts.x - Dp(11), tb.y - fs - Dp(9));
            dl->AddRectFilled(da, ImVec2(tb.x - Dp(5), tb.y - Dp(5)), WithAlpha(col::bg0, 0.75f), Dp(4));
            dl->AddText(f, fs, ImVec2(da.x + Dp(3), da.y + Dp(2)), col::text, d);
        }
        // Name + meta line.
        const float ny = tb.y + Dp(7);
        ImGui::RenderTextEllipsis(dl, ImVec2(a.x + Dp(8), ny), ImVec2(b.x - Dp(8), ny + ImGui::GetFontSize()), b.x - Dp(8),
                                  cmd::MediaDisplayName(m), nullptr, nullptr);
        char meta[48];
        if (m.kind == Media::Kind::Audio) std::snprintf(meta, sizeof(meta), "Audio");
        else std::snprintf(meta, sizeof(meta), "%d\xC3\x97%d%s", m.width, m.height, m.fps_num > 0 && m.fps_den == 1 ? "" : "");
        if (m.fps_num > 0 && m.fps_den > 0 && m.kind != Media::Kind::Image) {
            const size_t len = std::strlen(meta);
            std::snprintf(meta + len, sizeof(meta) - len, " \xC2\xB7 %d fps", m.fps_num / m.fps_den);
        }
        dl->AddText(Fonts::Get(FontRole::Label), Dp(12), ImVec2(a.x + Dp(8), ny + ImGui::GetFontSize() + Dp(2)), col::text_dim, meta);

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("CT_MEDIA", m.id.c_str(), m.id.size() + 1);
            ImGui::TextUnformatted(cmd::MediaDisplayName(m));
            ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
            ImGui::TextUnformatted(str::kDragHint);
            ImGui::PopStyleColor();
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            m_ctx = m.id;
            ImGui::OpenPopup("##mediactx");
        }
        if (ImGui::BeginPopup("##mediactx")) {
            if (ImGui::MenuItem(str::kAddToTimeline)) {
                const Media* mm = p.FindMedia(m_ctx);
                Clip clip;
                bool ok = false;
                for (const Track& t : p.tracks)
                    if (mm && !ok && MakeClipForTrack(p, *mm, t, state.Playhead(), &clip)) ok = true;
                // No compatible track (an empty project, or every track busy at the playhead): make one, exactly as
                // dropping the media on the timeline's empty area does.
                bool new_track = false;
                if (!ok && mm) {
                    Track tmp;
                    tmp.kind = mm->kind == Media::Kind::Audio ? Track::Kind::Audio : Track::Kind::Video;
                    ok = new_track = MakeClipForTrack(p, *mm, tmp, state.Playhead(), &clip);
                }
                Err err;
                Id added;
                if (ok && state.EditCopy(str::kUndoAddClip, [&](Project& pp) {
                        Clip c = clip;
                        if (new_track) {
                            Id tid;
                            const bool audio = mm->kind == Media::Kind::Audio;
                            if (!ops::AddTrack(pp, audio ? Track::Kind::Audio : Track::Kind::Video, audio ? "Audio" : "Video", &tid, -1))
                                return false;
                            c.track = tid;
                        }
                        if (!ops::AddClip(pp, std::move(c), &added, &err)) return false;
                        if (const Clip* v = pp.FindClip(added)) cmd::AddLinkedAudioClip(pp, *v);   // sound on an audio track
                        return true;
                    }))
                    state.selection.Set(added);
                else if (!ok || !err.Ok()) state.last_error = err.message.empty() ? "No compatible track with room at the playhead" : err.message;
            }
            if (ImGui::MenuItem(str::kReveal, nullptr, false, static_cast<bool>(on_reveal))) on_reveal(m_ctx);
            if (ImGui::MenuItem(str::kRelink, nullptr, false, static_cast<bool>(on_relink))) on_relink(m_ctx);
            ImGui::Separator();
            int used = 0;
            for (const Clip& c : p.clips) used += c.media == m_ctx ? 1 : 0;
            if (ImGui::MenuItem(used ? str::kRemoveUsed : str::kRemove)) {
                const Id id = m_ctx;
                if (used) {
                    m_confirm = id;   // used by clips: confirm first (P09 9.2)
                    m_confirm_clips = used;
                } else {
                    Err err;
                    if (state.Edit(str::kUndoRemoveMedia, [&](Project& pp) { return ops::RemoveMedia(pp, id, true, &err); })) state.PruneSelection();
                }
            }
            ImGui::EndPopup();
        }
        if (hovered) ItemTooltip(m.path.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void MediaPanel::DrawEffectsTab(EditorState&) {
    struct Preset { const char* name; const char* icon; int type; };
    static const Preset kPresets[] = {
        {"Text", ICON_FONT, 0}, {"Callout", ICON_INFO, 1}, {"Arrow", ICON_NEXT, 2}, {"Rectangle", ICON_FULLSCREEN, 3},
        {"Ellipse", ICON_VIEW, 4}, {"Highlight", ICON_EDIT, 5}, {"Blur", ICON_HIDE, 6}, {"Keystrokes", ICON_FONT, 7},
    };
    ImGui::Dummy(Dp(0, 4));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = Dp(8);
    const float w = std::floor((avail - gap) / 2);
    const float h = Dp(64);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 8; ++i) {
        const Preset& pr = kPresets[i];
        ImGui::PushID(i);
        if (i % 2) ImGui::SameLine(0, gap);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##fx", ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(pos, pos + ImVec2(w, h), hov ? col::bg3 : col::bg2, Dp(kRadiusCard));
        ImFont* icf = Fonts::Get(FontRole::IconLarge);
        dl->AddText(icf, Dp(18), pos + Dp(12, 12), col::clip_annotation_text, pr.icon);
        dl->AddText(pos + ImVec2(Dp(12), h - Dp(24)), col::text, pr.name);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("CT_ANNOTATION", &pr.type, sizeof(int));
            ImGui::TextUnformatted(pr.name);
            ImGui::EndDragDropSource();
        }
        ItemTooltip(str::kDragHint);
        ImGui::PopID();
    }
}

} // namespace ct::ui

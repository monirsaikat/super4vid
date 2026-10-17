#pragma once
// src/ui/src/editor_player.h (private to ct_ui)
// The editor's project player (render::TimelinePlayer, P10b) and the PlaybackControl the panels drive.
// Thread model: UI thread.

#include <ct/render/timeline_player.h>
#include <ct/ui/editor_state.h>

#include <algorithm>

namespace ct::ui::detail {

using EditorPlayer = render::TimelinePlayer;

// PlaybackControl over TimelinePlayer: the playhead follows Tick() while playing (which pauses at the end / at 0 in
// reverse); the loop range (I/O) wraps.
class PlayerTransport final : public PlaybackControl {
public:
    PlayerTransport(EditorState& s, EditorPlayer& p) : m_s(s), m_p(p) {}

    bool Playing() const override { return m_p.Playing(); }
    double Rate() const override { return Playing() ? m_p.Rate() : 0.0; }
    void Play(double rate) override {
        const TimeUs end = m_s.project.Duration();
        TimeUs t = m_s.Playhead();
        if (rate > 0 && t >= end) t = 0;   // play at the end restarts
        if (rate < 0 && t <= 0) t = end;
        const TimeUs now = m_p.Now();
        if (now - t > m_s.FrameUs() || t - now > m_s.FrameUs()) m_p.Seek(t);
        m_p.SetRate(rate);
        if (!Playing()) m_p.Play();
        m_s.SetPlayhead(t);
    }
    void Pause() override {
        if (Playing()) m_p.Pause();
    }
    void Seek(TimeUs t) override {
        t = std::clamp<TimeUs>(t, 0, std::max<TimeUs>(0, m_s.project.Duration()));
        m_p.Seek(t);
        m_s.SetPlayhead(t);
    }
    void HintScrub(TimeUs t) override { m_p.HintScrub(t); }
    bool Tick(double) override {
        if (!Playing()) {
            m_p.Tick();   // picks up audio edits while paused
            return false;
        }
        TimeUs t = m_p.Tick();
        const auto& loop = m_s.project.ui.loop;
        if (loop && loop->second > loop->first && m_p.Rate() > 0 && t >= loop->second) {
            t = loop->first;
            m_p.Seek(t);
        }
        m_s.SetPlayhead(t);
        return Playing();
    }
    float Volume() const override { return m_p.Audio().Volume(); }
    void SetVolume(float v) override { m_p.Audio().SetVolume(v); }
    bool Muted() const override { return m_p.Audio().Muted(); }
    void SetMuted(bool m) override { m_p.Audio().SetMuted(m); }

private:
    EditorState& m_s;
    EditorPlayer& m_p;
};

} // namespace ct::ui::detail

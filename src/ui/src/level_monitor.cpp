// Live input level for the settings meters test and the bar's idle meters (P11 11.6).
#include <ct/ui/level_monitor.h>

#include <ct/audio/devices.h>
#include <ct/audio/meter.h>
#include <ct/audio/wasapi_capture.h>
#include <ct/audio/wasapi_loopback.h>
#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/time.h>

#include <algorithm>
#include <atomic>

namespace ct::ui {

namespace {
struct NullSink final : audio::AudioSink {
    bool OnAudio(uint32_t, const audio::AudioPacket&) override { return true; }
};
}

struct LevelMonitor::State {
    std::shared_ptr<audio::AudioDevices> devices;
    std::unique_ptr<audio::WasapiCapture> mic;
    std::unique_ptr<audio::WasapiLoopback> sys;
    NullSink sink;
    std::atomic<const audio::Meter*> meter{nullptr};   // set once the stream runs (IO pool), read by the UI
    std::atomic<int> status{static_cast<int>(Status::Starting)};
    std::atomic<bool> stop{false};
    std::atomic<HRESULT> hr{S_OK};
    ~State() {
        if (mic) mic->Stop();
        if (sys) sys->Stop();
    }
};

LevelMonitor::LevelMonitor() = default;
LevelMonitor::~LevelMonitor() { Stop(); }

void LevelMonitor::Start(Source source, std::wstring device_id, std::shared_ptr<audio::AudioDevices> devices) {
    Stop();
    m_source = source;
    m_shown = audio::Meter::kFloorDb;
    auto st = std::make_shared<State>();
    st->devices = std::move(devices);
    m_state = st;
    Async::Run([st, source, id = std::move(device_id)]() {
        if (st->stop.load()) return;
        HRESULT hr = S_OK;
        if (!st->devices) {
            st->devices = std::make_shared<audio::AudioDevices>();
            hr = st->devices->Init();
        }
        audio::CaptureConfig cc;
        cc.device_id = id;
        if (SUCCEEDED(hr) && source == Source::Microphone) {
            auto c = std::make_unique<audio::WasapiCapture>();
            hr = c->Init(cc, *st->devices);
            if (SUCCEEDED(hr) && !c->Start(Clock::QpcNow(), st->sink)) hr = FAILED(c->LastError()) ? c->LastError() : E_FAIL;
            if (SUCCEEDED(hr)) {
                st->mic = std::move(c);
                st->meter.store(&st->mic->Levels(), std::memory_order_release);
            }
        } else if (SUCCEEDED(hr)) {
            auto c = std::make_unique<audio::WasapiLoopback>();
            hr = c->Init(cc, *st->devices);
            if (SUCCEEDED(hr) && !c->Start(Clock::QpcNow(), st->sink)) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                st->sys = std::move(c);
                st->meter.store(&st->sys->Levels(), std::memory_order_release);
            }
        }
        st->hr.store(hr);
        st->status.store(static_cast<int>(SUCCEEDED(hr) ? Status::Running : Status::Failed), std::memory_order_release);
        if (FAILED(hr)) CT_LOG_WARN("level monitor: %s could not be opened: %s", source == Source::Microphone ? "microphone" : "system audio",
                                    HrToString(hr).c_str());
    });
}

void LevelMonitor::Stop() {
    if (!m_state) return;
    std::shared_ptr<State> st = std::move(m_state);
    st->stop.store(true);
    st->meter.store(nullptr, std::memory_order_release);
    // The open job holds its own reference; the last owner closes the stream (Stop joins the capture thread), on the
    // IO pool, never on the UI thread.
    Async::Run([st]() mutable { st.reset(); });
    m_shown = audio::Meter::kFloorDb;
}

LevelMonitor::Status LevelMonitor::GetStatus() const {
    if (!m_state) return Status::Stopped;
    return static_cast<Status>(m_state->status.load(std::memory_order_acquire));
}

HRESULT LevelMonitor::LastError() const { return m_state ? m_state->hr.load() : S_OK; }

float LevelMonitor::PeakDb() const {
    if (!m_state) return audio::Meter::kFloorDb;
    const audio::Meter* m = m_state->meter.load(std::memory_order_acquire);
    if (!m) return audio::Meter::kFloorDb;
    float p = audio::Meter::kFloorDb;
    const int n = std::max(1, m->Channels());
    for (int c = 0; c < n && c < audio::Meter::kMaxChannels; ++c) p = std::max(p, m->PeakDb(c));
    return p;
}

float LevelMonitor::Falloff(float shown_db, float new_db, float dt_s) {
    const float dt = std::clamp(dt_s, 0.0f, 0.5f);
    return std::max(new_db, std::max(audio::Meter::kFloorDb, shown_db - 20.0f * dt));
}

float LevelMonitor::Shown(float dt_s) {
    m_shown = Falloff(m_shown, PeakDb(), dt_s);
    return m_shown;
}

} // namespace ct::ui

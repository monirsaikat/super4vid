#include <ct/audio/wasapi_loopback.h>

#include "capture_engine.h"

namespace ct::audio {

WasapiLoopback::WasapiLoopback()
    : m_engine(std::make_unique<detail::CaptureEngine>(detail::EngineMode::Loopback)) {}
WasapiLoopback::~WasapiLoopback() = default;

HRESULT WasapiLoopback::Init(const CaptureConfig& config, AudioDevices& devices) {
    return m_engine->Init(config, devices);
}
bool WasapiLoopback::Start(int64_t epoch_qpc, AudioSink& sink) { return m_engine->Start(epoch_qpc, sink); }
void WasapiLoopback::SetPaused(bool paused, int64_t at_qpc) { m_engine->SetPaused(paused, at_qpc); }
void WasapiLoopback::Stop() { m_engine->Stop(); }
void WasapiLoopback::SwitchDevice(const std::wstring& device_id) { m_engine->SwitchDevice(device_id); }
const Meter& WasapiLoopback::Levels() const { return m_engine->Levels(); }
std::string WasapiLoopback::DeviceName() const { return m_engine->DeviceName(); }
HRESULT WasapiLoopback::LastError() const { return m_engine->LastError(); }
CaptureStats WasapiLoopback::GetStats() const { return m_engine->Stats(); }

} // namespace ct::audio

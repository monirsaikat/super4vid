#include <ct/audio/wasapi_capture.h>

#include "capture_engine.h"

namespace ct::audio {

WasapiCapture::WasapiCapture() : m_engine(std::make_unique<detail::CaptureEngine>(detail::EngineMode::Mic)) {}
WasapiCapture::~WasapiCapture() = default;

HRESULT WasapiCapture::Init(const CaptureConfig& config, AudioDevices& devices) { return m_engine->Init(config, devices); }
bool WasapiCapture::Start(int64_t epoch_qpc, AudioSink& sink) { return m_engine->Start(epoch_qpc, sink); }
void WasapiCapture::SetPaused(bool paused, int64_t at_qpc) { m_engine->SetPaused(paused, at_qpc); }
void WasapiCapture::Stop() { m_engine->Stop(); }
const Meter& WasapiCapture::Levels() const { return m_engine->Levels(); }
std::string WasapiCapture::DeviceName() const { return m_engine->DeviceName(); }
HRESULT WasapiCapture::LastError() const { return m_engine->LastError(); }
CaptureStats WasapiCapture::GetStats() const { return m_engine->Stats(); }

} // namespace ct::audio

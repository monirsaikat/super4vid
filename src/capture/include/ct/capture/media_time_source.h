#pragma once
// ct/capture/media_time_source.h
// The pause-adjusted session time base used by capture sources (webcam). recorder::SessionClock implements it;
// QpcMediaTimeSource is the plain epoch-based version for tools and tests (defined in webcam_source.cpp).
// Thread model: implementations must be callable from any thread (the capture threads read it per frame).

#include <ct/core/types.h>

#include <cstdint>

namespace ct::capture {

struct MediaTimeSource {
    virtual TimeUs QpcHnsToMediaUs(uint64_t qpc_hns) const = 0;   // QPC in 100 ns units (device timestamps)
    virtual TimeUs NowMediaUs() const = 0;
    virtual ~MediaTimeSource() = default;
};

// Media time = QPC time since epoch_qpc, no pauses.
class QpcMediaTimeSource final : public MediaTimeSource {
public:
    explicit QpcMediaTimeSource(int64_t epoch_qpc) : m_epoch_qpc(epoch_qpc) {}
    TimeUs QpcHnsToMediaUs(uint64_t qpc_hns) const override;
    TimeUs NowMediaUs() const override;
    int64_t EpochQpc() const { return m_epoch_qpc; }

private:
    int64_t m_epoch_qpc;
};

} // namespace ct::capture

#include <ct/recorder/typing.h>

namespace ct::recorder {

int TypingDetector::OnKeyDown(TimeUs t_us, Edge out[2]) {
    int n = 0;
    if (m_open && t_us - m_last >= m_gap) {
        out[n++] = {m_last, false};
        m_open = false;
    }
    if (!m_open) {
        out[n++] = {t_us, true};
        m_open = true;
    }
    if (t_us > m_last || n > 0) m_last = t_us;
    return n;
}

int TypingDetector::Tick(TimeUs now_us, Edge out[1]) {
    if (!m_open || now_us - m_last < m_gap) return 0;
    out[0] = {m_last, false};
    m_open = false;
    return 1;
}

int TypingDetector::Finish(Edge out[1]) {
    if (!m_open) return 0;
    out[0] = {m_last, false};
    m_open = false;
    return 1;
}

} // namespace ct::recorder

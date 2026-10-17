#pragma once
// Synthetic `.ctrec` bundles for the importer tests: meta.json (the docs/04 sample, adjusted) and an events.ctev written
// with ct::events::EventWriter. screen.mp4 / cam.mp4 are not created: the importer reads only meta.json and events.

#include <ct/core/fs.h>
#include <ct/core/json.h>
#include <ct/core/strings.h>
#include <ct/events/ctev.h>

#include <string>
#include <vector>

namespace ct::project::testutil {

// The meta.json sample of docs/04 section 1 (185 s, 1920x1080@60, camera, mic stream 1, system stream 2, events).
inline Json DocsMetaSample() {
    std::string md;
    if (!fs::ReadAll("../docs/04-file-formats.md", md)) return Json();
    for (char& c : md)
        if (c == '\r') c = ' ';
    const size_t start = md.find("```json\n{\n  \"format\": \"ctrec\"");
    if (start == std::string::npos) return Json();
    const size_t body = start + 8;
    const size_t end = md.find("\n```", body);
    Json j;
    Json::Parse(md.substr(body, end - body), j);
    return j;
}

struct Click { TimeUs t_us; int x, y; };

// Cursor samples every 8333 us moving towards each click, plus a down/up pair per click.
inline std::string MakeEventsFile(int region_w, int region_h, const std::vector<Click>& clicks, TimeUs duration_us) {
    events::EventHeader h;
    h.qpc_frequency = 10'000'000;
    h.region_w = region_w;
    h.region_h = region_h;
    events::EventWriter w;
    w.OpenMemory(h);
    std::vector<uint8_t> arrow(16 * 16 * 4, 255);
    const uint32_t shape = w.CursorShape(0, 0, 16, 16, 0, 0, arrow.data());
    int x = region_w / 2, y = region_h / 2;
    size_t next = 0;
    for (TimeUs t = 0; t <= duration_us; t += 8333) {
        if (next < clicks.size()) {
            // Move linearly to the next click position over the last 500 ms before it.
            const Click& c = clicks[next];
            if (t >= c.t_us - 500'000 && t < c.t_us) {
                const double u = double(t - (c.t_us - 500'000)) / 500'000.0;
                w.CursorMove(t, int(x + (c.x - x) * u), int(y + (c.y - y) * u), shape, true);
            }
            if (t >= c.t_us) {
                x = c.x;
                y = c.y;
                w.CursorMove(c.t_us, x, y, shape, true);
                w.MouseButton(c.t_us, events::Button::Left, true, x, y);
                w.MouseButton(c.t_us + 90'000, events::Button::Left, false, x, y);
                ++next;
            }
        }
    }
    return w.Bytes();
}

// Writes <dir>\<name>.ctrec\{meta.json, events.ctev}; returns the bundle path.
inline std::string MakeBundle(const std::string& dir, const char* name, const Json& meta, const std::string& events_bytes) {
    const std::string bundle = dir + "\\" + name + ".ctrec";
    fs::CreateDirectories(bundle);
    fs::WriteAtomic(bundle + "\\meta.json", meta.Dump(2));
    if (!events_bytes.empty()) fs::WriteAtomic(bundle + "\\events.ctev", events_bytes);
    return bundle;
}

} // namespace ct::project::testutil

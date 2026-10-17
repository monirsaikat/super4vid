#pragma once
// ct/project/importer.h
// Imports a `.ctrec` recording bundle (docs/04 section 1) into a project: one Recording media, the standard tracks
// (Screen, Camera, Mic, System, Callouts), clips at the end of the timeline, the canvas of a new project, and the
// generated Auto-Zoom camera keys. Only meta.json and events.ctev are read; screen.mp4 / cam.mp4 are referenced by
// path and opened later by playback (so a bundle without them imports fine and shows as offline media).
// Thread model: blocking file IO; call from a ct::Async job and apply the result on the UI thread.

#include <ct/core/error.h>
#include <ct/core/types.h>
#include <ct/project/project.h>

#include <string>
#include <string_view>
#include <vector>

namespace ct::project {

struct ImportOptions {
    bool apply_default_style = true;   // new projects get the polished canvas (padding, radius, gradient, shadow)
    bool auto_zoom = true;             // generate camera keys from the events
};

// The parts of meta.json the editor uses.
struct RecordingMeta {
    int version = 0;
    TimeUs duration_us = 0;
    std::string screen_file = "screen.mp4";
    int width = 0, height = 0, fps_num = 0, fps_den = 1;
    RectI region;
    bool has_camera = false;
    std::string camera_file;
    int camera_width = 0, camera_height = 0;
    int mic_stream = -1, system_stream = -1;   // MP4 stream index, -1 = not recorded
    std::vector<std::string> audio_streams;    // "mic" / "system" in stream order
    bool has_events = false;                   // meta lists an events file and it exists
    std::string events_file;                   // absolute path when has_events
    struct Pause { TimeUs at_us, duration_us; };
    std::vector<Pause> pauses;
};

bool ReadRecordingMeta(std::string_view bundle_path, RecordingMeta* out, Err* err);

// On failure the project is unchanged.
bool ImportRecording(Project& p, std::string_view bundle_path, Err* err, ImportOptions options = {});

} // namespace ct::project

#pragma once
// ct/recorder/headless.h
// Console modes of Camtesia.exe that record without UI (docs/03 section 2), built on RecordingSession:
//   --record [--monitor N] [--fps F] [--out DIR] [--seconds S] [--no-mic] [--no-system] [--region x,y,w,h]
//            [--codec h264|hevc] [--quality Q] [--keys off|shortcuts|all] [--camera] [--synthetic-camera]
//            [--pause-at S --pause-for S]
//     records into DIR\<stamp>.ctrec (DIR defaults to the recordings folder) until S seconds of media time or Ctrl+C,
//     prints the bundle path and a summary, and verifies the bundle with --verify.
//   --selftest   3 s of monitor 0 (+ system audio, mic if present, a synthetic camera) into %TEMP%, verifies the
//                bundle, deletes it on success; exit 0 = pass, 1 = fail.
// Thread model: RunHeadless blocks the calling thread; the work runs on an MTA thread. VerifyBundle needs COM (MTA).

#include <ct/core/types.h>

#include <string>
#include <string_view>

namespace ct::recorder {

// True when the command line contains --record or --selftest (these modes skip the single-instance check).
bool IsHeadlessCommandLine(const wchar_t* cmdline);
// Runs the mode; returns the process exit code. COM and logging are set up by the caller.
int RunHeadless(const wchar_t* cmdline);

struct BundleExpect {
    double seconds = 0;          // > 0: expected media duration (+- tolerance_s)
    double tolerance_s = 0.25;
    bool require_cursor = true;  // events.ctev has at least one CURSOR_SHAPE record
};
struct BundleReport {
    bool ok = false;
    std::string text;            // one line per check, "ok"/"FAIL"
    double duration_s = 0;
    int fps = 0;
    uint64_t video_frames = 0;
    int audio_streams = 0;
    uint64_t cam_frames = 0;
    size_t cursor_shapes = 0, cursor_samples = 0;
};
// Checks meta.json (docs/04 section 1), probes screen.mp4 (1 video stream, the audio streams meta lists, frame count
// = fps * duration +- 3 %), cam.mp4 when listed, and loads events.ctev with events::EventLog.
BundleReport VerifyBundle(std::string_view bundle_path, const BundleExpect& expect);

} // namespace ct::recorder

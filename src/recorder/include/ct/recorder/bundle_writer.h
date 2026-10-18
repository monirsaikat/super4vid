#pragma once
// ct/recorder/bundle_writer.h
// The .ctrec recording bundle (docs/04 section 1): a folder "<dir>\<yyyy-MM-dd HH-mm-ss>.ctrec\" holding screen.mp4,
// optional cam.mp4, events.ctev and meta.json; plus the meta.json builder (pure, unit tested).
// Thread model: a BundleWriter belongs to one thread (the session thread); its functions do blocking file IO.
// BuildMetaJson is a pure function. Lifetime: plain value; Delete() removes the folder and everything in it.

#include <ct/core/json.h>
#include <ct/core/types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ct::recorder {

class BundleWriter {
public:
    // Creates "<output_dir>\<LocalFileStamp>.ctrec" (and output_dir itself when missing). A name that exists already
    // gets " (2)", " (3)", ... before the extension. bundle_path_out: the folder, without a trailing separator.
    bool Create(std::string_view output_dir, std::string& bundle_path_out);
    // Same with an explicit stem (tests; "<stem>.ctrec", collision suffixes as above).
    bool CreateNamed(std::string_view output_dir, std::string_view stem, std::string& bundle_path_out);

    const std::string& Path() const { return m_path; }
    std::string ScreenPath() const;
    std::string CamPath() const;
    std::string EventsPath() const;
    std::string MetaPath() const;
    bool WriteMeta(const Json& meta);   // pretty-printed (2 spaces), atomic replace
    bool Delete();                      // the whole folder
    // An armed session's bundle (RecordingSession::Arm) is hidden until the recording starts, so an idle bar shows no
    // half-made recording in the folder.
    bool SetHidden(bool hidden);
    // Renames the folder to "<stem>.ctrec" next to it (collision suffixes as in Create); every file inside must be
    // closed. Used when the bundle was created long before its recording started (armed), so the name tells when the
    // recording was made. false (old name kept) when the rename fails.
    bool Rename(std::string_view stem);
    // The "<stem>" part of the current folder name.
    std::string Stem() const;
    // Deletes armed bundles a killed or crashed process left behind in output_dir: hidden "*.ctrec" folders with no
    // meta.json and no events.ctev (a started recording has both or is visible). Returns how many were removed.
    static int DeleteStaleArmed(std::string_view output_dir);

private:
    std::string m_path;
};

// Everything meta.json records (docs/04 section 1). Optional parts are null in the JSON when their flag is false.
struct BundleMeta {
    std::string app_version;
    std::string created_utc;               // "2026-10-17T10:03:44Z"
    int64_t epoch_qpc = 0, qpc_frequency = 0;
    TimeUs duration_us = 0;
    // screen
    std::string screen_file = "screen.mp4";
    int width = 0, height = 0, fps_num = 60, fps_den = 1;
    std::string codec = "h264";
    RectI region;
    std::string monitor_device, monitor_name;
    int monitor_origin_x = 0, monitor_origin_y = 0, monitor_dpi = 96;
    bool monitor_hdr = false;
    double scale = 1.0;
    // camera
    bool has_camera = false;
    std::string camera_file = "cam.mp4", camera_device;
    int camera_width = 0, camera_height = 0, camera_fps_num = 30, camera_fps_den = 1;
    // audio
    struct Audio {
        bool present = false;
        int stream_index = 0;
        std::string device;
        int sample_rate = 48000, channels = 2;
    } mic, system_audio;
    // events
    std::string events_file = "events.ctev";
    int cursor_hz = 120;
    std::string keystrokes = "shortcuts";   // "off" | "shortcuts" | "all"
    // system
    std::string os, gpu, encoder;           // encoder: "AMDh264Encoder (hardware)"
    struct Pause { TimeUs at_us = 0, duration_us = 0; };
    std::vector<Pause> pauses;
    bool incomplete = false;                // EmergencyFinalize: "incomplete": true (key omitted otherwise)
};

Json BuildMetaJson(const BundleMeta& meta);
// "10.0.26100" from RtlGetVersion.
std::string OsVersionString();

} // namespace ct::recorder

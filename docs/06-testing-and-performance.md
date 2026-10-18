# 06 · Testing and performance measurement

## 1. Test harness (`src/test/include/ct/test/ct_test.h`, target `ct_test`, created in P00)

A ~250-line header-only harness plus one `.cpp`. No third-party test framework.

```cpp
#include <ct/test/ct_test.h>

CT_TEST(json_roundtrip_unicode) {            // unit test: always runs
    ct::Json j; CT_REQUIRE(ct::Json::Parse("{\"a\":\"\\u00e9\"}", j));
    CT_CHECK_EQ(j["a"].AsString(), "é");
}
CT_TEST_DEVICE(dup_acquires_frame) {         // needs GPU/audio/webcam: SKIPPED when CT_CI is set or hardware is missing
    CT_CHECK_HR(dup.Init(dev, cfg));
}
CT_TEST_GOLDEN(compositor_rounded_shadow) {  // compares against tests/data/golden/<name>.png; --update-golden rewrites
    CT_GOLDEN_IMAGE(readback_rgba, width, height);
}
CT_BENCH(spsc_queue_push_pop, 10'000'000) {  // runs only with --bench; prints ops/s and ns/op
    for (int i = 0; i < CT_BENCH_N; ++i) { q.Push(i); int v; q.Pop(v); }
}
int main(int argc, char** argv) { return ct::test::RunAll(argc, argv); }
```

Macros: `CT_CHECK(expr)`, `CT_CHECK_EQ(a,b)`, `CT_CHECK_NEAR(a,b,eps)`, `CT_CHECK_HR(hr)`, `CT_REQUIRE(expr)` (aborts the test),
`CT_SKIP("reason")`, `CT_FAIL("msg")`. Runner flags: `--filter <substr>`, `--list`, `--bench`, `--repeat N`, `--update-golden`,
`--verbose`. Output: `[PASS] name (1.2 ms)`, `[SKIP] name: reason`, `[FAIL] file(line): expr  left=.. right=..`. Exit code = number of failures.
Golden comparison: pass when max absolute channel difference <= 2 **and** PSNR >= 45 dB (goldens that contain text: PSNR >= 35 dB, because glyph rasterization varies slightly between driver versions); on failure write `<name>.actual.png` and `<name>.diff.png` next to the golden.

Timing budgets inside tests (timer lateness, logger cost, frame times) are enforced only when the environment variable
`CT_STRICT_TIMING=1` is set, which is how acceptance runs on an idle machine are done. Without it the tests print the
numbers, warn above budget and fail only on gross regressions, so parallel builds and loaded CI machines stay green.
Correctness checks (ordering, no early wake-ups, frame counts) are always enforced.

Test executables, one per module (`tests/<module>/CMakeLists.txt` via `ct_add_test`): `ct_core_tests`, `ct_platform_tests`,
`ct_capture_tests`, `ct_audio_tests`, `ct_media_tests`, `ct_recorder_tests`, `ct_project_tests`, `ct_playback_tests`,
`ct_render_tests`, `ct_export_tests`, `ct_ui_tests`. All run under `ctest`; device tests self-skip without hardware.

## 2. Test data (`tests/data/`, total < 20 MB, listed in `tests/data/README.md`)

| File | Source | Used by |
|---|---|---|
| `pattern_1080p60_5s.mp4` | generated once by `tools/gen_test_media` (P04): moving color bars + frame counter burned in + 1 kHz tone with a beep every second | decoder, playback, export, A/V sync |
| `pattern_4k60_3s.mp4` | same generator | playback budget |
| `tone_48k_stereo.wav`, `tone_44k1_mono.wav`, `speech.mp3`, `music.m4a` (CC0 clips, < 1 MB each) | audio decode/resample |
| `cursor_shapes.ctev`, `sample.ctrec/` (3 s recording made by the recorder in P05 on the reference machine) | project import, cursor renderer |
| `golden/*.png` | written by `--update-golden` on the reference machine, reviewed by eye once | compositor, GIF, export |
| `projects/*.ctproj` | hand-written edge cases: empty, 1,000 clips, missing media, v0 migration | project model |

The generator draws every frame with a frame number as a 7-segment-style digit pattern in a known rectangle so tests can
decode a frame and assert its index without OCR (`tests/common/frame_index.h` reads the pixels).

## 3. Test categories and what each phase must add

| Category | Where | Runs in CI | Purpose |
|---|---|---|---|
| Unit | `ct_*_tests` | yes | pure logic: strings, json, time math, model ops, auto-zoom, GIF LZW, resampler SNR |
| Device | `ct_*_tests` (`CT_TEST_DEVICE`) | skipped | DXGI dup, WASAPI, MF encoder/decoder, D2D text |
| Golden | `ct_render_tests`, `ct_export_tests` | skipped (needs GPU) but **required** locally | compositor pixel output |
| Bench | any (`CT_BENCH`) | no | queues, resampler, GIF quantizer, timeline culling |
| Tool runs | `capture_bench`, `audio_bench`, `player_demo`, `Camtesia.exe --selftest` | selftest skipped | end-to-end numbers for `PROGRESS.md` |
| Fuzz-lite | `ct_core_tests`, `ct_project_tests`, `ct_media_tests` | yes | random JSON, random `.ctev` bytes, random edit sequences, 10,000 iterations, must not crash and must keep invariants |

## 4. A/V sync "clap test" (P04, P05, P11)

1. `tools/avsync_source.exe` (built in P04): a window that flashes white for 1 frame and plays a 2 kHz 20 ms beep through the default output every 2.000 s, driven by a waitable timer; logs the QPC of each flash.
2. Record it for 60 s (or 60 min for the budget) with system audio enabled.
3. `player_demo --avsync recording.ctrec` decodes video, finds frames whose mean luminance in the window region exceeds a threshold, detects beep onsets in the system track by energy in a 2 kHz band-pass, pairs them, prints offset per event and the drift slope.
4. Pass: |offset| < 20 ms for every event and |drift| < 20 ms over the whole recording.

## 5. Measuring the budgets (write the numbers in `PROGRESS.md`)

| Metric | How |
|---|---|
| Cold start | Reboot, run `Camtesia.exe --time-to-window` which prints (window shown QPC - process creation time from `GetProcessTimes`) and exits. Run 3 times, report the median. Warm = second run without reboot. |
| Idle CPU / redraws | `Camtesia.exe --stats` with the bar visible for 60 s: `redraws/s` must be 0 while the mouse is still; Task Manager CPU column shows 0 %. |
| Recording CPU/GPU/RSS | `Camtesia.exe --record --monitor 0 --fps 60 --seconds 600 --stats` while a 1080p YouTube video plays in a browser. CPU = delta of `GetProcessTimes` kernel+user divided by wall time and core count; GPU = Task Manager "GPU engine" column of the process averaged by eye over 60 s (or `typeperf "\GPU Engine(*)\Utilization Percentage"`); RSS = `PROCESS_MEMORY_COUNTERS_EX.WorkingSetSize` peak printed by `--stats`. |
| Dropped frames | `--stats` prints `captured`, `repeated`, `dropped_queue_full`, `encoder_rejected`. Budget: `dropped_* == 0`. Also verify with `player_demo --count-frames` that the file has `fps * seconds ± 1` frames. |
| Hotkey latency | `--stats` logs the QPC of `WM_HOTKEY` and of the first `Acquire` success; print the difference. |
| Editor fps | F11 stats overlay while dragging a clip in `projects/1000_clips.ctproj`; shows CPU frame time and GPU time; budget 16.6 ms total, < 1 ms timeline draw. |
| Scrub latency | overlay shows `seek->frame ms` for the last scrub; test with `pattern_1080p60_5s.mp4` random seeks (`player_demo --seek-bench`). |
| Export speed | export dialog prints `x realtime` at completion; log it. |
| Binary sizes | `dir out\build\ninja-release\bin\Camtesia.exe`, `out\dist\*.exe`, installed folder size in Settings > Apps. |
| Editor benches (P09 9.7) | `Camtesia.exe --editor tests\data\projects\1000_clips.ctproj --bench-drag 600 > drag.txt`; `--editor out\testmedia\preview_bench.ctproj --bench-scrub 60` / `--bench-play 600`; `--editor --stats` (redraws/s, mouse still). `preview_bench.ctproj` is written by `ct_ui_tests` (`editor_fixtures_synthetic_bundle_and_preview_project`, run from `tests\`). Redirect stdout to a file: the app is a GUI exe. |

### Running several instances side by side (dev/test only)

Camtesia is single-instance per session: the first launch holds the named mutex `Local\Camtesia.Instance` and a
second launch forwards its command line to the window of class `CamtesiaMain`, then exits. Parallel worktrees,
benches and agents on one machine would block each other, so the environment variable **`CT_INSTANCE_SUFFIX`**
gives a launch its own instance: when set, `.<suffix>` is appended to the mutex name and to the main window class
(`Local\Camtesia.Instance.wt1`, `CamtesiaMain.wt1`), so only launches with the same suffix forward to each other.
Allowed characters are letters, digits, `-`, `_` and `.` (others are dropped, at most 32 are kept; nothing left =
no suffix). Processes started by the app (the bar's "Open in editor") inherit it. Unset, which is every normal
user's case, the names are exactly the release names. There are no named pipes or other named objects to rename.

```
set CT_INSTANCE_SUFFIX=wt-a256
out\build\ninja-release\bin\Camtesia.exe --editor tests\data\projects\1000_clips.ctproj --bench-drag 600 > drag.txt
```

Diagnostics never forward anyway: `--screenshot`, `--time-to-window`, `--bench-*`, `--fault-test`, `--bar-keys`,
`--record`, `--selftest` and
editors started from the bar (`CAMTESIA_SEPARATE_INSTANCE=1`) skip the single-instance check. `--stats` does not,
so use the suffix for idle measurements next to another running instance.

### Error paths and keyboard (P11 11.5/11.6)

Each recorder error path is forced without the real condition through `recorder::SessionFaults`
(`SessionConfig::faults`) and each export error path through `exportng::ExportFaults` (`ExportRequest::faults`):
an injected writer HRESULT after n samples (`ERROR_DISK_FULL`, `DXGI_ERROR_DEVICE_REMOVED`), a capture error after
n ms (TDR), a failing hardware encoder (software fallback) or no encoder at all, a microphone that cannot be opened
(`AUDCLNT_E_DEVICE_IN_USE`), an audio stream error after n ms (`AUDCLNT_E_DEVICE_INVALIDATED`), and a fake free-disk
value (the 1 GB warning / 200 MB auto-stop). Unit tests: `ct_recorder_tests --filter fault_`,
`ct_export_tests --filter export_fault`, `ct_ui_tests` (views, banner, export dialog). End to end through the real
recorder bar (error panel text, recovery actions, device recreation, a clean second recording; exit 0 = PASS):

```
set CT_INSTANCE_SUFFIX=wt-a256
for %k in (disk-full low-disk device-lost no-encoder sw-encoder mic-busy mic-lost bad-folder) do (
  out\build\ninja-debug\bin\Camtesia.exe --fault-test %k > fault-%k.txt )
Camtesia.exe --fault-test disk-full --no-exclude --fault-shot panel.png      (screenshot of the error panel)
Camtesia.exe --bar-keys "tab,tab,tab,tab,enter,esc,tab" > keys.txt           (prints the focused control per key)
```

A real TDR (`dxcap -forcetdr`, admin) and a real full volume (a small VHD) remain manual checks (PROGRESS 11.5).

### Idle wake-ups without admin rights, leak and crash checks

- `idle_probe [--seconds 30] [--warmup 10] -- Camtesia.exe --tray` (or `--pid N`): per-thread wake-ups (context
  switches) and CPU time over the window, then process totals and memory; ends the app with WM_QUIT so Debug exit
  checks still run. Use it when WPR is not available (WPR needs admin). Budget: no thread of ours above 0 wakes/s while
  idle; OS thread-pool workers (ntdll) wake a few times a minute.
- `ctest -R camtesia_` runs `camtesia_exit_leak_check` (8 app modes; Debug: no CRT leak report, no debug-layer line on
  stderr) and `camtesia_crash_dump` (`--crash-test` dump whose stack `dump_check` resolves to main.cpp).
- `dump_check crash.dmp [--symbols dir]` prints the exception and the faulting thread's stack (WinDbg `.ecxr; kn`).

## 6. Profiling workflow (P11, and any time a budget fails)

1. **ETW first.** `wpr -start GeneralProfile -start Power` (admin) -> reproduce for 30 s -> `wpr -stop trace.etl`. Open in WPA: CPU Usage (Sampled) by process/thread/stack, and the `Camtesia` TraceLogging provider events (our `CT_TRACE_SCOPE` spans show up under Generic Events). Our provider GUID is in `src/core/src/trace.cpp`; it must be enabled: `wpr -start camtesia.wprp` with a profile that lists the provider name `Camtesia`.
2. **GPU.** `ID3D11Query` timestamp pairs around each compositor pass (already in `RenderContext` behind `stats.enabled`); F11 shows them. For deeper work use PIX for Windows GPU captures (`winget install Microsoft.PIX`).
3. **Media Foundation.** `mftrace -o trace.txt Camtesia.exe --record ...` when the encoder or sink rejects something.
4. **Memory.** `--stats` prints pool occupancy and cache bytes every second; Debug build prints CRT leak report and `ReportLiveDeviceObjects` at exit (must be empty).
5. Commit the fix with before/after numbers in the message.

## 7. Release test matrix (P12)

| Environment | Checks |
|---|---|
| Windows 11 24H2, reference machine, 1080p + 4K mixed DPI | full recording budget run, editor budget run, export, installer |
| Windows 10 22H2 VM (no GPU acceleration, WARP) | app starts, software encoder path records 720p30, editor opens a project, export completes, no crash |
| Windows 11 laptop with NVIDIA or Intel GPU (any) | HW encoder detected, HEVC availability reported correctly, loopback audio records |
| Clean user profile | first run creates folders, settings defaults, no prompts, no missing fonts |
| Uninstall | removes program files, shortcuts, association; keeps `%APPDATA%`/`%LOCALAPPDATA%`/Videos data |

The automatable part (per-user install into a temp folder, headless record + verify with the installed exe, import + MP4 export, association open, uninstall while a tray instance runs, leftover scan) is `tools\release_smoke.ps1`; the full per-release list is `tools/release_checklist.md`.

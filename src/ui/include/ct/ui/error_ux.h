#pragma once
// ct/ui/error_ux.h
// Error UX (P11 11.5, docs/05 section 4 "Dialogs and feedback"): the plain-language messages of strings.h with their
// drive/path filled in, and ErrorBanner, the non-modal red banner drawn at the top of the panel concerned with a
// Details disclosure (HRESULT text via ct::HrToString, log folder, "Open log folder", "Copy details").
// Thread model: ErrorMessage/DriveOf/FormatErrorDetails are pure and thread-safe. ErrorBanner is UI-thread only
// (Show from another thread must be posted to the UI thread first). OpenFolderAsync runs the shell call on the IO
// pool.

#include <string>
#include <string_view>
#include <vector>

namespace ct::ui {

enum class ErrorKind {
    DiskFull,       // arg: a path on the full volume (or the drive itself), shown as "D:"
    NoEncoder,
    MicBusy,
    DeviceLost,
    AccessLost,
    MediaMissing,   // arg: the missing file's path
    // Additions (P11 11.5 wiring).
    DiskFullNotStarted,   // arg: a path on the full volume
    OutputFolder,         // arg: the folder that cannot be written
    MicLost,
    DeviceLostNotSaved,
    ExportDiskFull,       // arg: a path on the full volume
    ExportFolder,         // arg: the export path or its folder
    ExportDeviceLost,
};

// The strings.h message for `kind` with "%s" replaced: the disk-full kinds get DriveOf(arg) (str::kErrUnknownDrive
// when that is empty), MediaMissing/OutputFolder/ExportFolder get arg (str::kErrUnknownPath when empty). Kinds without
// "%s" ignore arg.
std::string ErrorMessage(ErrorKind kind, std::string_view arg = {});
// The kind an HRESULT stands for in a recording / export context, when it has one: disk full (ERROR_DISK_FULL,
// ERROR_HANDLE_DISK_FULL), device lost (DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG), access lost (DXGI_ERROR_ACCESS_LOST),
// microphone busy (AUDCLNT_E_DEVICE_IN_USE, exclusive mode), microphone gone (AUDCLNT_E_DEVICE_INVALIDATED), no
// encoder (MF_E_TOPO_CODEC_NOT_FOUND). false otherwise.
bool ErrorKindForHr(long hr, ErrorKind* out);
// Replaces the first "%s" in `fmt` with `arg` (no printf: messages may contain '%').
std::string FillPlaceholder(std::string_view fmt, std::string_view arg);
// Volume of a path for messages: "D:\x\y.mp4" -> "D:", "d:" -> "D:", "\\server\share\x" -> "\\server\share",
// "\\?\D:\x" -> "D:". Relative or empty paths -> "".
std::string DriveOf(std::string_view path);
// Details text: "Error code: <HrToString(hr)>" (omitted when hr == 0), "Log folder: <log_dir>" (omitted when
// empty), then `extra` (omitted when empty), one per line.
std::string FormatErrorDetails(long hr, std::string_view log_dir, std::string_view extra);

// Opens a folder (or file) in Explorer on the IO pool (fs::OpenWithShell); never blocks the UI thread.
void OpenFolderAsync(std::string folder_utf8);

class ErrorBanner {
public:
    // Replaces the current banner. hr == 0 hides the error code line; `extra` adds a free-form details line.
    void Show(std::string message, long hr = 0, std::string extra = {});
    void Show(ErrorKind kind, std::string_view arg = {}, long hr = 0, std::string extra = {});
    void Clear();
    // Recovery buttons drawn under the message, in order (Show clears them). `id` > 0 is the caller's.
    void AddAction(int id, std::string label, bool primary = false);
    size_t ActionCount() const { return m_actions.size(); }
    int ActionIdAt(size_t i) const { return i < m_actions.size() ? m_actions[i].id : 0; }
    // The id of the action button clicked during the last Draw (0 = none); reading it resets it. The banner stays
    // visible: the caller decides (Clear) whether the action resolved the problem.
    int TakeAction();
    // Tests: as if the button were clicked.
    void ClickActionForTest(int id) { m_clicked = id; }
    bool Visible() const { return m_visible; }
    const std::string& Message() const { return m_message; }
    long Hr() const { return m_hr; }
    // Defaults to platform::Paths::LogsDir() when empty (tests set their own).
    void SetLogDir(std::string dir) { m_log_dir = std::move(dir); }
    std::string DetailsText() const;

    // Draws the banner at the cursor across the available width (the top of a panel) and advances the cursor.
    // Nothing is drawn while hidden. `id` must be unique in the window. Returns true while visible.
    bool Draw(const char* id);

private:
    struct Action {
        int id = 0;
        std::string label;
        bool primary = false;
    };
    std::string m_message, m_extra, m_log_dir;
    std::vector<Action> m_actions;
    int m_clicked = 0;
    long m_hr = 0;
    bool m_visible = false;
    bool m_details_open = false;
};

} // namespace ct::ui

#pragma once
// ct/ui/dialogs.h
// Shell file dialogs for the editor (IFileOpenDialog / IFileSaveDialog, folder picker with FOS_PICKFOLDERS), Explorer
// reveal and a few path helpers. Dialogs are modal to `owner` and block the calling thread until closed.
// Thread model: the UI thread (COM STA). Never call while an ImGui popup holds mouse capture: the editor defers dialog
// requests to the start of the next frame and releases the capture first (P09 pitfalls).

#include <string>
#include <string_view>
#include <vector>

namespace ct::ui::dialogs {

enum class Filter { Media, Project, ProjectOrRecording, Images, All };

// Multi-select when `multi`. Empty result = cancelled. `start_dir` may be empty.
std::vector<std::string> OpenFiles(void* owner, Filter filter, bool multi, const char* title, std::string_view start_dir = {});
// One folder (FOS_PICKFOLDERS), e.g. a .ctrec bundle. Empty = cancelled.
std::string PickFolder(void* owner, const char* title, std::string_view start_dir = {});
// Save dialog for a .ctproj (default extension added). Empty = cancelled.
std::string SaveProject(void* owner, std::string_view suggested_name, std::string_view start_dir = {});
// Opens Explorer with the file selected (or the folder itself when it is a directory).
void RevealInExplorer(std::string_view path);
// ShellExecute "open" of a folder or file.
void OpenWithShell(std::string_view path);

// Path helpers (UTF-8, '\\' separators).
std::string Extension(std::string_view path);   // lowercase, with the dot; "" when none
bool IsDirectory(std::string_view path);
bool Exists(std::string_view path);
std::string ParentDir(std::string_view path);
std::string FileStem(std::string_view path);    // name without directory and extension
// Classification of a path given on the command line, dropped, or picked: project, recording bundle, media, or other.
enum class PathKind { Project, Recording, Media, Unknown };
PathKind Classify(std::string_view path);

} // namespace ct::ui::dialogs

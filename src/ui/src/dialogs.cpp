// Shell file dialogs and Explorer helpers (UI thread, STA).
#define CT_LOG_CATEGORY "ui"
#include <ct/ui/dialogs.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>

using Microsoft::WRL::ComPtr;

namespace ct::ui::dialogs {

namespace {

const COMDLG_FILTERSPEC kMedia[] = {
    {L"Media files", L"*.mp4;*.mov;*.m4v;*.m4a;*.mp3;*.wav;*.aac;*.png;*.jpg;*.jpeg;*.bmp;*.webp"},
    {L"Video", L"*.mp4;*.mov;*.m4v"},
    {L"Audio", L"*.m4a;*.mp3;*.wav;*.aac"},
    {L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.webp"},
    {L"All files", L"*.*"},
};
const COMDLG_FILTERSPEC kProject[] = {{L"Camtesia projects", L"*.ctproj"}, {L"All files", L"*.*"}};
const COMDLG_FILTERSPEC kProjectOrRec[] = {{L"Projects and recordings", L"*.ctproj;meta.json"}, {L"All files", L"*.*"}};
const COMDLG_FILTERSPEC kImages[] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.webp"}, {L"All files", L"*.*"}};
const COMDLG_FILTERSPEC kAll[] = {{L"All files", L"*.*"}};

void SetFilter(IFileDialog* d, Filter f) {
    switch (f) {
    case Filter::Media: d->SetFileTypes(ARRAYSIZE(kMedia), kMedia); break;
    case Filter::Project: d->SetFileTypes(ARRAYSIZE(kProject), kProject); break;
    case Filter::ProjectOrRecording: d->SetFileTypes(ARRAYSIZE(kProjectOrRec), kProjectOrRec); break;
    case Filter::Images: d->SetFileTypes(ARRAYSIZE(kImages), kImages); break;
    case Filter::All: d->SetFileTypes(ARRAYSIZE(kAll), kAll); break;
    }
    d->SetFileTypeIndex(1);
}

void SetFolder(IFileDialog* d, std::string_view dir) {
    if (dir.empty()) return;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(Utf8ToWide(dir).c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()))))
        d->SetFolder(item.Get());
}

std::string ItemPath(IShellItem* item) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = WideToUtf8(p);
        CoTaskMemFree(p);
    }
    return out;
}

} // namespace

std::vector<std::string> OpenFiles(void* owner, Filter filter, bool multi, const char* title, std::string_view start_dir) {
    std::vector<std::string> out;
    ComPtr<IFileOpenDialog> d;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(d.GetAddressOf()));
    if (FAILED(hr)) {
        CT_LOG_ERROR("IFileOpenDialog: %s", HrToString(hr).c_str());
        return out;
    }
    DWORD opts = 0;
    d->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    if (multi) opts |= FOS_ALLOWMULTISELECT;
    d->SetOptions(opts);
    SetFilter(d.Get(), filter);
    if (title) d->SetTitle(Utf8ToWide(title).c_str());
    SetFolder(d.Get(), start_dir);
    hr = d->Show(static_cast<HWND>(owner));
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return out;
    if (FAILED(hr)) {
        CT_LOG_ERROR("IFileOpenDialog::Show: %s", HrToString(hr).c_str());
        return out;
    }
    ComPtr<IShellItemArray> items;
    if (FAILED(d->GetResults(items.GetAddressOf()))) return out;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> it;
        if (SUCCEEDED(items->GetItemAt(i, it.GetAddressOf()))) {
            std::string p = ItemPath(it.Get());
            if (!p.empty()) out.push_back(std::move(p));
        }
    }
    return out;
}

std::string PickFolder(void* owner, const char* title, std::string_view start_dir) {
    ComPtr<IFileOpenDialog> d;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(d.GetAddressOf()));
    if (FAILED(hr)) {
        CT_LOG_ERROR("IFileOpenDialog (folder): %s", HrToString(hr).c_str());
        return {};
    }
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (title) d->SetTitle(Utf8ToWide(title).c_str());
    SetFolder(d.Get(), start_dir);
    hr = d->Show(static_cast<HWND>(owner));
    if (FAILED(hr)) {
        if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) CT_LOG_ERROR("folder picker: %s", HrToString(hr).c_str());
        return {};
    }
    ComPtr<IShellItem> it;
    if (FAILED(d->GetResult(it.GetAddressOf()))) return {};
    return ItemPath(it.Get());
}

std::string SaveProject(void* owner, std::string_view suggested, std::string_view start_dir) {
    ComPtr<IFileSaveDialog> d;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(d.GetAddressOf()));
    if (FAILED(hr)) {
        CT_LOG_ERROR("IFileSaveDialog: %s", HrToString(hr).c_str());
        return {};
    }
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    d->SetFileTypes(ARRAYSIZE(kProject), kProject);
    d->SetFileTypeIndex(1);
    d->SetDefaultExtension(L"ctproj");
    d->SetTitle(L"Save project");
    if (!suggested.empty()) d->SetFileName(Utf8ToWide(suggested).c_str());
    SetFolder(d.Get(), start_dir);
    hr = d->Show(static_cast<HWND>(owner));
    if (FAILED(hr)) {
        if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) CT_LOG_ERROR("IFileSaveDialog::Show: %s", HrToString(hr).c_str());
        return {};
    }
    ComPtr<IShellItem> it;
    if (FAILED(d->GetResult(it.GetAddressOf()))) return {};
    return ItemPath(it.Get());
}

void RevealInExplorer(std::string_view path) {
    const std::wstring w = Utf8ToWide(path);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w.c_str());
    if (pidl) {
        const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr)) return;
    }
    const std::wstring args = L"/select,\"" + w + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void OpenWithShell(std::string_view path) {
    ShellExecuteW(nullptr, L"open", Utf8ToWide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string Extension(std::string_view path) {
    const size_t slash = path.find_last_of("\\/");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    std::string e(path.substr(dot));
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool IsDirectory(std::string_view path) {
    const DWORD a = GetFileAttributesW(Utf8ToWide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(std::string_view path) { return GetFileAttributesW(Utf8ToWide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string ParentDir(std::string_view path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string FileStem(std::string_view path) {
    const size_t slash = path.find_last_of("\\/");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    while (!name.empty() && (name.back() == '\\' || name.back() == '/')) name.remove_suffix(1);
    const size_t dot = name.find_last_of('.');
    return std::string(dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot));
}

PathKind Classify(std::string_view path) {
    std::string p(path);
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    const std::string ext = Extension(p);
    if (IsDirectory(p)) return (ext == ".ctrec" || Exists(p + "\\meta.json")) ? PathKind::Recording : PathKind::Unknown;
    if (ext == ".ctproj") return PathKind::Project;
    if (ext == ".json" && FileStem(p) == "meta" && Extension(ParentDir(p)) == ".ctrec") return PathKind::Recording;
    static const char* const kMediaExt[] = {".mp4", ".mov", ".m4v", ".m4a", ".mp3", ".wav", ".aac", ".png", ".jpg", ".jpeg", ".bmp", ".webp"};
    for (const char* e : kMediaExt)
        if (ext == e) return PathKind::Media;
    return PathKind::Unknown;
}

} // namespace ct::ui::dialogs

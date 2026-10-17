#pragma once
// ct/ui/icons.h
// Icon glyphs (UTF-8) from "Segoe Fluent Icons" / "Segoe MDL2 Assets", merged into the UI fonts by Fonts::Load.
// Codepoints from docs/05 section 1 (identical in both fonts). Use them like text: ImGui::Button(ICON_PLAY).
// Any new codepoint must be verified in Character Map in both fonts before use.

namespace ct::ui {

constexpr const char* ICON_PLAY = "\uE768";
constexpr const char* ICON_PAUSE = "\uE769";
constexpr const char* ICON_STOP = "\uE71A";
constexpr const char* ICON_RECORD = "\uE7C8";
constexpr const char* ICON_SETTINGS = "\uE713";
constexpr const char* ICON_MICROPHONE = "\uE720";
constexpr const char* ICON_CAMERA = "\uE722";
constexpr const char* ICON_VOLUME = "\uE767";
constexpr const char* ICON_MUTE = "\uE74F";
constexpr const char* ICON_CUT = "\uE8C6";
constexpr const char* ICON_UNDO = "\uE7A7";
constexpr const char* ICON_REDO = "\uE7A6";
constexpr const char* ICON_SAVE = "\uE74E";
constexpr const char* ICON_FOLDER_OPEN = "\uE838";
constexpr const char* ICON_OPEN_FILE = "\uE8E5";
constexpr const char* ICON_FULLSCREEN = "\uE740";
constexpr const char* ICON_CLOSE = "\uE8BB";
constexpr const char* ICON_MINIMIZE = "\uE921";
constexpr const char* ICON_MAXIMIZE = "\uE922";
constexpr const char* ICON_CHEVRON_DOWN = "\uE70D";
constexpr const char* ICON_CHEVRON_RIGHT = "\uE76C";
constexpr const char* ICON_ADD = "\uE710";
constexpr const char* ICON_DELETE = "\uE74D";
constexpr const char* ICON_MONITOR = "\uE7F4";
constexpr const char* ICON_ZOOM_IN = "\uE8A3";
constexpr const char* ICON_ZOOM_OUT = "\uE71F";
constexpr const char* ICON_PREVIOUS = "\uE892";
constexpr const char* ICON_NEXT = "\uE893";
constexpr const char* ICON_FLAG = "\uE7C1";
constexpr const char* ICON_MORE = "\uE712";
constexpr const char* ICON_VIDEO = "\uE714";
constexpr const char* ICON_PICTURE = "\uE8B9";
constexpr const char* ICON_FONT = "\uE8D2";
constexpr const char* ICON_EDIT = "\uE70F";
constexpr const char* ICON_COPY = "\uE8C8";
constexpr const char* ICON_PASTE = "\uE77F";
constexpr const char* ICON_INFO = "\uE946";
constexpr const char* ICON_WARNING = "\uE7BA";
constexpr const char* ICON_ERROR = "\uEA39";
constexpr const char* ICON_CHECK = "\uE73E";
constexpr const char* ICON_CANCEL = "\uE711";
constexpr const char* ICON_SEARCH = "\uE721";
constexpr const char* ICON_LOCK = "\uE72E";
constexpr const char* ICON_UNLOCK = "\uE785";
constexpr const char* ICON_HIDE = "\uED1A";
constexpr const char* ICON_VIEW = "\uE890";
constexpr const char* ICON_EXPORT = "\uEDE1";
// Additions beyond the docs/05 list; present in both fonts (checked by ct_ui_tests icons_present_in_icon_font).
constexpr const char* ICON_CHEVRON_LEFT = "\uE76B";
constexpr const char* ICON_REPEAT = "\uE8EE";

// Every icon above, for tests and the font coverage check.
constexpr const char* kAllIcons[] = {
    ICON_PLAY, ICON_PAUSE, ICON_STOP, ICON_RECORD, ICON_SETTINGS, ICON_MICROPHONE, ICON_CAMERA, ICON_VOLUME,
    ICON_MUTE, ICON_CUT, ICON_UNDO, ICON_REDO, ICON_SAVE, ICON_FOLDER_OPEN, ICON_OPEN_FILE, ICON_FULLSCREEN,
    ICON_CLOSE, ICON_MINIMIZE, ICON_MAXIMIZE, ICON_CHEVRON_DOWN, ICON_CHEVRON_RIGHT, ICON_ADD, ICON_DELETE,
    ICON_MONITOR, ICON_ZOOM_IN, ICON_ZOOM_OUT, ICON_PREVIOUS, ICON_NEXT, ICON_FLAG, ICON_MORE, ICON_VIDEO,
    ICON_PICTURE, ICON_FONT, ICON_EDIT, ICON_COPY, ICON_PASTE, ICON_INFO, ICON_WARNING, ICON_ERROR, ICON_CHECK,
    ICON_CANCEL, ICON_SEARCH, ICON_LOCK, ICON_UNLOCK, ICON_HIDE, ICON_VIEW, ICON_EXPORT, ICON_CHEVRON_LEFT, ICON_REPEAT,
};

} // namespace ct::ui

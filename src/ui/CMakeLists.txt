# P09: the ui module (theme, fonts, widgets, shortcuts, editor state, timeline, panels, editor window/app). Target
# name stays ct_ui; ct_imconfig.h stays in include/ct/ui (the imgui target points IMGUI_USER_CONFIG at it).
ct_add_module(ui
  SOURCES
    src/theme.cpp src/fonts.cpp src/widgets.cpp src/shortcuts.cpp
    src/editor_state.cpp src/editor_commands.cpp src/timeline_math.cpp
    src/editor_shell.cpp src/editor_banners.cpp src/editor_window.cpp src/editor_app.cpp
    src/dialogs.cpp src/cache_media_provider.cpp
    src/panels/timeline_widget.cpp src/panels/properties_panel.cpp src/panels/media_panel.cpp src/panels/status_bar.cpp
    src/panels/preview_panel.cpp
  DEPS ct_core ct_project ct_platform ct_render ct_playback ct_media ct_audio ct_events imgui)
# The editor app uses the ImGui Win32/DX11 backends (compiled into the imgui target) and the shell dialogs.
target_link_libraries(ct_ui PRIVATE shell32 ole32 comdlg32)

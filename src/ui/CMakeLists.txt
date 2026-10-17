# P09: the ui module (theme, fonts, widgets, shortcuts, editor state, timeline, panels). Target name stays ct_ui;
# ct_imconfig.h stays in include/ct/ui (the imgui target points IMGUI_USER_CONFIG at it).
ct_add_module(ui
  SOURCES
    src/theme.cpp src/fonts.cpp src/widgets.cpp src/shortcuts.cpp
    src/editor_state.cpp src/editor_commands.cpp src/timeline_math.cpp
    src/editor_shell.cpp
    src/panels/timeline_widget.cpp src/panels/properties_panel.cpp src/panels/media_panel.cpp src/panels/status_bar.cpp
    src/error_ux.cpp
  DEPS ct_core ct_project ct_platform imgui)

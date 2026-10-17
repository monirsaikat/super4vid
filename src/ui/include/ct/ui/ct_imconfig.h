#pragma once
// ct/ui/ct_imconfig.h
// Dear ImGui user config (IMGUI_USER_CONFIG). ImGui sources are never edited; everything is configured here.
#include <ct/core/assert.h>
#define IM_ASSERT(_EXPR) CT_ASSERT(_EXPR)
#define IMGUI_USE_WCHAR32
#define ImDrawIdx unsigned int
// IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS is intentionally NOT defined: imgui.cpp tests it with defined(),
// so even "#define ... 0" would disable the default IME handler (see DECISIONS.md, P00 task 0.4).

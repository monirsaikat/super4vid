#pragma once
// Headless ImGui context for ct_ui_tests: no window, no renderer. Textures requested by the atlas are marked as
// created so frames can be rendered (ImGuiBackendFlags_RendererHasTextures), and the mouse is driven with
// synthetic IO events.

#include <ct/ui/fonts.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <functional>

namespace ct::ui::testing {

class Headless {
public:
    explicit Headless(float w = 1600, float h = 400) {
        m_ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(w, h);
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigDebugHighlightIdConflicts = true;
        io.Fonts->AddFontDefault();
        m_size = ImVec2(w, h);
    }
    ~Headless() {
        Fonts::Unload();
        ImGui::DestroyContext(m_ctx);
    }
    Headless(const Headless&) = delete;
    Headless& operator=(const Headless&) = delete;

    // One frame: fn runs inside a borderless full-size window "##host" at (0,0) with no padding.
    void Frame(const std::function<void()>& fn, bool host_window = true) {
        ImGui::NewFrame();
        if (host_window) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(m_size);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleVar();
            fn();
            ImGui::End();
        } else {
            fn();
        }
        ImGui::Render();
        for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
            if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
                t->SetTexID(static_cast<ImTextureID>(1));
                t->SetStatus(ImTextureStatus_OK);
            } else if (t->Status == ImTextureStatus_WantDestroy) {
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }
    void Mouse(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
    void Button(int b, bool down) { ImGui::GetIO().AddMouseButtonEvent(b, down); }
    void Key(ImGuiKey k, bool down) { ImGui::GetIO().AddKeyEvent(k, down); }
    bool IdConflict() const { return m_ctx->DebugDrawIdConflictsId != 0; }

private:
    ImGuiContext* m_ctx;
    ImVec2 m_size;
};

} // namespace ct::ui::testing

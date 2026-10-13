// WalkerCore — SettingsMenu implementation (M11).

#include "walkercore/SettingsMenu.h"

#include <cstring>

#include "walkercore/Log.h"

#if defined(WALKERCORE_HAVE_IMGUI)
#include "imgui.h"
#endif

namespace walkercore {

SettingsMenu::SettingsMenu() = default;
SettingsMenu::~SettingsMenu() = default;

void SettingsMenu::Bind(SettingsStore *store) {
    store_ = store;
}

void SettingsMenu::Open() {
    is_open_ = true;
    dirty_ = false;
    WALKERCORE_LOG_INFO("SettingsMenu: opened");
}

void SettingsMenu::Close() {
    is_open_ = false;
    if (dirty_ && store_ != nullptr) {
        store_->SaveToDefaultPath();
        WALKERCORE_LOG_INFO("SettingsMenu: closed (saved)");
    } else {
        WALKERCORE_LOG_INFO("SettingsMenu: closed");
    }
}

void SettingsMenu::Toggle() {
    if (is_open_) {
        Close();
    } else {
        Open();
    }
}

void SettingsMenu::SetSliderValue(const char *key, float v) {
    if (store_ == nullptr || key == nullptr) return;
    WalkerSettings s = store_->current();
    bool changed = false;

    // Movement
    if (std::strcmp(key, "walk_speed_mps") == 0) {
        s.movement.walkSpeedMps = v; changed = true;
    } else if (std::strcmp(key, "run_speed_mps") == 0) {
        s.movement.runSpeedMps = v; changed = true;
    } else if (std::strcmp(key, "crouch_speed_mps") == 0) {
        s.movement.crouchSpeedMps = v; changed = true;
    } else if (std::strcmp(key, "jump_initial_velocity_mps") == 0) {
        s.movement.jumpInitialVelocityMps = v; changed = true;
    } else if (std::strcmp(key, "gravity_mps2") == 0) {
        s.movement.gravityMps2 = v; changed = true;
    } else if (std::strcmp(key, "max_step_up_m") == 0) {
        s.movement.maxStepUpM = v; changed = true;
    } else if (std::strcmp(key, "max_slope_angle_rad") == 0) {
        s.movement.maxSlopeAngleRad = v; changed = true;
    } else if (std::strcmp(key, "mouse_sensitivity") == 0) {
        s.movement.mouseSensitivityRadPerUnit = v; changed = true;
    } else if (std::strcmp(key, "horizontal_fov_radians") == 0) {
        s.horizontal_fov_radians = v; changed = true;
    } else if (std::strcmp(key, "head_bob_vertical_amp_m") == 0) {
        s.movement.headBobVerticalAmpM = v; changed = true;
    } else if (std::strcmp(key, "head_bob_lateral_amp_m") == 0) {
        s.movement.headBobLateralAmpM = v; changed = true;
    } else if (std::strcmp(key, "head_bob_frequency_hz") == 0) {
        s.movement.headBobFrequencyHz = v; changed = true;
    } else if (std::strcmp(key, "eye_height_standing_m") == 0) {
        s.movement.eyeHeightStandingM = v; changed = true;
    } else if (std::strcmp(key, "eye_height_crouching_m") == 0) {
        s.movement.eyeHeightCrouchingM = v; changed = true;
    } else if (std::strcmp(key, "transition_seconds") == 0) {
        s.transition_seconds = v; changed = true;
    } else if (std::strcmp(key, "near_z") == 0) {
        s.near_z = v; changed = true;
    } else if (std::strcmp(key, "far_z") == 0) {
        s.far_z = v; changed = true;
    }

    if (changed) {
        store_->SetCurrent(s);
        store_->ApplyAll();
        dirty_ = true;
    }
}

void SettingsMenu::SetBoolValue(const char *key, bool v) {
    if (store_ == nullptr || key == nullptr) return;
    WalkerSettings s = store_->current();
    bool changed = false;
    if (std::strcmp(key, "use_mouse") == 0) {
        s.head_tracker.use_mouse = v; changed = true;
    } else if (std::strcmp(key, "use_trackir") == 0) {
        s.head_tracker.use_trackir = v; changed = true;
    } else if (std::strcmp(key, "use_opentrack") == 0) {
        s.head_tracker.use_opentrack = v; changed = true;
    }
    if (changed) {
        store_->SetCurrent(s);
        store_->ApplyAll();
        dirty_ = true;
    }
}

void SettingsMenu::SetKeyBinding(const char *key, int vk) {
    if (store_ == nullptr || key == nullptr) return;
    WalkerSettings s = store_->current();
    bool changed = false;
    if (std::strcmp(key, "forward_vk") == 0) { s.keys.forward_vk = vk; changed = true; }
    else if (std::strcmp(key, "backward_vk") == 0) { s.keys.backward_vk = vk; changed = true; }
    else if (std::strcmp(key, "strafe_left_vk") == 0) { s.keys.strafe_left_vk = vk; changed = true; }
    else if (std::strcmp(key, "strafe_right_vk") == 0) { s.keys.strafe_right_vk = vk; changed = true; }
    else if (std::strcmp(key, "run_vk") == 0) { s.keys.run_vk = vk; changed = true; }
    else if (std::strcmp(key, "crouch_vk") == 0) { s.keys.crouch_vk = vk; changed = true; }
    else if (std::strcmp(key, "jump_vk") == 0) { s.keys.jump_vk = vk; changed = true; }
    else if (std::strcmp(key, "interact_vk") == 0) { s.keys.interact_vk = vk; changed = true; }
    else if (std::strcmp(key, "toggle_walk_vk") == 0) { s.keys.toggle_walk_vk = vk; changed = true; }
    else if (std::strcmp(key, "flashlight_vk") == 0) { s.keys.flashlight_vk = vk; changed = true; }
    else if (std::strcmp(key, "eye_up_vk") == 0) { s.keys.eye_up_vk = vk; changed = true; }
    else if (std::strcmp(key, "eye_down_vk") == 0) { s.keys.eye_down_vk = vk; changed = true; }
    else if (std::strcmp(key, "eye_reset_vk") == 0) { s.keys.eye_reset_vk = vk; changed = true; }

    if (changed) {
        store_->SetCurrent(s);
        store_->ApplyAll();
        dirty_ = true;
    }
}

void SettingsMenu::Render() {
    // M11 lander: real ImGui widgets for the most common settings.
    // The menu opens via Ctrl+F10 (rising edge from the input
    // hook) and closes via the same binding; the Open()/Close()
    // path already handles dirty-tracking and SaveToDefaultPath()
    // on close, so the renderer just edits the store and trusts
    // the lifecycle.
    if (!is_open_) return;
    if (store_ == nullptr) return;

#if defined(WALKERCORE_HAVE_IMGUI)
    // Re-read the current settings each frame so a user edit via
    // the INI file (or another consumer of the store) is reflected
    // without the menu losing its grip. The read is cheap: a few
    // struct copies.
    const WalkerSettings s = store_->current();

    // Position the menu top-left under the status window so the
    // two don't overlap. Fixed size with scroll bars for long
    // keys lists — the menu's full key set (16 sliders, 3 bools,
    // 12 keybindings) needs ~640 px of vertical space.
    ImGui::SetNextWindowPos(ImVec2(8.0f, 96.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 560.0f), ImGuiCond_Always);
    ImGui::Begin("WalkerCore settings (Ctrl+F10 to close)", nullptr,
                 ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Save now")) {
        // The Close() path saves to disk, but a user who wants to
        // commit mid-edit without closing the window gets a button
        // for it. The SetSlider/SetBool/SetKeyBinding calls below
        // already call store_->ApplyAll() — we only need the
        // SaveToDefaultPath() flush.
        if (store_ != nullptr) {
            store_->SaveToDefaultPath();
            WALKERCORE_LOG_INFO("SettingsMenu: manual save");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        // Re-bind the store to its on-disk contents so the in-
        // memory settings mirror the last saved state. This drops
        // any unsaved edits without writing them to disk.
        store_->LoadFromDefaultPath();
        WALKERCORE_LOG_INFO("SettingsMenu: discarded unsaved edits");
    }
    ImGui::Separator();

    // ---- Movement sliders ---------------------------------------
    if (ImGui::CollapsingHeader("Movement")) {
        float walk = s.movement.walkSpeedMps;
        if (ImGui::SliderFloat("Walk speed (m/s)", &walk, 0.5f, 8.0f)) {
            SetSliderValue("walk_speed_mps", walk);
        }
        float run = s.movement.runSpeedMps;
        if (ImGui::SliderFloat("Run speed (m/s)", &run, 1.0f, 12.0f)) {
            SetSliderValue("run_speed_mps", run);
        }
        float crouch = s.movement.crouchSpeedMps;
        if (ImGui::SliderFloat("Crouch speed (m/s)", &crouch, 0.2f, 4.0f)) {
            SetSliderValue("crouch_speed_mps", crouch);
        }
        float jump = s.movement.jumpInitialVelocityMps;
        if (ImGui::SliderFloat("Jump velocity (m/s)", &jump, 0.0f, 12.0f)) {
            SetSliderValue("jump_initial_velocity_mps", jump);
        }
        float grav = s.movement.gravityMps2;
        if (ImGui::SliderFloat("Gravity (m/s^2)", &grav, 1.0f, 30.0f)) {
            SetSliderValue("gravity_mps2", grav);
        }
        float step = s.movement.maxStepUpM;
        if (ImGui::SliderFloat("Max step up (m)", &step, 0.1f, 1.0f)) {
            SetSliderValue("max_step_up_m", step);
        }
        float slope = s.movement.maxSlopeAngleRad;
        if (ImGui::SliderFloat("Max slope (rad)", &slope, 0.1f, 1.5f)) {
            SetSliderValue("max_slope_angle_rad", slope);
        }
    }
    // ---- Camera sliders -----------------------------------------
    if (ImGui::CollapsingHeader("Camera")) {
        float sens = s.movement.mouseSensitivityRadPerUnit;
        if (ImGui::SliderFloat("Mouse sensitivity", &sens, 0.0001f, 0.01f,
                               "%.5f")) {
            SetSliderValue("mouse_sensitivity", sens);
        }
        float fov = s.horizontal_fov_radians;
        if (ImGui::SliderFloat("Horizontal FOV (rad)", &fov, 0.5f, 3.0f)) {
            SetSliderValue("horizontal_fov_radians", fov);
        }
        float bob_v = s.movement.headBobVerticalAmpM;
        if (ImGui::SliderFloat("Head bob vertical (m)", &bob_v, 0.0f, 0.1f)) {
            SetSliderValue("head_bob_vertical_amp_m", bob_v);
        }
        float bob_l = s.movement.headBobLateralAmpM;
        if (ImGui::SliderFloat("Head bob lateral (m)", &bob_l, 0.0f, 0.1f)) {
            SetSliderValue("head_bob_lateral_amp_m", bob_l);
        }
        float bob_f = s.movement.headBobFrequencyHz;
        if (ImGui::SliderFloat("Head bob frequency (Hz)", &bob_f, 0.1f, 5.0f)) {
            SetSliderValue("head_bob_frequency_hz", bob_f);
        }
        float eye_s = s.movement.eyeHeightStandingM;
        if (ImGui::SliderFloat("Eye height standing (m)", &eye_s, 1.0f, 2.2f)) {
            SetSliderValue("eye_height_standing_m", eye_s);
        }
        float eye_c = s.movement.eyeHeightCrouchingM;
        if (ImGui::SliderFloat("Eye height crouching (m)", &eye_c, 0.5f, 1.8f)) {
            SetSliderValue("eye_height_crouching_m", eye_c);
        }
        float trans = s.transition_seconds;
        if (ImGui::SliderFloat("Transition seconds", &trans, 0.0f, 2.0f)) {
            SetSliderValue("transition_seconds", trans);
        }
        float nz = s.near_z;
        if (ImGui::SliderFloat("Near Z", &nz, 0.01f, 1.0f)) {
            SetSliderValue("near_z", nz);
        }
        float fz = s.far_z;
        if (ImGui::SliderFloat("Far Z", &fz, 100.0f, 5000.0f)) {
            SetSliderValue("far_z", fz);
        }
    }
    // ---- Head tracker bools -------------------------------------
    if (ImGui::CollapsingHeader("Head tracker")) {
        bool use_mouse = s.head_tracker.use_mouse;
        if (ImGui::Checkbox("Use mouse (raw input)", &use_mouse)) {
            SetBoolValue("use_mouse", use_mouse);
        }
        bool use_trackir = s.head_tracker.use_trackir;
        if (ImGui::Checkbox("Use TrackIR", &use_trackir)) {
            SetBoolValue("use_trackir", use_trackir);
        }
        bool use_opentrack = s.head_tracker.use_opentrack;
        if (ImGui::Checkbox("Use opentrack (UDP)", &use_opentrack)) {
            SetBoolValue("use_opentrack", use_opentrack);
        }
    }
    // ---- Key bindings -------------------------------------------
    if (ImGui::CollapsingHeader("Key bindings (decimal VK)")) {
        int fwd = s.keys.forward_vk;
        if (ImGui::InputInt("Forward", &fwd)) {
            SetKeyBinding("forward_vk", fwd);
        }
        int bwd = s.keys.backward_vk;
        if (ImGui::InputInt("Backward", &bwd)) {
            SetKeyBinding("backward_vk", bwd);
        }
        int sl = s.keys.strafe_left_vk;
        if (ImGui::InputInt("Strafe left", &sl)) {
            SetKeyBinding("strafe_left_vk", sl);
        }
        int sr = s.keys.strafe_right_vk;
        if (ImGui::InputInt("Strafe right", &sr)) {
            SetKeyBinding("strafe_right_vk", sr);
        }
        int run_k = s.keys.run_vk;
        if (ImGui::InputInt("Run", &run_k)) {
            SetKeyBinding("run_vk", run_k);
        }
        int crouch = s.keys.crouch_vk;
        if (ImGui::InputInt("Crouch", &crouch)) {
            SetKeyBinding("crouch_vk", crouch);
        }
        int jump = s.keys.jump_vk;
        if (ImGui::InputInt("Jump", &jump)) {
            SetKeyBinding("jump_vk", jump);
        }
        int interact = s.keys.interact_vk;
        if (ImGui::InputInt("Interact", &interact)) {
            SetKeyBinding("interact_vk", interact);
        }
        int tw = s.keys.toggle_walk_vk;
        if (ImGui::InputInt("Toggle walk", &tw)) {
            SetKeyBinding("toggle_walk_vk", tw);
        }
        int fl = s.keys.flashlight_vk;
        if (ImGui::InputInt("Flashlight", &fl)) {
            SetKeyBinding("flashlight_vk", fl);
        }
        int eu = s.keys.eye_up_vk;
        if (ImGui::InputInt("Eye up", &eu)) {
            SetKeyBinding("eye_up_vk", eu);
        }
        int ed = s.keys.eye_down_vk;
        if (ImGui::InputInt("Eye down", &ed)) {
            SetKeyBinding("eye_down_vk", ed);
        }
        int er = s.keys.eye_reset_vk;
        if (ImGui::InputInt("Eye reset", &er)) {
            SetKeyBinding("eye_reset_vk", er);
        }
    }
    ImGui::End();
#else
    // No ImGui at build time — the stub warning fires once so a
    // user who somehow toggles the menu (the Open path is the
    // public API and the runtime can call it even when the menu
    // cannot render) knows why nothing happens on screen.
    static bool stub_warned = false;
    if (!stub_warned) {
        stub_warned = true;
        WALKERCORE_LOG_WARN(
            "SettingsMenu: stub active — built without "
            "WALKERCORE_HAVE_IMGUI=1; menu cannot render. Settings "
            "edits via SetSliderValue/SetBoolValue/SetKeyBinding "
            "still take effect immediately and will be persisted "
            "on Close().");
    }
#endif
}

}  // namespace walkercore
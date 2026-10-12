// WalkerCore — SettingsMenu implementation (M11).

#include "walkercore/SettingsMenu.h"

#include <cstring>

#include "walkercore/Log.h"

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
    // Stub. The real M11 lander wraps Dear ImGui here.
    if (!is_open_) return;
    static bool stub_warned = false;
    if (!stub_warned) {
        stub_warned = true;
        WALKERCORE_LOG_WARN(
            "SettingsMenu: stub active — ImGui integration lands in M11 lander. "
            "Settings edits via SetSliderValue/SetBoolValue/SetKeyBinding still "
            "take effect immediately and will be persisted on Close().");
    }
}

}  // namespace walkercore
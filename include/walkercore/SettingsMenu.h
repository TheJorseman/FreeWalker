// WalkerCore — Settings menu (M11).
//
// The in-game settings menu is the user-facing knob for the most common
// settings (mouse sensitivity, walk speed, FOV, key bindings). It does not
// replace the M6 INI; it edits the same WalkerSettings struct that the INI
// stores, then calls SettingsStore::SaveToDefaultPath() on close.
//
// Renderer:
//   - M11 lander uses Dear ImGui under D3D11.
//   - The stub doesn't render anything; it just toggles a flag.
//
// Stub behaviour:
//   - Open()/Close()/Toggle() update `is_open_`.
//   - Render() no-ops.
//   - When Close() is called with dirty_=true, the menu writes its
//     current settings back to disk via the bound SettingsStore.

#ifndef WALKERCORE_SETTINGS_MENU_H
#define WALKERCORE_SETTINGS_MENU_H

#include "walkercore/Settings.h"

namespace walkercore {

class SettingsMenu {
public:
    SettingsMenu();
    ~SettingsMenu();

    void Bind(SettingsStore *store);

    void Open();
    void Close();
    void Toggle();
    bool isOpen() const { return is_open_; }

    // Edit hooks for the renderer. Each takes the field type and a default
    // label; the renderer uses these to build widgets. We expose them as
    // typed accessors so the stub and real impl can stay in sync.
    void SetSliderValue(const char *key, float v);
    void SetBoolValue(const char *key, bool v);
    void SetKeyBinding(const char *key, int vk);
    void SetUIntValue(const char *key, unsigned int v);

    // The render call. The real M11 lander calls ImGui::Begin/End inside
    // here; the stub logs and returns.
    void Render();

    // True iff any widget has been edited since the last Open().
    bool isDirty() const { return dirty_; }

private:
    SettingsStore *store_ = nullptr;
    bool is_open_ = false;
    bool dirty_ = false;
};

}  // namespace walkercore

#endif  // WALKERCORE_SETTINGS_MENU_H
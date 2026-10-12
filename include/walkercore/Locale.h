// WalkerCore — Localization (M12).
//
// User-facing strings live in a per-language key=value map. The English
// defaults are compiled in; other languages load from a file alongside
// WalkerCore.ini:
//
//   %APPDATA%\WalkerCore\lang\<lang>.lang
//
// Format:
//
//   # comment
//   menu.title = WalkerCore Settings
//   menu.mouse_sensitivity = Mouse sensitivity
//   ...
//
// Strings are UTF-8 in memory; we convert to wide chars at the boundary
// when handing them to Windows GUI APIs.
//
// Language selection:
//   1. The M6 INI has a "language" key.
//   2. If unset, fall back to the system's user-default language.
//   3. If the .lang file is missing, fall back to English.

#ifndef WALKERCORE_LOCALE_H
#define WALKERCORE_LOCALE_H

#include <string>
#include <unordered_map>

#include "walkercore/Settings.h"

namespace walkercore {

// BCP-47 language tag, lower-case. Examples: "en", "en-us", "es", "fr".
using LanguageTag = std::string;

class LocaleManager {
public:
    LocaleManager();
    ~LocaleManager();

    // Initialise with a target language. "en" or "" selects English
    // defaults; otherwise the .lang file is loaded from
    // %APPDATA%\WalkerCore\lang\<tag>.lang.
    bool SelectLanguage(const LanguageTag &tag);

    // Look up a string by key. Falls back to the English default if the
    // key isn't in the loaded language.
    std::string Get(const char *key) const;

    // True if the loaded language actually has overrides for any keys.
    bool hasOverrides() const { return !overrides_.empty(); }

    const LanguageTag &currentTag() const { return current_; }

    // List of tags we ship with. Just "en" for M12; multi-language
    // expansion is M12 lander.
    static const LanguageTag kDefaultLanguage;  // "en"

private:
    LanguageTag current_;
    // Loaded overrides from the .lang file.
    std::unordered_map<std::string, std::string> overrides_;
    std::string GetOverride(const char *key) const;
    std::string overrides_path_;
};

// Compile-time English defaults. The macros below are intentional — they
// match the keys the rest of the plugin uses so a translator can rename
// strings in one place.
namespace strings {
constexpr const char *kMenuTitle = "WalkerCore Settings";
constexpr const char *kMenuMouseSensitivity = "Mouse sensitivity";
constexpr const char *kMenuWalkSpeed = "Walk speed";
constexpr const char *kMenuRunSpeed = "Run speed";
constexpr const char *kMenuHeadBob = "Head bob";
constexpr const char *kMenuFov = "Field of view";
constexpr const char *kMenuTransitionSeconds = "Door transition";
constexpr const char *kMenuForward = "Forward";
constexpr const char *kMenuBackward = "Backward";
constexpr const char *kMenuStrafeLeft = "Strafe left";
constexpr const char *kMenuStrafeRight = "Strafe right";
constexpr const char *kMenuRun = "Run";
constexpr const char *kMenuCrouch = "Crouch";
constexpr const char *kMenuJump = "Jump";
constexpr const char *kMenuInteract = "Interact";
constexpr const char *kMenuToggleWalk = "Toggle walk";
constexpr const char *kMenuFlashlight = "Flashlight";
constexpr const char *kMenuEyeUp = "Look up";
constexpr const char *kMenuEyeDown = "Look down";
constexpr const char *kMenuEyeReset = "Look reset";
constexpr const char *kKnockedOut = "You were knocked out";
constexpr const char *kFuelFull = "Fuel and AdBlue full — fueling complete";
constexpr const char *kCouplingComplete = "Trailer attached";
constexpr const char *kSettingsSaved = "Settings saved";
constexpr const char *kSettingsReloaded = "Settings reloaded from disk";
}  // namespace strings

}  // namespace walkercore

#endif  // WALKERCORE_LOCALE_H
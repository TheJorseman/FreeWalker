// WalkerCore — LocaleManager implementation (M12).

#include "walkercore/Locale.h"

#include <fstream>

#include "walkercore/Log.h"

namespace walkercore {

const LanguageTag LocaleManager::kDefaultLanguage = "en";

LocaleManager::LocaleManager() {
    current_ = kDefaultLanguage;
}

LocaleManager::~LocaleManager() = default;

bool LocaleManager::SelectLanguage(const LanguageTag &tag) {
    current_ = tag.empty() ? kDefaultLanguage : tag;

    // English: nothing to load — defaults live in `strings::`.
    if (current_ == kDefaultLanguage) {
        overrides_.clear();
        WALKERCORE_LOG_INFO("Locale: switched to %s (defaults)",
                            current_.c_str());
        return true;
    }

    // Try to load <APPDATA>\WalkerCore\lang\<tag>.lang. For M12 we ship
    // only English; other languages will be added once a translator
    // provides a .lang file.
    const std::string path = "<unknown>\\WalkerCore\\lang\\" + current_ + ".lang";
    overrides_path_ = path;
    std::ifstream f(path);
    if (!f.is_open()) {
        WALKERCORE_LOG_WARN(
            "Locale: %s.lang missing, falling back to English defaults",
            current_.c_str());
        return false;
    }

    int loaded = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        // Trim.
        const auto ws = [](const std::string &s) {
            const auto a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return std::string{};
            const auto b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        };
        overrides_[ws(key)] = ws(value);
        loaded++;
    }
    WALKERCORE_LOG_INFO("Locale: loaded %d strings from %s",
                        loaded, current_.c_str());
    return loaded > 0;
}

std::string LocaleManager::GetOverride(const char *key) const {
    if (key == nullptr) return {};
    const auto it = overrides_.find(key);
    if (it != overrides_.end()) return it->second;
    return {};
}

std::string LocaleManager::Get(const char *key) const {
    const std::string o = GetOverride(key);
    if (!o.empty()) return o;
    // English defaults. The macro map below returns the compile-time
    // constant for each known key.
    if (key == nullptr) return {};
#define MATCH(s) if (std::string(key) == strings::s) return strings::s
    MATCH(kMenuTitle);
    MATCH(kMenuMouseSensitivity);
    MATCH(kMenuWalkSpeed);
    MATCH(kMenuRunSpeed);
    MATCH(kMenuHeadBob);
    MATCH(kMenuFov);
    MATCH(kMenuTransitionSeconds);
    MATCH(kMenuForward);
    MATCH(kMenuBackward);
    MATCH(kMenuStrafeLeft);
    MATCH(kMenuStrafeRight);
    MATCH(kMenuRun);
    MATCH(kMenuCrouch);
    MATCH(kMenuJump);
    MATCH(kMenuInteract);
    MATCH(kMenuToggleWalk);
    MATCH(kMenuFlashlight);
    MATCH(kMenuEyeUp);
    MATCH(kMenuEyeDown);
    MATCH(kMenuEyeReset);
    MATCH(kKnockedOut);
    MATCH(kFuelFull);
    MATCH(kCouplingComplete);
    MATCH(kSettingsSaved);
    MATCH(kSettingsReloaded);
#undef MATCH
    return std::string("?") + (key ? key : "(null)") + "?";
}

}  // namespace walkercore
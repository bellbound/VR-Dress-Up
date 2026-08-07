// Provider-side implementation of DressUp interface
// Exports the interface via DLL export for other plugins to consume
//
// CONSUMER PLUGINS: Copy DressUpInterface001.h to your project and create your
// own implementation of GetInterface() using the pattern in the header comments.

#include "DressUpInterface001.h"
#include "../dressup/OutfitLockManager.h"
#include "../log.h"
#include <Windows.h>

namespace DressUp {

// Build number - increment with each release
constexpr uint32_t BUILD_NUMBER = 1;

// Menu opening enabled state - controlled via API (true = enabled by default)
static bool g_menuOpeningEnabled = true;

// =============================================================================
// Interface Implementation
// =============================================================================

class Interface001Impl : public Interface001 {
public:
    uint32_t GetVersion() override {
        return DRESSUP_INTERFACE_VERSION;
    }

    uint32_t GetBuild() override {
        return BUILD_NUMBER;
    }

    void SetOpenDressMenuEnabled(bool enabled) override {
        g_menuOpeningEnabled = enabled;
        spdlog::info("DressUp::Interface001: Menu opening {} by external mod",
            enabled ? "enabled" : "disabled");
    }

    bool IsOpenDressMenuEnabled() override {
        return g_menuOpeningEnabled;
    }

    bool LockActor(RE::Actor* actor) override {
        if (!actor) {
            spdlog::warn("DressUp::Interface001::LockActor - null actor");
            return false;
        }
        return OutfitLockManager::GetSingleton()->Lock(actor);
    }

    bool UnlockActor(RE::Actor* actor) override {
        if (!actor) {
            spdlog::warn("DressUp::Interface001::UnlockActor - null actor");
            return false;
        }
        return OutfitLockManager::GetSingleton()->Unlock(actor);
    }

    bool IsActorLocked(RE::Actor* actor) override {
        if (!actor) {
            return false;
        }
        return OutfitLockManager::GetSingleton()->IsLocked(actor);
    }
};

// Singleton instance
static Interface001Impl g_interface;

// =============================================================================
// Internal accessor for InputDispatcher to check if menu opening is enabled
// =============================================================================

bool IsMenuOpeningEnabledInternal() {
    return g_menuOpeningEnabled;
}

// Shared with Interface002 so both interfaces drive the same toggle rather than
// each keeping its own idea of it.
void SetMenuOpeningEnabledInternal(bool enabled) {
    g_menuOpeningEnabled = enabled;
}

} // namespace DressUp

// =============================================================================
// DLL Export - called by consuming plugins
// =============================================================================

// Export function for other plugins to call
// Uses extern "C" to prevent name mangling
#include "DressUpInterface002.h"

namespace DressUp {
// Implemented in DressUpInterface002.cpp, where Interface002Impl is complete. An
// accessor rather than an extern object, so this file never needs the concrete type.
Interface002* GetInterface002Impl();
}  // namespace DressUp

extern "C" __declspec(dllexport) void* GetDressUpInterface(unsigned int version) {
    spdlog::info("DressUp: GetDressUpInterface called with version {}", version);

    if (version == 1) {
        return static_cast<DressUp::Interface001*>(&DressUp::g_interface);
    }
    if (version == 2) {
        return DressUp::GetInterface002Impl();
    }

    spdlog::warn("DressUp: Unknown interface version {} requested", version);
    return nullptr;
}

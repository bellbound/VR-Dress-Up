#pragma once

// =============================================================================
// DressUp VR Plugin Interface v001
// =============================================================================
// This header is designed to be copied to other SKSE projects.
// Provides programmatic control over the DressUp VR plugin.
//
// Quick Start:
//   1. Copy this header to your project
//   2. Get interface after DataLoaded: auto* api = DressUp::GetInterface();
//   3. Use API functions to control menu and actor locking
//
// All types use virtual methods for ABI stability across DLL boundaries.
//
// Thread Safety:
//   All API calls must be made from the game's main thread.
// =============================================================================

#include <cstdint>

// Forward declaration for Skyrim types (consumer must have CommonLib)
namespace RE {
    class Actor;
}

namespace DressUp {

// =============================================================================
// Interface001 - Main entry point
// =============================================================================

// Interface version for compatibility checking
constexpr uint32_t DRESSUP_INTERFACE_VERSION = 1;

struct Interface001 {
    // === Version ===
    // Returns DRESSUP_INTERFACE_VERSION (1 for this interface)
    virtual uint32_t GetVersion() = 0;
    // Returns implementation build number (increments with each release)
    virtual uint32_t GetBuild() = 0;

    // === Menu Opening Control ===
    // Controls whether the normal "grab NPC + trigger" hotkey opens the menu.
    // When disabled, the hotkey won't open the menu (useful when another mod
    // needs exclusive control of the trigger input).
    virtual void SetOpenDressMenuEnabled(bool enabled) = 0;
    virtual bool IsOpenDressMenuEnabled() = 0;

    // === Actor Locking ===
    // Locks an actor's current outfit. When locked, the plugin will prevent
    // equipment changes and restore the locked outfit if modified.
    // Returns true on success, false if actor is null, player, or already locked.
    virtual bool LockActor(RE::Actor* actor) = 0;

    // Unlocks an actor's outfit, allowing equipment changes.
    // Returns true on success, false if actor is null or not locked.
    virtual bool UnlockActor(RE::Actor* actor) = 0;

    // Returns true if the actor's outfit is currently locked.
    virtual bool IsActorLocked(RE::Actor* actor) = 0;

    // === Reserved for future expansion ===
    virtual void _reserved1() {}
    virtual void _reserved2() {}
    virtual void _reserved3() {}
    virtual void _reserved4() {}
    virtual void _reserved5() {}
    virtual void _reserved6() {}
    virtual void _reserved7() {}
    virtual void _reserved8() {}
};

// =============================================================================
// Interface Accessor (Consumer Implementation Required)
// =============================================================================
//
// Consumers must implement GetInterface() in their own .cpp file:
//
//   #include "DressUpInterface001.h"
//   #include <Windows.h>
//
//   namespace DressUp {
//       typedef void* (*GetDressUpInterfaceFunc)(unsigned int version);
//       static Interface001* g_cachedInterface = nullptr;
//
//       Interface001* GetInterface() {
//           if (g_cachedInterface) return g_cachedInterface;
//           HMODULE hModule = GetModuleHandleA("DressUpVR.dll");
//           if (!hModule) return nullptr;
//           auto fn = (GetDressUpInterfaceFunc)GetProcAddress(hModule, "GetDressUpInterface");
//           if (!fn) return nullptr;
//           g_cachedInterface = static_cast<Interface001*>(fn(1));
//           return g_cachedInterface;
//       }
//   }

} // namespace DressUp

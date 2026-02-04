#pragma once

// =============================================================================
// ThreeDUIActorMenu.h - Public API for Actor Tween Menu
// =============================================================================
// This API allows multiple mods to share the "grab NPC + press trigger" UX.
//
// Usage:
// 1. Get interface via GetActorMenuInterface()
// 2. Register elements with eligibility and activation callbacks
// 3. When user grabs NPC + presses trigger:
//    - 0 eligible elements → nothing happens
//    - 1 eligible element → activation callback fired immediately
//    - 2+ eligible elements → tween menu shown, activation on selection
//
// Example:
//   auto* actorMenu = P3DUI::GetActorMenuInterface();
//
//   P3DUI::ActorMenuElementConfig config =
//       P3DUI::ActorMenuElementConfig::Default("MyMod", "my_action");
//   config.texturePath = "textures/MyMod/icon.dds";
//   config.tooltip = L"My Action";
//
//   actorMenu->RegisterElement(
//       config,
//       [](RE::Actor* actor, void*) { return !actor->IsDead(); },
//       [](RE::Actor* actor, const char*, const char*, void*) {
//           DoSomethingWithActor(actor);
//       },
//       nullptr);
// =============================================================================

#include <cstdint>

// Forward declaration for RE::Actor
namespace RE { class Actor; }

namespace P3DUI {

// Version constant for this interface
constexpr uint32_t P3DUI_ACTORMENU_VERSION = 1;

// =============================================================================
// Configuration Structures
// =============================================================================

struct ActorMenuElementConfig {
    uint32_t structSize;            // Must be sizeof(ActorMenuElementConfig)
    const char* modId;              // Required: mod identifier (e.g., "DressUpVR")
    const char* elementId;          // Required: unique element ID within this mod
    const char* texturePath;        // Icon texture path (mutually exclusive with modelPath)
    const char* modelPath;          // 3D model path (mutually exclusive with texturePath)
    uint32_t formID;                // Optional: form ID for auto model lookup
    const wchar_t* tooltip;         // Display name shown on hover
    float scale;                    // Scale multiplier (default 0.3)

    // Convenience factory with sensible defaults
    static ActorMenuElementConfig Default(const char* modId, const char* elementId) {
        ActorMenuElementConfig c{};
        c.structSize = sizeof(ActorMenuElementConfig);
        c.modId = modId;
        c.elementId = elementId;
        c.texturePath = nullptr;
        c.modelPath = nullptr;
        c.formID = 0;
        c.tooltip = nullptr;
        c.scale = 1.2f;
        return c;
    }
};

// =============================================================================
// Callback Types
// =============================================================================

// Called when the actor menu would open to determine if this element should appear.
//
// Parameters:
//   actor    - The NPC being interacted with (grabbed by HIGGS)
//   userData - Pointer passed during RegisterElement()
//
// Returns:
//   true  - Element should appear in the menu for this actor
//   false - Element should be hidden for this actor
//
// Notes:
//   - Called on the game's main thread
//   - Should be fast (avoid I/O or heavy computation)
//   - May be called multiple times per menu open
typedef bool (*ActorMenuEligibilityCallback)(RE::Actor* actor, void* userData);

// Called when the user selects this element from the actor menu.
//
// Parameters:
//   actor     - The NPC being interacted with
//   modId     - The mod that registered this element
//   elementId - The specific element that was activated
//   userData  - Pointer passed during RegisterElement()
//
// Notes:
//   - Called on the game's main thread
//   - Menu is hidden before this callback is invoked
//   - Safe to open your own menus from this callback
typedef void (*ActorMenuActivationCallback)(
    RE::Actor* actor,
    const char* modId,
    const char* elementId,
    void* userData);

// =============================================================================
// ActorMenuInterface
// =============================================================================

struct ActorMenuInterface {
    // === Version ===
    // Returns the interface version for compatibility checking
    virtual uint32_t GetInterfaceVersion() = 0;

    // === Registration ===

    // Register an element that can appear in the actor menu.
    // Only one element per (modId, elementId) pair is allowed.
    //
    // Parameters:
    //   config     - Visual configuration (icon, tooltip, scale)
    //   isEligible - Callback to check if element should appear for a given actor
    //   onActivate - Callback when user selects this element
    //   userData   - Optional pointer passed to callbacks (default nullptr)
    //
    // Returns:
    //   true  - Registration successful
    //   false - Failed (already registered, invalid params, or null callbacks)
    virtual bool RegisterElement(
        const ActorMenuElementConfig& config,
        ActorMenuEligibilityCallback isEligible,
        ActorMenuActivationCallback onActivate,
        void* userData = nullptr) = 0;

    // Update an existing element's visual configuration.
    // Cannot change callbacks - unregister and re-register to change those.
    //
    // Returns:
    //   true  - Update successful
    //   false - Element not found or invalid config
    virtual bool UpdateElement(const ActorMenuElementConfig& config) = 0;

    // Unregister a specific element.
    // Safe to call even if not registered (no-op).
    virtual void UnregisterElement(const char* modId, const char* elementId) = 0;

    // Unregister all elements from a mod.
    // Call this during mod shutdown to clean up.
    virtual void UnregisterMod(const char* modId) = 0;

    // === Query ===

    // Check if a specific element is registered
    virtual bool IsElementRegistered(const char* modId, const char* elementId) = 0;

    // Check if the actor menu is currently visible
    virtual bool IsMenuOpen() = 0;

    // Get the actor currently being interacted with (null if menu closed)
    virtual RE::Actor* GetCurrentActor() = 0;

    // === Manual Control ===

    // Force close the menu without triggering any activation.
    // Use sparingly - prefer letting the user naturally close via release.
    virtual void ForceClose() = 0;

    // === Reserved for future expansion ===
    virtual void _actormenu_reserved1() {}
    virtual void _actormenu_reserved2() {}
    virtual void _actormenu_reserved3() {}
    virtual void _actormenu_reserved4() {}

protected:
    virtual ~ActorMenuInterface() = default;
};

// =============================================================================
// Interface Accessor
// =============================================================================

// Returns the ActorMenu interface singleton.
//
// Call timing:
//   - Safe to call after SKSE PostPostLoad
//   - First call initializes the actor menu subsystem
//
// Returns:
//   - Pointer to ActorMenuInterface on success
//   - nullptr if initialization fails (check logs for details)
//
// Thread safety:
//   - Must be called from the game's main thread
ActorMenuInterface* GetActorMenuInterface();

} // namespace P3DUI

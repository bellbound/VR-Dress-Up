#pragma once

#include "api/ThreeDUIActorMenu.h"
#include "InputManager.h"
#include "NpcUtils.h"
#include "DressupMenuManager.h"
#include "higgsinterface001.h"
#include "log.h"

// Forward declaration for menu opening check (from DressUpInterface001.cpp)
namespace DressUp {
    bool IsMenuOpeningEnabledInternal();
}

// =============================================================================
// InputDispatcher
// =============================================================================
// Registers DressUpVR with the 3DUI ActorMenu system.
// This replaces the old direct trigger handling - now ActorMenu coordinates
// between multiple mods that want to use the "grab NPC + trigger" UX.
//
// Flow:
// 1. User grabs NPC with HIGGS
// 2. User presses trigger
// 3. ActorMenu calls our eligibility callback
// 4. If we're the only eligible mod: activation callback fires immediately
// 5. If multiple mods eligible: tween menu shown, user picks one
// 6. Our activation callback opens DressupMenuManager
// 7. We also register a trigger callback to handle the "attach to hand" UX

class InputDispatcher
{
public:
    static InputDispatcher* GetSingleton()
    {
        static InputDispatcher instance;
        return &instance;
    }

    void Initialize()
    {
        if (m_initialized) {
            spdlog::warn("InputDispatcher already initialized");
            return;
        }

        // Get ActorMenu interface
        auto* actorMenu = P3DUI::GetActorMenuInterface();
        if (!actorMenu) {
            spdlog::error("InputDispatcher::Initialize - ActorMenu interface not available!");
            return;
        }

        // Register our element with ActorMenu
        P3DUI::ActorMenuElementConfig config =
            P3DUI::ActorMenuElementConfig::Default("DressUpVR", "dressup");
        config.texturePath = "textures\\VRDressup\\clothes.dds";
        config.tooltip = L"Dress Up";
        config.scale = 1.4f;

        bool registered = actorMenu->RegisterElement(
            config,
            &InputDispatcher::IsEligible,
            &InputDispatcher::OnActivate,
            this);

        if (!registered) {
            spdlog::error("InputDispatcher::Initialize - Failed to register with ActorMenu");
            return;
        }

        // Also register trigger callback for the "attach to hand then place" UX
        // This handles trigger RELEASE when our menu is open
        auto* inputMgr = InputManager::GetSingleton();
        if (inputMgr && inputMgr->IsInitialized()) {
            uint64_t triggerMask = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
            m_triggerCallbackId = inputMgr->AddVrButtonCallback(triggerMask,
                [](bool isLeft, bool isReleased, vr::EVRButtonId) -> bool {
                    return InputDispatcher::GetSingleton()->OnTriggerInput(isLeft, isReleased);
                });
            spdlog::info("InputDispatcher: Registered trigger callback for menu positioning");
        }

        m_initialized = true;
        spdlog::info("InputDispatcher: Registered with ActorMenu");
    }

    void Shutdown()
    {
        if (!m_initialized) {
            return;
        }

        // Unregister trigger callback
        if (m_triggerCallbackId != InputManager::InvalidCallbackId) {
            auto* inputMgr = InputManager::GetSingleton();
            if (inputMgr) {
                inputMgr->RemoveVrButtonCallback(m_triggerCallbackId);
            }
            m_triggerCallbackId = InputManager::InvalidCallbackId;
        }

        // Unregister from ActorMenu
        auto* actorMenu = P3DUI::GetActorMenuInterface();
        if (actorMenu) {
            actorMenu->UnregisterElement("DressUpVR", "dressup");
        }

        m_initialized = false;
        spdlog::info("InputDispatcher shut down");
    }

private:
    InputDispatcher() = default;
    ~InputDispatcher() = default;
    InputDispatcher(const InputDispatcher&) = delete;
    InputDispatcher& operator=(const InputDispatcher&) = delete;

    // Trigger input handler - only handles RELEASE for menu positioning
    bool OnTriggerInput(bool /*isLeft*/, bool isReleased)
    {
        // Only handle release, and only when our menu is open
        if (!isReleased) {
            return false;  // Don't consume press - let ActorMenu handle it
        }

        auto* menuMgr = DressupMenuManager::GetSingleton();
        if (!menuMgr->IsMenuOpen()) {
            return false;  // Our menu isn't open
        }

        // Menu is open and trigger released - end the positioning
        spdlog::info("InputDispatcher: Trigger released - ending menu positioning");
        menuMgr->OnTriggerRelease();
        return false;  // Don't consume - other systems may need this
    }

    // Eligibility callback - called by ActorMenu to check if we should appear
    static bool IsEligible(RE::Actor* actor, void* /*userData*/)
    {
        if (!actor) return false;

        // Check if disabled by external mod via our API
        if (!DressUp::IsMenuOpeningEnabledInternal()) {
            spdlog::trace("InputDispatcher::IsEligible - Disabled by external mod");
            return false;
        }

        // Check if actor is valid for dressup
        if (actor->IsDead()) {
            spdlog::trace("InputDispatcher::IsEligible - Actor is dead");
            return false;
        }

        if (actor->IsChild()) {
            spdlog::trace("InputDispatcher::IsEligible - Actor is a child");
            return false;
        }

        // Check for creature/animal (non-humanoid race)
        auto* race = actor->GetRace();
        if (race) {
            if (!race->GetPlayable() && !race->AllowsPickpocket()) {
                spdlog::trace("InputDispatcher::IsEligible - Actor is a creature (race: {})",
                    race->GetFormEditorID() ? race->GetFormEditorID() : "unknown");
                return false;
            }
        }

        // Check if dressup menu is already open for a different NPC
        auto* menuMgr = DressupMenuManager::GetSingleton();
        if (menuMgr->IsMenuOpen()) {
            RE::Actor* currentTarget = menuMgr->GetCurrentTargetActor();
            if (currentTarget && currentTarget != actor) {
                // Different NPC - we're still eligible, we'll close current menu
                return true;
            }
            // Same NPC already open - not eligible
            return false;
        }

        return true;
    }

    // Activation callback - called by ActorMenu when user selects our element
    static void OnActivate(RE::Actor* actor, const char* /*modId*/,
                          const char* /*elementId*/, void* userData)
    {
        if (!actor) return;

        auto* self = static_cast<InputDispatcher*>(userData);

        spdlog::info("InputDispatcher::OnActivate - Opening DressUp menu for '{}'",
            actor->GetName() ? actor->GetName() : "unknown");

        // Determine which hand has the NPC (other hand pressed trigger)
        // We want the menu at the trigger hand position
        bool npcInLeftHand = false;
        if (g_higgsInterface) {
            RE::TESObjectREFR* leftObj = g_higgsInterface->GetGrabbedObject(true);
            if (leftObj && leftObj->As<RE::Actor>() == actor) {
                npcInLeftHand = true;
            }
        }

        // Menu opens at trigger hand (opposite of NPC hand)
        bool menuAtLeftHand = !npcInLeftHand;
        self->m_lastMenuHand = menuAtLeftHand;

        auto* menuMgr = DressupMenuManager::GetSingleton();
        menuMgr->ShowDressUpMenu(actor, menuAtLeftHand);
    }

    bool m_initialized = false;
    InputManager::CallbackId m_triggerCallbackId = InputManager::InvalidCallbackId;
    bool m_lastMenuHand = false;  // Track which hand opened the menu
};

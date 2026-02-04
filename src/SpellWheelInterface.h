#pragma once

// SpellWheel Interface adapter for CommonLibSSE-NG
// Adapted from SpellWheelVR's spellwheelinterface001.h (merged with v002 features)

#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace spellwheelPluginApi {

    // Orb definition for custom wheels (external API - uses const char* for ABI stability)
    struct CustomOrbDef {
        const char* id;             // Unique identifier for callbacks (required)
        const char* displayName;    // Label shown to user (can be nullptr)
        const char* modelPath;      // NIF model path, nullptr = default orb
        const char* texturePath;    // DDS texture for orb face, full path from Data (e.g. "Data/Interfaces/MyMod/icon.dds"). Can be nullptr.
        const char* borderColor;    // Hex color "ff0000" or nullptr for none

        // Scaling options (optional - use formType OR manual scales, not both)
        int formType = 0;           // RE::FormType enum value for auto-scaling (e.g. 26 for Armor). 0 = use manual scales below.
        float orbScaleSmall = 0.0f; // Custom small scale. 0 = use default (0.07f). Only used when formType == 0.
        float orbScaleGrown = 0.0f; // Custom grown scale. 0 = use default (0.12f). Only used when formType == 0.
    };

    // Callback signatures
    typedef void(*OrbSelectedCallback)(const char* wheelId, const char* orbId, int orbIndex, bool isLeftHand);
    typedef void(*WheelOpenedCallback)(const char* wheelId, bool isLeftHand);
    typedef void(*WheelClosedCallback)(const char* wheelId, bool isLeftHand, bool wasCancelled);

    // Wheel configuration - combines callbacks and options
    struct WheelConfig {
        // === Callbacks (all optional) ===
        OrbSelectedCallback onOrbSelected = nullptr;
        WheelOpenedCallback onWheelOpened = nullptr;
        WheelClosedCallback onWheelClosed = nullptr;

        // === Display Options ===
        float orbScale = 1.0f;              // Orb size multiplier (default 1.0)
        float selectionDistance = 1.0f;     // Selection distance multiplier (default 1.0)
        float verticalOffset = 0.0f;        // Vertical position offset from default
        float wheelRadius = 1.0f;           // Wheel radius multiplier (default 1.0)

        // === Behavior Options ===
        const char* title = nullptr;        // Optional title displayed above wheel
    };

    // Interface definition (merged v001 + custom wheel API)
    struct ISpellWheelInterface001 {
        // === Original API (v1.0) ===
        virtual unsigned int getBuildNumber() = 0;
        virtual bool IsMainWheelOpen() = 0;
        virtual bool IsSecondaryWheelOpen() = 0;
        virtual void SpawnConjurationCircle(void* pos) = 0;  // NiPoint3 pos - use void* for CommonLib compat
        virtual void CloseOstimWheels() = 0;

        // === Custom Wheel API (v1.1) ===
        // Register a new wheel with its configuration
        // If wheelId already registered, updates the existing wheel's config (wheel must be closed)
        virtual bool RegisterWheel(const char* wheelId, const WheelConfig& config) = 0;

        // Unregister a wheel (frees memory)
        virtual bool UnregisterWheel(const char* wheelId) = 0;

        // Check if a wheel is registered
        virtual bool IsWheelRegistered(const char* wheelId) = 0;

        // === Orb Management ===
        virtual void ClearOrbs(const char* wheelId) = 0;
        virtual void AddOrb(const char* wheelId, const CustomOrbDef& orb) = 0;
        virtual void SetOrbs(const char* wheelId, const CustomOrbDef* orbs, int count) = 0;
        virtual bool UpdateOrb(const char* wheelId, int orbIndex, const CustomOrbDef& orb) = 0;

        // === Wheel Lifecycle ===
        virtual bool OpenWheel(const char* wheelId, bool isLeftHand) = 0;
        virtual void CloseWheel(const char* wheelId, bool isLeftHand, bool cancelled = false) = 0;
        virtual bool IsWheelOpen(const char* wheelId, bool isLeftHand) = 0;
    };

    // Message struct used to fetch SpellWheel VR's interface
    struct SpellWheelMessage {
        enum { kMessage_GetInterface = 0xFA27C15D };  // v001 interface message type
        void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
    };

    // Stores the API after it has already been fetched
    inline ISpellWheelInterface001* g_spellWheelInterface = nullptr;

    // Get the SpellWheel interface via SKSE messaging
    inline ISpellWheelInterface001* GetSpellWheelInterface001(
        const SKSE::MessagingInterface* messagingInterface)
    {
        // If the interface has already been fetched, return the same object
        if (g_spellWheelInterface) {
            return g_spellWheelInterface;
        }

        if (!messagingInterface) {
            spdlog::error("GetSpellWheelInterface001: messagingInterface is null");
            return nullptr;
        }

        // Dispatch a message to get the plugin interface from SpellWheelVR
        SpellWheelMessage swMessage;
        bool dispatched = messagingInterface->Dispatch(
            SpellWheelMessage::kMessage_GetInterface,
            (void*)&swMessage,
            sizeof(SpellWheelMessage*),
            "SpellWheelVR"
        );

        spdlog::info("SpellWheelVR message dispatch result: {}", dispatched);

        if (!swMessage.GetApiFunction) {
            spdlog::error("Failed to get SpellWheelVR API function - SpellWheelVR may not be installed");
            return nullptr;
        }

        // Fetch the API for revision 1 of the SpellWheel interface
        g_spellWheelInterface = static_cast<ISpellWheelInterface001*>(swMessage.GetApiFunction(1));

        if (g_spellWheelInterface) {
            spdlog::info("Successfully obtained SpellWheelVR interface v001 (build {})",
                g_spellWheelInterface->getBuildNumber());
        }

        return g_spellWheelInterface;
    }

}

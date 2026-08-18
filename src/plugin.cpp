#include "log.h"
#include "settings.h"
#include "InputManager.h"
#include "InputDispatcher.h"
#include "higgsinterface001.h"
#include "DressupMenuManager.h"
#include "MenuChecker.h"
#include "dressup/OutfitLockManager.h"
#include "dressup/OutfitFormBackend.h"
#include "dressup/SeverActionsCompat.h"
#include "dressup/DeviceCompat.h"
#include "dressup/UndressManager.h"
#include "dressup/WeaponLockManager.h"
#include "dressup/GalleryStateManager.h"
#include "dressup/ArmorModManager.h"
#include "InventoryManager.h"

// Flag to track if 3DUI is missing (set at DataLoaded, used for deferred notification)
static bool g_3DUIMissing = false;
static bool g_3DUIMissingNotificationShown = false;

// Combined serialization callbacks for all managers
namespace SerializationCallbacks
{
    void OnGameSave(SKSE::SerializationInterface* a_intfc)
    {
        OutfitLockManager::OnGameSave(a_intfc);
        OutfitFormBackend::OnGameSave(a_intfc);
        SeverActionsCompat::OnGameSave(a_intfc);
        UndressManager::OnGameSave(a_intfc);
        GalleryStateManager::OnGameSave(a_intfc);
        WeaponLockManager::OnGameSave(a_intfc);
    }

    void OnGameLoad(SKSE::SerializationInterface* a_intfc)
    {
        // Clear all managers before processing records
        OutfitLockManager::OnPreLoad();
        OutfitFormBackend::OnPreLoad();
        SeverActionsCompat::OnPreLoad();
        UndressManager::OnPreLoad();
        GalleryStateManager::OnPreLoad();
        WeaponLockManager::OnPreLoad();

        // Central dispatch loop — each record goes to the correct manager
        std::uint32_t type, version, length;
        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            switch (type) {
            case OutfitLockManager::kOutfitRecord:
            case OutfitLockManager::kPlayerItemsRecord:
                OutfitLockManager::OnLoadRecord(a_intfc, type, version, length);
                break;
            case OutfitFormBackend::kRecord:
                OutfitFormBackend::OnLoadRecord(a_intfc, type, version, length);
                break;
            case SeverActionsCompat::kRecord:
                SeverActionsCompat::OnLoadRecord(a_intfc, type, version, length);
                break;
            case UndressManager::kUndressRecord:
                UndressManager::OnLoadRecord(a_intfc, type, version, length);
                break;
            case GalleryStateManager::kRecord:
                GalleryStateManager::OnLoadRecord(a_intfc, type, version, length);
                break;
            case WeaponLockManager::kRecord:
                WeaponLockManager::OnLoadRecord(a_intfc, type, version, length);
                break;
            default:
                if (length > 0) {
                    std::vector<char> skipBuffer(length);
                    a_intfc->ReadRecordData(skipBuffer.data(), length);
                }
                spdlog::warn("OnGameLoad - Unknown record type: {:08X}, skipped {} bytes", type, length);
                break;
            }
        }

        spdlog::info("OnGameLoad - Done loading all records");
    }

    void OnRevert(SKSE::SerializationInterface* a_intfc)
    {
        OutfitLockManager::OnRevert(a_intfc);
        OutfitFormBackend::OnRevert(a_intfc);
        SeverActionsCompat::OnRevert(a_intfc);
        UndressManager::OnRevert(a_intfc);
        GalleryStateManager::OnRevert(a_intfc);
        WeaponLockManager::OnRevert(a_intfc);
    }
}

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kPostLoad:
		spdlog::info("PostLoad");
		break;

	case SKSE::MessagingInterface::kPostPostLoad:
		spdlog::info("PostPostLoad - Getting HIGGS interface");
		{
			auto* messaging = SKSE::GetMessagingInterface();
			HiggsPluginAPI::GetHiggsInterface001(messaging);

			if (g_higgsInterface) {
				spdlog::info("Got HIGGS interface! Build number: {}", g_higgsInterface->GetBuildNumber());
			} else {
				spdlog::error("Failed to get HIGGS interface - is HIGGS installed?");
			}
		}
		break;

	case SKSE::MessagingInterface::kDataLoaded:
		spdlog::info("DataLoaded - Initializing managers");

		// Register menu event handler for input blocking during menus
		MenuChecker::RegisterEventSink();

		// Initialize 3DUI interface for DressupMenuManager
		spdlog::info("Attempting to get 3DUI interface...");
		if (DressupMenuManager::GetSingleton()->Initialize()) {
			spdlog::info("DressupMenuManager initialized successfully");
		} else {
			spdlog::warn("Failed to initialize DressupMenuManager - 3DUI.dll may not be installed");
			g_3DUIMissing = true;
		}

		// Initialize InputManager (needs OpenVR hook API)
		InputManager::GetSingleton()->Initialize();

		// Initialize InputDispatcher (registers button callbacks)
		InputDispatcher::GetSingleton()->Initialize();

		// Outfit backend: detect SeverActions first, then resolve the outfit pool
		SeverActionsCompat::Initialize();
		OutfitFormBackend::GetSingleton()->Initialize();

		// Devious Devices: resolve its keywords so we can tell a device's two halves
		// apart before anything asks us to equip or strip one
		DeviceCompat::Initialize();

		// Note: ArmorModManager cache is built lazily on first gallery open
		// to avoid blocking startup and potential timing issues with VR init

		break;

	case SKSE::MessagingInterface::kPreLoadGame:
		spdlog::info("PreLoadGame");
		break;

	case SKSE::MessagingInterface::kPostLoadGame:
		// Initialize OutfitLockManager (registers for equip events)
		OutfitLockManager::GetSingleton()->Initialize();
		// Initialize InventoryManager (registers for equip events to return items on unequip)
		InventoryManager::GetSingleton()->Initialize();
		// Initialize WeaponLockManager (keeps hands empty for NPCs the player disarmed)
		WeaponLockManager::GetSingleton()->Initialize();
		// Apply locked outfits to NPCs in current cell
		OutfitLockManager::GetSingleton()->OnPostLoadGame();
		// ...and look at anyone whose hands are meant to stay empty
		WeaponLockManager::GetSingleton()->Sweep();

		// Notify user if VR interactivity is unavailable due to missing dependency
		if (InputManager::GetSingleton()->IsSkyrimVRToolsMissing()) {
			RE::DebugNotification("DressUp VR: SkyrimVRTools not found - VR interactions disabled");
			spdlog::warn("Displayed user notification: SkyrimVRTools missing");
		}

		// Notify user if 3DUI is missing (deferred from DataLoaded, only show once per session)
		if (g_3DUIMissing && !g_3DUIMissingNotificationShown) {
			RE::DebugNotification("DressUp VR: Requirement 3DUI is missing, disabling mod functionality");
			spdlog::warn("Displayed user notification: 3DUI missing");
			g_3DUIMissingNotificationShown = true;
		}
		break;

	case SKSE::MessagingInterface::kNewGame:
		// Initialize OutfitLockManager (registers for equip events)
		OutfitLockManager::GetSingleton()->Initialize();
		// Initialize InventoryManager (registers for equip events to return items on unequip)
		InventoryManager::GetSingleton()->Initialize();
		// Initialize WeaponLockManager (keeps hands empty for NPCs the player disarmed)
		WeaponLockManager::GetSingleton()->Initialize();

		// Notify user if VR interactivity is unavailable due to missing dependency
		if (InputManager::GetSingleton()->IsSkyrimVRToolsMissing()) {
			RE::DebugNotification("DressUp VR: SkyrimVRTools not found - VR interactions disabled");
			spdlog::warn("Displayed user notification: SkyrimVRTools missing");
		}

		// Notify user if 3DUI is missing (deferred from DataLoaded, only show once per session)
		if (g_3DUIMissing && !g_3DUIMissingNotificationShown) {
			RE::DebugNotification("DressUp VR: Requirement 3DUI is missing, disabling mod functionality");
			spdlog::warn("Displayed user notification: 3DUI missing");
			g_3DUIMissingNotificationShown = true;
		}
		break;
	}
}

SKSEPluginLoad(const SKSE::LoadInterface *skse) {
	SKSE::Init(skse);
	SetupLog();

	spdlog::info("DressUp VR loading...");

	// Load settings from INI file
	Settings::GetSingleton()->Load();

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", MessageHandler)) {
		spdlog::error("Failed to register SKSE message listener");
		return false;
	}

	// Register serialization callbacks for outfit saving/loading
	auto serialization = SKSE::GetSerializationInterface();
	if (serialization) {
		serialization->SetUniqueID('5DVR');  // 5 + DressUp VR
		serialization->SetSaveCallback(SerializationCallbacks::OnGameSave);
		serialization->SetLoadCallback(SerializationCallbacks::OnGameLoad);
		serialization->SetRevertCallback(SerializationCallbacks::OnRevert);
		spdlog::info("Registered SKSE serialization callbacks");
	} else {
		spdlog::error("Failed to get SKSE serialization interface");
	}

	spdlog::info("DressUp VR loaded successfully");
	return true;
}

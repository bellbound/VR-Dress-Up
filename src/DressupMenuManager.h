#pragma once

#include <RE/Skyrim.h>
#include <memory>
#include <Windows.h>
#include "InventoryManager.h"
#include "log.h"
#include "settings.h"
#include "api/ThreeDUIInterface001.h"
#include "dressup/UndressManager.h"
#include "dressup/ArmorModManager.h"
#include "dressup/GalleryStateManager.h"
#include "dressup/KeywordCategoryManager.h"
#include "dressup/ItemEquipHelper.h"
#include "openvr.h"
#include "InputManager.h"

// Internal narrow->wide conversion using Windows API (robust, no exceptions)
inline std::wstring ToWide(const char* str) {
    if (!str || !*str) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, -1, result.data(), len);
    return result;
}

inline std::wstring ToWide(const std::string& str) {
    return ToWide(str.c_str());
}

// What the shared row below the wheel is currently showing. Only one at a time.
enum class GalleryMode : std::uint8_t
{
    None = 0,
    Mods,       // one entry per installed mod
    Keywords    // one entry per keyword category ("Boots", "Wigs", ...)
};

class DressupMenuManager
{
public:
    static DressupMenuManager* GetSingleton()
    {
        static DressupMenuManager instance;
        return &instance;
    }

    // Initialize the 3D UI subsystems
    bool Initialize()
    {
        if (m_initialized) {
            spdlog::warn("DressupMenuManager::Initialize - Already initialized");
            return true;
        }

        // Get the 3D UI interface - this initializes all subsystems
        m_api = P3DUI::GetInterface001();
        if (!m_api) {
            spdlog::error("DressupMenuManager::Initialize - Failed to get 3D UI interface");
            return false;
        }

        // Check for interface version compatibility
        uint32_t interfaceVersion = m_api->GetInterfaceVersion();
        if (interfaceVersion > P3DUI::P3DUI_INTERFACE_VERSION) {
            spdlog::warn("DressupMenuManager::Initialize - 3DUI interface version {} is newer than expected version {}",
                interfaceVersion, P3DUI::P3DUI_INTERFACE_VERSION);
            RE::DebugNotification("[Dress Up VR] Incompatible 3DUI Version detected");
            RE::DebugNotification("[Dress Up VR] Please update Dress Up VR to the latest version");
        }

        spdlog::info("DressupMenuManager::Initialize - 3D UI subsystems initialized successfully (version {})", interfaceVersion);
        m_initialized = true;
        return true;
    }

    // One-time setup (called lazily on first ShowDressUpMenu)
    bool SetupDressUpMenu()
    {
        if (m_root) {
            return true;  // Already setup
        }

        if (!m_initialized || !m_api) {
            spdlog::error("DressupMenuManager::SetupDressUpMenu - Not initialized");
            return false;
        }

        spdlog::info("DressupMenuManager::SetupDressUpMenu - Creating menu via public API");

        // === Create Root with interaction and event handling ===
        P3DUI::RootConfig rootConfig = P3DUI::RootConfig::Default("dressup_root", "DressUpVR");
        rootConfig.interactive = true;
        rootConfig.activationButtonMask = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
        rootConfig.grabButtonMask = vr::ButtonMaskFromId(vr::k_EButton_Grip);
        rootConfig.eventCallback = &DressupMenuManager::OnEvent;

        m_root = m_api->GetOrCreateRoot(rootConfig);
        if (!m_root) {
            spdlog::error("DressupMenuManager::SetupDressUpMenu - Failed to create root");
            return false;
        }



        // === Create Item Spiral (ScrollWheel) ===
        P3DUI::ScrollWheelConfig spiralConfig = P3DUI::ScrollWheelConfig::Default("item_spiral");
        spiralConfig.itemSpacing = 8.0f;
        spiralConfig.ringSpacing = 10.0f;
        spiralConfig.firstRingSpacing = 15.0f;

        m_itemSpiral = m_api->CreateScrollWheel(spiralConfig);
        if (m_itemSpiral) {
            m_root->AddChild(m_itemSpiral);
        }

        // === Create Handle Row (using ColumnGrid with single row) ===
        P3DUI::ColumnGridConfig handleRowConfig = P3DUI::ColumnGridConfig::Default("handle_row");
        handleRowConfig.columnSpacing = 10.0f;
        handleRowConfig.numRows = 1;
        handleRowConfig.visibleWidth = 1000.0f;  // Large enough to show all items without scrolling

        m_handleRow = m_api->CreateColumnGrid(handleRowConfig);
        if (m_handleRow) {
            m_handleRow->SetOrigin(P3DUI::VerticalOrigin::Center, P3DUI::HorizontalOrigin::Center);
            m_root->AddChild(m_handleRow);
            m_handleRow->SetLocalPosition(0, 0, -10.5f);
        }

        // === Create Gallery Row (ColumnGrid for mod categories - horizontal scrolling) ===
        P3DUI::ColumnGridConfig galleryConfig = P3DUI::ColumnGridConfig::Default("gallery_row");
        galleryConfig.columnSpacing = 15.0f;
        galleryConfig.visibleWidth = 40.0f;
        galleryConfig.numRows = 1;

        m_galleryRow = m_api->CreateColumnGrid(galleryConfig);
        if (m_galleryRow) {
            m_galleryRow->SetOrigin(P3DUI::VerticalOrigin::Center, P3DUI::HorizontalOrigin::Center);
            m_root->AddChild(m_galleryRow);
            m_galleryRow->SetLocalPosition(0, 0, -20.0f);  // Below handle row
            m_galleryRow->SetVisible(false);  // Hidden by default
        }

        // === Create Info Text (context-dependent display below tool/gallery row) ===
        P3DUI::TextConfig infoTextConfig = P3DUI::TextConfig::Default("info_text");
        infoTextConfig.text = L"";  // Initially empty
        infoTextConfig.scale = 1.0f;
        infoTextConfig.facingMode = P3DUI::FacingMode::YawOnly;

        m_infoText = m_api->CreateText(infoTextConfig);
        if (m_infoText) {
            m_root->AddChild(m_infoText);
            m_infoText->SetLocalPosition(0, 0, -18.0f);  // Below handle row (moves when gallery opens)
            m_infoText->SetVisible(false);
        }

        spdlog::info("DressupMenuManager::SetupDressUpMenu - Setup complete");
        return true;
    }

    void ShowDressUpMenu(RE::Actor* targetActor, bool isLeftHand = false)
    {
        if (!m_initialized) return;
        if (!targetActor) {
            spdlog::warn("DressupMenuManager::ShowDressUpMenu - No target actor provided");
            return;
        }

        // If menu is already open, check if it's for a different NPC
        if (IsMenuOpen()) {
            RE::Actor* currentTarget = GetCurrentTargetActor();
            if (currentTarget && currentTarget != targetActor) {
                // Different NPC - close current menu and open new one
                spdlog::info("DressupMenuManager::ShowDressUpMenu - Closing menu for '{}' to open for '{}'",
                    currentTarget->GetName() ? currentTarget->GetName() : "unknown",
                    targetActor->GetName() ? targetActor->GetName() : "unknown");
                CloseMenu(false);
            } else {
                // Same NPC or no current target - just return
                return;
            }
        }

        // Check if target is dead - not allowed
        if (targetActor->IsDead()) {
            spdlog::warn("DressupMenuManager::ShowDressUpMenu - Target is dead, cannot dress up");
            return;
        }

        // Check if target is a child - not allowed
        if (targetActor->IsChild()) {
            spdlog::warn("DressupMenuManager::ShowDressUpMenu - Target is a child, cannot dress up");
            return;
        }

        // Check if target is a creature/animal (non-humanoid race)
        auto* race = targetActor->GetRace();
        if (race) {
            if (!race->GetPlayable() && !race->AllowsPickpocket()) {
                spdlog::warn("DressupMenuManager::ShowDressUpMenu - Target is a creature/animal (race: {}), cannot dress up",
                    race->GetFormEditorID() ? race->GetFormEditorID() : "unknown");
                return;
            }
        }

        // Lazy setup if needed
        if (!m_root && !SetupDressUpMenu()) {
            return;
        }


        // Store hand preference
        m_isLeftHand = isLeftHand;

        // Set up inventory manager with target actor
        auto* invMgr = InventoryManager::GetSingleton();
        invMgr->SetTargetActor(targetActor);
        invMgr->SetTargetIsPlayer(false);  // Start in NPC inventory mode

        const char* actorName = targetActor->GetName();
        spdlog::info("DressupMenuManager::ShowDressUpMenu - Opening menu for actor: {} (0x{:X}), hand: {}",
            actorName ? actorName : "unknown",
            targetActor->GetFormID(),
            isLeftHand ? "left" : "right");

        // Configure VR anchor (HMD for facing and position)
        m_root->SetVRAnchor(P3DUI::VRAnchorType::HMD);

        // End any existing grab before starting new one
        m_root->EndPositioning();

        // Start positioning on hand so items spawn at hand position
        m_root->StartPositioning(m_isLeftHand);

        // Check if trigger is already released (happens when ActorMenu had multiple items
        // and we only got the callback after user selected from selection menu)
        // In this case, immediately end positioning to snap menu in place
        auto* inputMgr = InputManager::GetSingleton();
        if (inputMgr && inputMgr->IsInitialized()) {
            uint64_t triggerMask = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
            if (!inputMgr->IsButtonPressed(triggerMask, m_isLeftHand)) {
                spdlog::info("DressupMenuManager::ShowDressUpMenu - Trigger already released, ending positioning immediately");
                m_root->EndPositioning();
            }
        }

        // Clear any existing items from item spiral
        if (m_itemSpiral) {
            m_itemSpiral->Clear();
        }

        // Populate handle row with inventory source selectors and handle
        PopulateHandleRow();

        // Populate item spiral with inventory items
        RefreshItemSpiral();

        // Initialize info text (hidden for NPC mode)
        UpdateInfoText();

        // Show menu
        m_root->SetVisible(true);

        spdlog::info("DressupMenuManager::ShowDressUpMenu - Opened with {} items",
            m_currentItemList.size());
    }

private:

    // Static event callback - routes to singleton instance
    static bool OnEvent(const P3DUI::Event* event)
    {
        return GetSingleton()->HandleEvent(event);
    }

    // Instance event handler
    bool HandleEvent(const P3DUI::Event* event)
    {
        if (!event || !event->sourceID) return false;

        std::string id(event->sourceID);

        // Handle hover events for info text updates
        if (event->type == P3DUI::EventType::HoverEnter) {
            if (id == "lock_button") {
                ShowHoverInfoText(L"When locked, the NPC's outfit can still change, but is reset to your selection on each location change");
                return true;
            }
            if (id == "gallery_toggle") {
                ShowHoverInfoText(L"Browse all Armors of the Game");
                return true;
            }
            if (id == "keyword_toggle") {
                ShowHoverInfoText(L"Browse by kind of item - boots, wigs, capes - across every mod");
                return true;
            }
        }

        if (event->type == P3DUI::EventType::HoverExit) {
            if (id == "lock_button" || id == "gallery_toggle" || id == "keyword_toggle") {
                ClearHoverInfoText();
                return true;
            }
        }

        // Handle activation events (trigger press)
        if (event->type == P3DUI::EventType::ActivateDown) {
            // Anchor handle - close menu
            if (id == "anchor_handle") {
                spdlog::info("DressupMenuManager: Anchor handle activated - closing menu");
                CloseMenu(false);
                return true;
            }

            // Inventory toggle
            if (id == "inventory_toggle") {
                OnToggleInventorySource();
                return true;
            }

            // Lock button
            if (id == "lock_button") {
                OnToggleLock();
                return true;
            }

            // Return items button
            if (id == "return_button") {
                OnReturnPlayerItems();
                return true;
            }

            // Undress/Redress button
            if (id == "undress_button") {
                OnUndressButtonClicked();
                return true;
            }

            // Gallery toggle button
            if (id == "gallery_toggle") {
                OnGalleryToggleClicked();
                return true;
            }

            // Keyword category gallery toggle button
            if (id == "keyword_toggle") {
                OnKeywordGalleryToggleClicked();
                return true;
            }

            // Mod selection (category_0, category_1, etc.)
            if (id.rfind("category_", 0) == 0) {
                try {
                    int categoryIndex = std::stoi(id.substr(9));
                    OnCategorySelected(categoryIndex);
                    return true;
                } catch (...) {
                    spdlog::warn("DressupMenuManager: Invalid category ID: {}", id);
                }
            }

            // Keyword category selection (kwcat_0, kwcat_1, etc.)
            if (id.rfind("kwcat_", 0) == 0) {
                try {
                    int categoryIndex = std::stoi(id.substr(6));
                    OnKeywordCategorySelected(categoryIndex);
                    return true;
                } catch (...) {
                    spdlog::warn("DressupMenuManager: Invalid keyword category ID: {}", id);
                }
            }

            // Item selection (item_0, item_1, etc.)
            if (id.rfind("item_", 0) == 0) {
                try {
                    int itemIndex = std::stoi(id.substr(5));
                    OnItemSelected(itemIndex);
                    return true;
                } catch (...) {
                    spdlog::warn("DressupMenuManager: Invalid item ID: {}", id);
                }
            }
        }

        return false;
    }

    // Populate handle row with inventory toggle, filter toggle, and lock button
    void PopulateHandleRow()
    {
        if (!m_handleRow || !m_api) return;

        m_handleRow->Clear();

        auto* invMgr = InventoryManager::GetSingleton();

        // Get NPC name for toggle button text
        RE::Actor* targetActor = invMgr->GetTargetActor();
        const char* npcName = targetActor ? targetActor->GetName() : "NPC";

        // Inventory source toggle button
        std::wstring toggleTooltip;
        if (invMgr->IsTargetPlayer()) {
            toggleTooltip = ToWide(npcName);
        } else {
            toggleTooltip = ToWide(RE::PlayerCharacter::GetSingleton()->GetDisplayFullName());
        }
        std::string inventoryIcon = invMgr->IsTargetPlayer()
            ? "textures\\VRDressup\\player.dds"
            : "textures\\VRDressup\\npc.dds";

        P3DUI::ElementConfig invToggleConfig = P3DUI::ElementConfig::Default("inventory_toggle");
        invToggleConfig.texturePath = inventoryIcon.c_str();
        invToggleConfig.tooltip = toggleTooltip.c_str();
        invToggleConfig.scale = 1.2f;
        invToggleConfig.facingMode = P3DUI::FacingMode::None;

        auto* inventoryToggle = m_api->CreateElement(invToggleConfig);
        if (inventoryToggle) {
            m_handleRow->AddChild(inventoryToggle);
        }

        // Lock toggle button - allows manual lock/unlock
        bool isLocked = invMgr->IsNpcLocked();
        std::wstring lockTooltip = isLocked
            ? L"Unlock (" + ToWide(npcName) + L" is Locked)"
            : L"Lock (" + ToWide(npcName) + L" is Unlocked)";
        std::string lockIcon = isLocked
            ? "textures\\VRDressup\\lock_highlight.dds"
            : "textures\\VRDressup\\unlock.dds";

        P3DUI::ElementConfig lockConfig = P3DUI::ElementConfig::Default("lock_button");
        lockConfig.texturePath = lockIcon.c_str();
        lockConfig.tooltip = lockTooltip.c_str();
        lockConfig.scale = 1.2f;
        lockConfig.facingMode = P3DUI::FacingMode::None;

        auto* lockButton = m_api->CreateElement(lockConfig);
        if (lockButton) {
            m_handleRow->AddChild(lockButton);
        }

        // Undress/Redress button - cycles through undress states
        auto* undressMgr = UndressManager::GetSingleton();
        auto undressState = undressMgr->GetUndressState(targetActor);

        P3DUI::ElementConfig undressConfig = P3DUI::ElementConfig::Default("undress_button");
        undressConfig.scale = 1.2f;
        undressConfig.facingMode = P3DUI::FacingMode::None;

        switch (undressState) {
            case UndressState::Dressed:
            default:  // Fallback to dressed state for safety
                undressConfig.texturePath = "textures\\VRDressup\\undress-partial.dds";
                undressConfig.tooltip = L"Undress Armor";
                break;
            case UndressState::PartiallyUndressed:
                undressConfig.texturePath = "textures\\VRDressup\\undress-full.dds";
                undressConfig.tooltip = L"Undress Fully";
                break;
            case UndressState::FullyUndressed:
                undressConfig.texturePath = "textures\\VRDressup\\redress-full.dds";
                undressConfig.tooltip = L"Re-dress";
                break;
        }

        auto* undressButton = m_api->CreateElement(undressConfig);
        if (undressButton) {
            m_handleRow->AddChild(undressButton);
        }

        // "Return Items" button (only shown if NPC has player items)
        if (invMgr->HasPlayerItems()) {
            P3DUI::ElementConfig returnConfig = P3DUI::ElementConfig::Default("return_button");
            returnConfig.texturePath = "textures\\VRDressup\\rewind.dds";
            returnConfig.tooltip = L"Take back your items";
            returnConfig.scale = 1.2f;
            returnConfig.facingMode = P3DUI::FacingMode::None;

            auto* returnButton = m_api->CreateElement(returnConfig);
            if (returnButton) {
                m_handleRow->AddChild(returnButton);
            }
        }

        // Gallery toggle button (only shown if enabled in INI)
        if (Settings::GetSingleton()->IsModGalleryEnabled()) {
            bool showingMods = (m_galleryMode == GalleryMode::Mods);
            P3DUI::ElementConfig galleryConfig = P3DUI::ElementConfig::Default("gallery_toggle");
            galleryConfig.texturePath = showingMods
                ? "textures\\VRDressup\\gallery_highlight.dds"
                : "textures\\VRDressup\\gallery.dds";
            galleryConfig.tooltip = showingMods ? L"Close Gallery" : L"Mod Gallery";
            galleryConfig.scale = 1.2f;
            galleryConfig.facingMode = P3DUI::FacingMode::None;

            auto* galleryButton = m_api->CreateElement(galleryConfig);
            if (galleryButton) {
                m_handleRow->AddChild(galleryButton);
            }
        }

        // Keyword category gallery toggle button (only shown if enabled in INI)
        if (Settings::GetSingleton()->IsCategoryGalleryEnabled()) {
            bool showingKeywords = (m_galleryMode == GalleryMode::Keywords);
            P3DUI::ElementConfig keywordConfig = P3DUI::ElementConfig::Default("keyword_toggle");
            keywordConfig.texturePath = showingKeywords
                ? "textures\\VRDressup\\clothes_highlight.dds"
                : "textures\\VRDressup\\clothes.dds";
            keywordConfig.tooltip = showingKeywords ? L"Close Categories" : L"Browse by Category";
            keywordConfig.scale = 1.2f;
            keywordConfig.facingMode = P3DUI::FacingMode::None;

            auto* keywordButton = m_api->CreateElement(keywordConfig);
            if (keywordButton) {
                m_handleRow->AddChild(keywordButton);
            }
        }

        spdlog::info("PopulateHandleRow: isLocked={}, hasPlayerItems={}, galleryMode={}",
            isLocked, invMgr->HasPlayerItems(), static_cast<int>(m_galleryMode));
    }

    // Refresh item spiral based on current inventory source, filter mode, or active mod category
    void RefreshItemSpiral()
    {
        if (!m_itemSpiral || !m_api) return;

        m_itemSpiral->Clear();

        // Item 0: Anchor handle (Close/Grab) - always first in spiral
        P3DUI::ElementConfig anchorConfig = P3DUI::ElementConfig::Default("anchor_handle");
        anchorConfig.modelPath = "meshes\\Magic\\orbunequip.nif";
        anchorConfig.scale = 1.2f;
        anchorConfig.rotationPitch = 90.0f;
        anchorConfig.facingMode = P3DUI::FacingMode::None;
        anchorConfig.isAnchorHandle = true;

        auto* anchorHandle = m_api->CreateElement(anchorConfig);
        if (anchorHandle) {
            m_itemSpiral->AddChild(anchorHandle);
        }

        // If a gallery category is selected, show its items instead of the inventory
        if (IsShowingGalleryItems()) {
            RefreshItemSpiralWithGalleryArmor();
            return;
        }

        // Get display items from manager (respects filter mode and item count threshold)
        auto* invMgr = InventoryManager::GetSingleton();
        m_currentItemList = invMgr->GetDisplayItems();

        // Add items to item spiral (starting after anchor handle)
        for (size_t i = 0; i < m_currentItemList.size(); ++i) {
            const auto& inventoryItem = m_currentItemList[i];

            // Create unique ID for this item
            std::string itemId = "item_" + std::to_string(i);
            std::wstring itemTooltip = ToWide(inventoryItem.name);

            P3DUI::ElementConfig itemConfig = P3DUI::ElementConfig::Default(itemId.c_str());
            itemConfig.tooltip = itemTooltip.c_str();
            itemConfig.scale = 1.0f;

            // Use formID for auto model path and corrections
            if (inventoryItem.armor) {
                itemConfig.formID = inventoryItem.armor->GetFormID();
                // Set model path manually since formID auto-lookup might not work for all armor
                itemConfig.modelPath = inventoryItem.nifPath.c_str();
            } else if (inventoryItem.weapon) {
                itemConfig.formID = inventoryItem.weapon->GetFormID();
                itemConfig.modelPath = inventoryItem.nifPath.c_str();
            } else {
                itemConfig.modelPath = inventoryItem.nifPath.c_str();
            }

            auto* item = m_api->CreateElement(itemConfig);
            if (item) {
                m_itemSpiral->AddChild(item);
            }
        }

        spdlog::info("DressupMenuManager::RefreshItemSpiral - Refreshed with {} items + anchor handle", m_currentItemList.size());
    }

    // Refresh item spiral with armor from the active gallery category (mod or keyword)
    void RefreshItemSpiralWithGalleryArmor()
    {
        m_galleryArmorList = !m_activeModCategory.empty()
            ? ArmorModManager::GetSingleton()->GetArmorForMod(m_activeModCategory)
            : KeywordCategoryManager::GetSingleton()->GetItemsForCategory(m_activeKeywordCategory);

        // Sort alphabetically by name (gallery mode only - inventory uses game order)
        std::sort(m_galleryArmorList.begin(), m_galleryArmorList.end(),
            [](RE::TESObjectARMO* a, RE::TESObjectARMO* b) {
                const char* nameA = a ? a->GetFullName() : "";
                const char* nameB = b ? b->GetFullName() : "";
                return _stricmp(nameA, nameB) < 0;
            });

        // Add armor items to spiral
        for (size_t i = 0; i < m_galleryArmorList.size(); ++i) {
            auto* armor = m_galleryArmorList[i];
            if (!armor) continue;

            std::string itemId = "item_" + std::to_string(i);
            std::wstring itemTooltip = ToWide(armor->GetFullName());
            // Store model path in local variable to ensure pointer validity until CreateElement
            std::string modelPath = ItemEquipHelper::GetModelPath(armor);

            P3DUI::ElementConfig itemConfig = P3DUI::ElementConfig::Default(itemId.c_str());
            itemConfig.tooltip = itemTooltip.c_str();
            itemConfig.scale = 1.0f;
            itemConfig.formID = armor->GetFormID();
            itemConfig.modelPath = modelPath.c_str();

            auto* item = m_api->CreateElement(itemConfig);
            if (item) {
                m_itemSpiral->AddChild(item);
            }
        }

        spdlog::info("DressupMenuManager::RefreshItemSpiralWithGalleryArmor - Showing {} items from '{}'",
            m_galleryArmorList.size(),
            m_activeModCategory.empty() ? m_activeKeywordCategory : m_activeModCategory);
    }

    // Callback: Toggle between player and NPC inventory source
    void OnToggleInventorySource()
    {
        auto* invMgr = InventoryManager::GetSingleton();
        bool wasPlayer = invMgr->IsTargetPlayer();

        // Toggle the source
        invMgr->SetTargetIsPlayer(!wasPlayer);

        spdlog::info("DressupMenuManager::OnToggleInventorySource - Switched from {} to {}",
            wasPlayer ? "Player" : "NPC",
            wasPlayer ? "NPC" : "Player");

        // Refresh both the item spiral and handle row (to update toggle button text)
        RefreshItemSpiral();
        PopulateHandleRow();
        UpdateInfoText();
    }

    // Callback: Toggle NPC lock state
    void OnToggleLock()
    {
        auto* invMgr = InventoryManager::GetSingleton();
        RE::Actor* targetActor = invMgr->GetTargetActor();
        std::string npcName = targetActor ? targetActor->GetName() : "NPC";

        if (invMgr->IsNpcLocked()) {
            spdlog::info("DressupMenuManager::OnToggleLock - Unlocking '{}'", npcName);
            invMgr->UnlockNpc();
        } else {
            spdlog::info("DressupMenuManager::OnToggleLock - Locking '{}'", npcName);
            invMgr->LockNpc();
        }

        // Refresh handle row to update lock button state
        PopulateHandleRow();
    }

    // Callback: Return all player items back to player
    void OnReturnPlayerItems()
    {
        auto* invMgr = InventoryManager::GetSingleton();

        if (!invMgr->HasPlayerItems()) {
            spdlog::info("DressupMenuManager::OnReturnPlayerItems - No player items to return");
            return;
        }

        RE::Actor* targetActor = invMgr->GetTargetActor();
        std::string npcName = targetActor ? targetActor->GetName() : "NPC";

        spdlog::info("DressupMenuManager::OnReturnPlayerItems - Returning items from '{}'", npcName);

        invMgr->ReturnPlayerItems();

        // Refresh UI - items have changed
        RefreshItemSpiral();
        PopulateHandleRow();
    }

    // Callback: Undress/Redress button clicked - cycle through states
    void OnUndressButtonClicked()
    {
        auto* invMgr = InventoryManager::GetSingleton();
        RE::Actor* targetActor = invMgr->GetTargetActor();

        if (!targetActor) {
            spdlog::warn("DressupMenuManager::OnUndressButtonClicked - No target actor");
            return;
        }

        auto* undressMgr = UndressManager::GetSingleton();
        auto state = undressMgr->GetUndressState(targetActor);

        switch (state) {
            case UndressState::Dressed:
            default:  // Treat unknown states as dressed
                spdlog::info("DressupMenuManager::OnUndressButtonClicked - Partial undress '{}'",
                    targetActor->GetName());
                undressMgr->UndressPartial(targetActor);
                break;

            case UndressState::PartiallyUndressed:
                spdlog::info("DressupMenuManager::OnUndressButtonClicked - Full undress '{}'",
                    targetActor->GetName());
                undressMgr->UndressFull(targetActor);
                break;

            case UndressState::FullyUndressed:
                spdlog::info("DressupMenuManager::OnUndressButtonClicked - Re-dress '{}'",
                    targetActor->GetName());
                undressMgr->Redress(targetActor);
                break;
        }

        // Refresh UI - equipped items changed, lock state may have changed
        RefreshItemSpiral();
        PopulateHandleRow();
    }

    // Callback: Mod gallery toggle button clicked
    void OnGalleryToggleClicked()
    {
        SetGalleryMode(m_galleryMode == GalleryMode::Mods ? GalleryMode::None : GalleryMode::Mods);
    }

    // Callback: Keyword category gallery toggle button clicked
    void OnKeywordGalleryToggleClicked()
    {
        SetGalleryMode(m_galleryMode == GalleryMode::Keywords ? GalleryMode::None : GalleryMode::Keywords);
    }

    // Both galleries share one row below the wheel, so opening one closes the other.
    void SetGalleryMode(GalleryMode mode)
    {
        // The keyword categories are read from disk the first time they are asked for.
        if (mode == GalleryMode::Keywords && !KeywordCategoryManager::GetSingleton()->LoadDefinitions()) {
            RE::DebugNotification("Dress Up VR: no item categories installed");
            return;
        }

        spdlog::info("DressupMenuManager::SetGalleryMode - {} -> {}",
            static_cast<int>(m_galleryMode), static_cast<int>(mode));

        m_galleryMode = mode;

        // Leaving a category always returns the spiral to the inventory view
        m_activeModCategory.clear();
        m_activeKeywordCategory.clear();
        m_galleryArmorList.clear();
        m_categoryList.clear();
        m_keywordCategoryList.clear();
        m_galleryLoading = false;

        if (mode == GalleryMode::None) {
            if (m_galleryRow) {
                m_galleryRow->Clear();
                m_galleryRow->SetVisible(false);
            }
        } else {
            if (m_galleryRow) {
                m_galleryRow->SetVisible(true);
            }

            // Both galleries read the same armor scan; kick it off if it hasn't run yet.
            if (mode == GalleryMode::Mods) {
                auto* armorModMgr = ArmorModManager::GetSingleton();
                if (!armorModMgr->IsCacheReady()) {
                    m_galleryLoading = true;
                    armorModMgr->StartCacheBuildAsync([this](bool success) {
                        OnGalleryDataReady(GalleryMode::Mods, success);
                    });
                }
            } else {
                auto* keywordMgr = KeywordCategoryManager::GetSingleton();
                if (!keywordMgr->IsReady()) {
                    m_galleryLoading = true;
                    keywordMgr->StartBuildAsync([this](bool success) {
                        OnGalleryDataReady(GalleryMode::Keywords, success);
                    });
                }
            }
        }

        RefreshGalleryRow();
        RefreshItemSpiral();
        PopulateHandleRow();
        UpdateInfoText();
    }

    // A background scan finished. The menu may have closed or switched gallery meanwhile.
    void OnGalleryDataReady(GalleryMode mode, bool success)
    {
        if (m_galleryMode != mode) return;

        m_galleryLoading = false;

        if (!success) {
            spdlog::error("DressupMenuManager::OnGalleryDataReady - Build failed for gallery mode {}",
                static_cast<int>(mode));
        }

        // Must verify menu is still open - the row elements can be invalid if it closed
        if (!IsMenuOpen()) return;

        RefreshGalleryRow();
        PopulateHandleRow();  // Update button to remove loading state
    }

    // Refresh gallery row with whatever the current mode shows
    void RefreshGalleryRow()
    {
        // Bail out if menu is closed - UI elements may be invalid
        if (!IsMenuOpen() || !m_galleryRow || !m_api || m_galleryMode == GalleryMode::None) return;

        m_galleryRow->Clear();

        bool showingMods = (m_galleryMode == GalleryMode::Mods);

        // If the underlying scan is still running, show a loading indicator
        if (m_galleryLoading) {
            P3DUI::ElementConfig loadingConfig = P3DUI::ElementConfig::Default("loading_indicator");
            loadingConfig.texturePath = showingMods
                ? "textures\\VRDressup\\gallery.dds"
                : "textures\\VRDressup\\clothes.dds";
            loadingConfig.tooltip = showingMods ? L"Loading mods..." : L"Loading categories...";
            loadingConfig.scale = 1.2f;
            loadingConfig.facingMode = P3DUI::FacingMode::None;

            auto* loadingElement = m_api->CreateElement(loadingConfig);
            if (loadingElement) {
                m_galleryRow->AddChild(loadingElement);
            }

            float progress = showingMods
                ? ArmorModManager::GetSingleton()->GetLoadProgress()
                : KeywordCategoryManager::GetSingleton()->GetProgress();
            spdlog::info("DressupMenuManager::RefreshGalleryRow - Showing loading indicator (progress: {:.0f}%)",
                progress * 100.0f);
            return;
        }

        if (showingMods) {
            PopulateModCategories();
        } else {
            PopulateKeywordCategories();
        }

        // Reset scroll position to show the first entries
        m_galleryRow->ResetScroll();
    }

    // Fill the gallery row with one entry per installed mod
    void PopulateModCategories()
    {
        auto* armorModMgr = ArmorModManager::GetSingleton();

        // Get sorted categories from ArmorModManager
        m_categoryList = armorModMgr->GetSortedCategories();

        // Trace log: gallery mods summary
        auto* galleryState = GalleryStateManager::GetSingleton();
        spdlog::trace("=== Gallery Build: {} mods ===", m_categoryList.size());
        for (const auto& cat : m_categoryList) {
            int frontIdx = galleryState->GetFrontIndex(cat.modName);
            if (frontIdx >= 0) {
                spdlog::trace("  [FRONT #{}] {} ({} items)", frontIdx, cat.modName, cat.itemCount);
            } else {
                const char* sortReason =
                    cat.itemCount < 8 ? "small" :
                    cat.itemCount > 50 ? "large" :
                    cat.addsNPCs ? "npc-mod" : "standard";
                spdlog::trace("  [{}] {} ({} items)", sortReason, cat.modName, cat.itemCount);
            }
        }

        // Add category elements
        for (size_t i = 0; i < m_categoryList.size(); ++i) {
            const auto& category = m_categoryList[i];

            std::string categoryId = "category_" + std::to_string(i);

            // Build tooltip: mod name + item count
            std::wstring tooltip = ToWide(category.modName) + L" (" + std::to_wstring(category.itemCount) + L")";

            P3DUI::ElementConfig catConfig = P3DUI::ElementConfig::Default(categoryId.c_str());
            catConfig.tooltip = tooltip.c_str();
            catConfig.scale = 1.0f;

            // Use representative armor for the category icon
            // Store model path in local variable to ensure pointer validity until CreateElement
            std::string modelPath;
            if (category.representativeArmor) {
                catConfig.formID = category.representativeArmor->GetFormID();
                modelPath = ItemEquipHelper::GetModelPath(category.representativeArmor);
                catConfig.modelPath = modelPath.c_str();
            }

            auto* catElement = m_api->CreateElement(catConfig);
            if (catElement) {
                // Add background sphere for visual highlight
                catElement->SetBackgroundModel("meshes\\3DUI\\cloud-background-sphere.nif");
                catElement->SetBackgroundScale(20.0f);
                m_galleryRow->AddChild(catElement);
            }
        }

        spdlog::info("DressupMenuManager::PopulateModCategories - Populated with {} mods", m_categoryList.size());
    }

    // Fill the gallery row with one entry per keyword category
    void PopulateKeywordCategories()
    {
        m_keywordCategoryList = KeywordCategoryManager::GetSingleton()->GetSortedCategories();

        for (size_t i = 0; i < m_keywordCategoryList.size(); ++i) {
            const auto& category = m_keywordCategoryList[i];

            std::string categoryId = "kwcat_" + std::to_string(i);

            std::wstring tooltip = ToWide(category.name) + L" (" + std::to_wstring(category.itemCount) + L")";

            P3DUI::ElementConfig catConfig = P3DUI::ElementConfig::Default(categoryId.c_str());
            catConfig.tooltip = tooltip.c_str();
            catConfig.scale = 1.0f;

            // Preview the category with one of its own items
            // Store model path in local variable to ensure pointer validity until CreateElement
            std::string modelPath;
            if (category.representative) {
                catConfig.formID = category.representative->GetFormID();
                modelPath = ItemEquipHelper::GetModelPath(category.representative);
                catConfig.modelPath = modelPath.c_str();
            }

            auto* catElement = m_api->CreateElement(catConfig);
            if (catElement) {
                // Add background sphere for visual highlight
                catElement->SetBackgroundModel("meshes\\3DUI\\cloud-background-sphere.nif");
                catElement->SetBackgroundScale(20.0f);
                m_galleryRow->AddChild(catElement);
            }
        }

        spdlog::info("DressupMenuManager::PopulateKeywordCategories - Populated with {} categories",
            m_keywordCategoryList.size());
    }

    // Callback: Keyword category selected in gallery
    void OnKeywordCategorySelected(int categoryIndex)
    {
        if (categoryIndex < 0 || categoryIndex >= static_cast<int>(m_keywordCategoryList.size())) {
            spdlog::warn("DressupMenuManager::OnKeywordCategorySelected - Invalid index: {}", categoryIndex);
            return;
        }

        const auto& category = m_keywordCategoryList[categoryIndex];
        m_activeModCategory.clear();
        m_activeKeywordCategory = category.name;

        spdlog::info("DressupMenuManager::OnKeywordCategorySelected - Selected '{}' ({} items)",
            category.name, category.itemCount);

        RefreshItemSpiral();
        UpdateInfoText();
    }

    // Callback: Category selected in gallery
    void OnCategorySelected(int categoryIndex)
    {
        if (categoryIndex < 0 || categoryIndex >= static_cast<int>(m_categoryList.size())) {
            spdlog::warn("DressupMenuManager::OnCategorySelected - Invalid category index: {}", categoryIndex);
            return;
        }

        const auto& category = m_categoryList[categoryIndex];
        m_activeKeywordCategory.clear();
        m_activeModCategory = category.modName;

        spdlog::info("DressupMenuManager::OnCategorySelected - Selected mod '{}' ({} items)",
            category.modName, category.itemCount);

        // Mark this mod as interacted with (for future ordering)
        GalleryStateManager::GetSingleton()->PushModToFront(category.modName);

        // Refresh spiral to show mod armor
        RefreshItemSpiral();

        // Update info text to show mod name
        UpdateInfoText();
    }

public:

    void CloseMenu(bool cancelled = false)
    {
        if (!m_root || !m_root->IsVisible()) {
            return;
        }

        // End any active grab/positioning
        m_root->EndPositioning();

        // Hide menu
        m_root->SetVisible(false);

        // Clear item list
        m_currentItemList.clear();

        // Clear gallery state
        m_galleryMode = GalleryMode::None;
        m_galleryLoading = false;
        m_showingHoverText = false;
        m_activeModCategory.clear();
        m_activeKeywordCategory.clear();
        m_galleryArmorList.clear();
        m_categoryList.clear();
        m_keywordCategoryList.clear();
        if (m_galleryRow) {
            m_galleryRow->SetVisible(false);
        }

        // Reset inventory manager state (clears target actor and tracking)
        InventoryManager::GetSingleton()->Reset();

        spdlog::info("DressupMenuManager::CloseMenu - cancelled: {}", cancelled);
    }

    // Called when trigger is released - end grab to fix position
    void OnTriggerRelease()
    {
        // End the initial positioning (fixes at current position relative to HMD)
        if (m_root && m_root->IsGrabbing()) {
            m_root->EndPositioning();
            spdlog::info("DressupMenuManager::OnTriggerRelease - Ended initial positioning");
        }
    }

    bool IsMenuOpen() const
    {
        return m_root && m_root->IsVisible();
    }

    RE::Actor* GetCurrentTargetActor() const { return InventoryManager::GetSingleton()->GetTargetActor(); }
    const std::vector<DressupInventoryItem>& GetCurrentItemList() const { return m_currentItemList; }

private:
    DressupMenuManager() = default;
    ~DressupMenuManager() = default;
    DressupMenuManager(const DressupMenuManager&) = delete;
    DressupMenuManager& operator=(const DressupMenuManager&) = delete;

    // Update info text based on current context
    // Shows: category name when viewing one, "Your Inventory" when player mode, nothing when NPC mode
    void UpdateInfoText()
    {
        if (!m_infoText) return;

        // Don't update if showing hover text (hover takes precedence)
        if (m_showingHoverText) return;

        // Determine position based on gallery visibility
        float zPos = IsGalleryVisible() ? -28.0f : -18.0f;
        m_infoText->SetLocalPosition(0, 0, zPos);

        // Determine what text to show
        if (IsShowingGalleryItems()) {
            // Viewing a mod or keyword category - show its name
            const std::string& label = m_activeModCategory.empty() ? m_activeKeywordCategory : m_activeModCategory;
            m_infoText->SetText(ToWide(label).c_str());
            m_infoText->SetVisible(true);
        } else if (InventoryManager::GetSingleton()->IsTargetPlayer()) {
            // Player inventory mode
            m_infoText->SetText(L"Your Inventory");
            m_infoText->SetVisible(true);
        } else {
            // NPC inventory mode - show nothing
            m_infoText->SetText(L"");
            m_infoText->SetVisible(false);
        }
    }

    // Show hover-specific info text (takes precedence over normal context text)
    void ShowHoverInfoText(const wchar_t* text)
    {
        if (!m_infoText) return;

        m_showingHoverText = true;

        // Update position based on gallery visibility
        float zPos = IsGalleryVisible() ? -28.0f : -18.0f;
        m_infoText->SetLocalPosition(0, 0, zPos);

        m_infoText->SetText(text);
        m_infoText->SetVisible(true);
    }

    // Clear hover text and restore normal context text
    void ClearHoverInfoText()
    {
        m_showingHoverText = false;
        UpdateInfoText();
    }

    // Selection callback - delegates to InventoryManager or handles mod armor
    void OnItemSelected(int itemIndex)
    {
        auto* invMgr = InventoryManager::GetSingleton();
        bool wasLocked = invMgr->IsNpcLocked();

        // If viewing a gallery category, equip from it
        if (IsShowingGalleryItems()) {
            if (itemIndex < 0 || itemIndex >= static_cast<int>(m_galleryArmorList.size())) {
                spdlog::warn("DressupMenuManager::OnItemSelected - Invalid gallery armor index: {}", itemIndex);
                return;
            }

            // Check undress state before equip (EquipFromMod clears it internally)
            RE::Actor* targetActor = invMgr->GetTargetActor();
            bool hadUndressState = targetActor && UndressManager::GetSingleton()->HasUndressState(targetActor);

            auto* armor = m_galleryArmorList[itemIndex];
            if (armor) {
                invMgr->EquipFromMod(armor);
                spdlog::info("DressupMenuManager::OnItemSelected - Equipped gallery armor '{}'", armor->GetFullName());
            }

            // Refresh handle row if lock state or undress state changed
            bool isLocked = invMgr->IsNpcLocked();
            bool undressStateCleared = hadUndressState && targetActor &&
                !UndressManager::GetSingleton()->HasUndressState(targetActor);
            if (wasLocked != isLocked || undressStateCleared) {
                PopulateHandleRow();
            }
            return;
        }

        // Regular inventory selection
        if (itemIndex < 0 || itemIndex >= static_cast<int>(m_currentItemList.size())) {
            spdlog::warn("DressupMenuManager::OnItemSelected - Invalid index: {}", itemIndex);
            return;
        }

        const auto& selectedItem = m_currentItemList[itemIndex];
        bool wasPlayerMode = invMgr->IsTargetPlayer();

        // Delegate to inventory manager based on item type
        // Returns true if undress state was cleared
        bool undressStateCleared = false;
        if (selectedItem.armor) {
            undressStateCleared = invMgr->OnItemSelected(selectedItem.armor);
        } else if (selectedItem.weapon) {
            undressStateCleared = invMgr->OnItemSelected(selectedItem.weapon);
        } else {
            spdlog::warn("DressupMenuManager::OnItemSelected - No form");
            return;
        }

        // Refresh UI - in player mode: item was transferred out, need to refresh item list
        if (wasPlayerMode) {
            RefreshItemSpiral();
        }

        // Refresh handle row if lock state or undress state changed
        bool isLocked = invMgr->IsNpcLocked();
        if (wasLocked != isLocked || undressStateCleared) {
            PopulateHandleRow();
        }
    }

    P3DUI::Interface001* m_api = nullptr;
    P3DUI::Root* m_root = nullptr;
    P3DUI::Container* m_itemSpiral = nullptr;
    P3DUI::ScrollableContainer* m_handleRow = nullptr;   // ColumnGrid for handle buttons (single row)
    P3DUI::ScrollableContainer* m_galleryRow = nullptr;  // ColumnGrid for mod categories (horizontal scroll)
    P3DUI::Text* m_infoText = nullptr;         // Context-dependent info text

    bool m_initialized = false;
    bool m_isLeftHand = false;
    bool m_initialPlacementComplete = false;  // Prevents repositioning after initial placement
    std::vector<DressupInventoryItem> m_currentItemList;

    // Gallery state - the mod gallery and the keyword categories share one row
    GalleryMode m_galleryMode = GalleryMode::None;       // What the gallery row is showing
    bool m_galleryLoading = false;                       // Whether the backing scan is still running
    bool m_showingHoverText = false;                     // Whether info text shows hover description
    std::string m_activeModCategory;                     // Selected mod (empty = none)
    std::string m_activeKeywordCategory;                 // Selected keyword category (empty = none)
    std::vector<RE::TESObjectARMO*> m_galleryArmorList;  // Items shown for the selected category
    std::vector<ModCategoryInfo> m_categoryList;         // Mods currently in the gallery row
    std::vector<KeywordCategoryInfo> m_keywordCategoryList;  // Keyword categories currently in the gallery row

    bool IsGalleryVisible() const { return m_galleryMode != GalleryMode::None; }

    // True while the spiral shows a category's items rather than an inventory
    bool IsShowingGalleryItems() const
    {
        return !m_activeModCategory.empty() || !m_activeKeywordCategory.empty();
    }
};

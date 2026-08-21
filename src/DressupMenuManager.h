#pragma once

#include <RE/Skyrim.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <Windows.h>
#include "InventoryManager.h"
#include "dressup/OutfitSlotManager.h"
#include "log.h"
#include "settings.h"
#include "api/ThreeDUIInterface001.h"
#include "dressup/UndressManager.h"
#include "dressup/ArmorModManager.h"
#include "dressup/Backdrop.h"
#include "dressup/GalleryStateManager.h"
#include "dressup/KeywordCategoryManager.h"
#include "dressup/ItemEquipHelper.h"
#include "dressup/PapyrusBridge.h"
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

// The plate an element sits on. The gradient sphere replaces the older cloud one because
// its material is a BSEffectShaderProperty, which is the only kind 3DUI can tint - the
// cloud mesh is a lighting-shader mesh and silently ignored SetBackgroundColor.
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

        // The wheel is painted from what the actor has on, and half of that arrives after
        // the click that caused it - see OnWornStateChanged.
        InventoryManager::GetSingleton()->SetWornStateChangedCallback(
            [this]() { OnWornStateChanged(); });

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
        handleRowConfig.columnSpacing = 7.5f;   // 0.75x the original 10.0 - the tool row reads as one group
        handleRowConfig.numRows = 1;
        handleRowConfig.visibleWidth = 1000.0f;  // Large enough to show all items without scrolling

        m_handleRow = m_api->CreateColumnGrid(handleRowConfig);
        if (m_handleRow) {
            m_handleRow->SetOrigin(P3DUI::VerticalOrigin::Center, P3DUI::HorizontalOrigin::Center);
            m_root->AddChild(m_handleRow);
            m_handleRow->SetLocalPosition(0, 0, -10.5f);
        }

        // === Create Gallery Row (ColumnGrid for mod/keyword categories - horizontal scrolling) ===
        P3DUI::ColumnGridConfig galleryConfig = P3DUI::ColumnGridConfig::Default("gallery_row");
        galleryConfig.columnSpacing = 12.0f;
        galleryConfig.visibleWidth = 48.0f;   // +20% over the original 40.0; both gallery modes share this row
        galleryConfig.numRows = 1;

        m_galleryRow = m_api->CreateColumnGrid(galleryConfig);
        if (m_galleryRow) {
            m_galleryRow->SetOrigin(P3DUI::VerticalOrigin::Center, P3DUI::HorizontalOrigin::Center);
            m_root->AddChild(m_galleryRow);
            m_galleryRow->SetLocalPosition(0, 0, -20.0f);  // Below handle row
            m_galleryRow->SetVisible(false);  // Hidden by default
        }

        // === Create Outfit Row (ColumnGrid of saved looks - same shape as the gallery row) ===
        P3DUI::ColumnGridConfig outfitConfig = P3DUI::ColumnGridConfig::Default("outfit_row");
        // Tighter and wider than the gallery row: its plates are icons and numbered
        // snapshots rather than armour to study, so more of them on screen beats bigger
        // ones. -10% spacing against the gallery's 12.0, +10% visible width against its
        // 48.0, and Backdrop::kOutfitScale sizes the plate to sit inside the new gap.
        outfitConfig.columnSpacing = 10.8f;
        outfitConfig.visibleWidth = 52.8f;
        outfitConfig.numRows = 1;

        m_outfitRow = m_api->CreateColumnGrid(outfitConfig);
        if (m_outfitRow) {
            m_outfitRow->SetOrigin(P3DUI::VerticalOrigin::Center, P3DUI::HorizontalOrigin::Center);
            m_root->AddChild(m_outfitRow);
            m_outfitRow->SetLocalPosition(0, 0, -20.0f);  // LayoutRows moves it
            m_outfitRow->SetVisible(false);
        }

        // === Create Editing Text (between the wheel and the tool row) ===
        // The one thing that says a click is about to change a saved outfit, so it sits
        // where the eye already is rather than under the rows with the info text.
        P3DUI::TextConfig editingConfig = P3DUI::TextConfig::Default("editing_text");
        editingConfig.text = L"";
        editingConfig.scale = 1.0f;
        editingConfig.facingMode = P3DUI::FacingMode::YawOnly;

        m_editingText = m_api->CreateText(editingConfig);
        if (m_editingText) {
            m_root->AddChild(m_editingText);
            m_editingText->SetLocalPosition(0, 0, kEditingTextZ);
            m_editingText->SetVisible(false);
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
                spdlog::debug("DressupMenuManager::ShowDressUpMenu - Trigger already released, ending positioning immediately");
                m_root->EndPositioning();
            }
        }

        // Clear any existing items from item spiral. PopulateHandleRow runs before the
        // spiral is refilled, so the vector has to be emptied here rather than left to
        // RefreshItemSpiral - see the note there.
        m_itemElements.clear();
        if (m_itemSpiral) {
            m_itemSpiral->Clear();
        }

        // Populate handle row with inventory source selectors and handle
        PopulateHandleRow();

        // Populate item spiral with inventory items
        RefreshItemSpiral();

        // Both rows start closed; put the info text where that leaves it
        LayoutRows();

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

    // A repaint that something off the main thread asked for, run now that we are on it.
    //
    // The rows may only be touched from the game's main thread - 3DUI says so in its
    // header, under Thread Safety - and the SKSE task queue this used to post to is not
    // drained there. Doing it anyway is what tore the wheel apart mid-rebuild on
    // 2026-08-20; guarding the vectors with a mutex instead only moved the failure, since
    // the main thread waits on that drain and a lock between them deadlocks.
    //
    // So the repaint waits for the callback below, which 3DUI documents as main-thread and
    // fires on every hover and every press. A repaint is cosmetic and idempotent, so the
    // worst a still hand can cost is a plate that stays one state stale until it moves -
    // which is exactly what the wheel did before any of this existed.
    void DrainPendingHighlight()
    {
        if (!m_highlightDirty.exchange(false)) return;
        if (!IsMenuOpen()) return;

        RefreshItemHighlight();
    }

    // Instance event handler
    bool HandleEvent(const P3DUI::Event* event)
    {
        DrainPendingHighlight();

        if (!event || !event->sourceID) return false;

        std::string id(event->sourceID);

        // Handle hover events for info text updates
        if (event->type == P3DUI::EventType::HoverEnter) {
            if (id == "outfits_toggle") {
                ShowHoverInfoText(L"Saved outfits for this NPC");
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
            if (id == "outfits_toggle" || id == "gallery_toggle" || id == "keyword_toggle") {
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

            // Undress/Redress button
            if (id == "undress_button") {
                OnUndressButtonClicked();
                return true;
            }

            // Outfit row toggle and its buttons
            if (id == "outfits_toggle") {
                SetOutfitRowVisible(!m_outfitRowVisible);
                return true;
            }
            if (id == "outfit_default") {
                OnOutfitDefaultClicked();
                return true;
            }
            if (id == "outfit_save") {
                OnOutfitSaveClicked();
                return true;
            }
            if (id == "outfit_delete") {
                OnOutfitDeleteClicked();
                return true;
            }
            if (id.rfind("outfit_", 0) == 0) {
                try {
                    OnOutfitSelected(std::stoi(id.substr(7)));
                    return true;
                } catch (...) {
                    spdlog::warn("DressupMenuManager: Invalid outfit ID: {}", id);
                }
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

        // Undress/Redress button - cycles through undress states. Follows the inventory
        // toggle: in player mode it is the player who gets undressed, which is the whole
        // reason it follows - getting your own gear off and back on is otherwise a trip
        // through the pause menu, one slot at a time.
        RE::Actor* undressActor = UndressTarget();
        auto* undressMgr = UndressManager::GetSingleton();
        auto undressState = undressMgr->GetUndressState(undressActor);
        const std::wstring undressWho =
            invMgr->IsTargetPlayer() ? L" (You)" : L"";

        P3DUI::ElementConfig undressConfig = P3DUI::ElementConfig::Default("undress_button");
        undressConfig.scale = 1.2f;
        undressConfig.facingMode = P3DUI::FacingMode::None;

        std::wstring undressTooltip;
        switch (undressState) {
            case UndressState::Dressed:
            default:  // Fallback to dressed state for safety
                undressConfig.texturePath = "textures\\VRDressup\\undress-partial.dds";
                undressTooltip = L"Undress Armor" + undressWho;
                break;
            case UndressState::PartiallyUndressed:
                undressConfig.texturePath = "textures\\VRDressup\\undress-full.dds";
                undressTooltip = L"Undress Fully" + undressWho;
                break;
            case UndressState::FullyUndressed:
                undressConfig.texturePath = "textures\\VRDressup\\redress-full.dds";
                undressTooltip = L"Re-dress" + undressWho;
                break;
        }
        undressConfig.tooltip = undressTooltip.c_str();

        auto* undressButton = m_api->CreateElement(undressConfig);
        if (undressButton) {
            m_handleRow->AddChild(undressButton);
        }

        // Outfits toggle - opens the row of saved looks below this one
        {
            P3DUI::ElementConfig outfitsConfig = P3DUI::ElementConfig::Default("outfits_toggle");
            outfitsConfig.texturePath = m_outfitRowVisible
                ? "textures\\VRDressup\\outfits_highlight.dds"
                : "textures\\VRDressup\\outfits.dds";
            outfitsConfig.tooltip = m_outfitRowVisible ? L"Close Outfits" : L"Outfits";
            outfitsConfig.scale = 1.2f;
            outfitsConfig.facingMode = P3DUI::FacingMode::None;

            auto* outfitsButton = m_api->CreateElement(outfitsConfig);
            if (outfitsButton) {
                m_handleRow->AddChild(outfitsButton);
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

        spdlog::debug("PopulateHandleRow: outfitRow={}, galleryMode={}",
            m_outfitRowVisible, static_cast<int>(m_galleryMode));
    }

    // Refresh item spiral based on current inventory source, filter mode, or active mod category
    void RefreshItemSpiral()
    {
        if (!m_itemSpiral || !m_api) return;

        // Forget the pointers before the container destroys what they point at. Clear()
        // tombstones every child, so between the two calls the vector names dead elements -
        // and RefreshItemHighlight, which runs from a queued task, walks exactly this
        // vector. Done in this order there is no moment where it can find one.
        m_itemElements.clear();
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

            // Recorded even when creation failed, so m_itemElements stays index-aligned
            // with the list it was built from: RefreshItemHighlight walks the two in step,
            // and one missing element would shift every plate after it onto the wrong
            // item. Backdrop::Apply ignores a null element.
            Backdrop::ApplyItem(item, IsItemWorn(inventoryItem));
            m_itemElements.push_back(item);
            if (item) {
                m_itemSpiral->AddChild(item);
            }
        }

        spdlog::debug("DressupMenuManager::RefreshItemSpiral - Refreshed with {} items + anchor handle", m_currentItemList.size());
    }

    // Is this wheel entry something the actor whose inventory we are showing has on?
    //
    // Asked of the intent rather than of the engine - see ItemEquipHelper's Pending note.
    // An equip is queued and does not reach the actor for another frame or two, and a
    // Devious Devices removal takes far longer than that, so reading the worn flag straight
    // back after a click would leave the plate showing the state the player just left.
    bool IsItemWorn(const DressupInventoryItem& item) const
    {
        RE::Actor* source = InventoryManager::GetSingleton()->GetSourceActor();
        if (!source) return false;

        if (item.armor) return ItemEquipHelper::IsArmorEquippedOrPending(source, item.armor);
        if (item.weapon) return ItemEquipHelper::IsWeaponEquipped(source, item.weapon);
        return false;
    }

    // Same question for a gallery plate, in either gallery - mod categories and keyword
    // categories share one spiral and one click path.
    //
    // Asked of the target actor rather than of GetSourceActor: the wheel may be listing
    // the player's own pack, but a gallery pick is conjured onto the NPC being dressed
    // either way, which is what InventoryManager::EquipFromMod does with it.
    bool IsGalleryArmorWorn(RE::TESObjectARMO* armor) const
    {
        RE::Actor* target = InventoryManager::GetSingleton()->GetTargetActor();
        if (!armor || !target) return false;

        return ItemEquipHelper::IsArmorEquippedOrPending(target, armor);
    }

    // Re-tint the wheel in place after a click, the way RefreshCategoryHighlight does for
    // the gallery row. Rebuilding the spiral instead would drop every element and recreate
    // it - dozens of models re-read from disk - to change one plate's colour.
    void RefreshItemHighlight()
    {
        // Parenthesised: Windows.h defines min as a macro.
        if (IsShowingGalleryItems()) {
            const size_t count = (std::min)(m_itemElements.size(), m_galleryArmorList.size());
            for (size_t i = 0; i < count; ++i) {
                Backdrop::ApplyItem(m_itemElements[i], IsGalleryArmorWorn(m_galleryArmorList[i]));
            }
            return;
        }

        const size_t count = (std::min)(m_itemElements.size(), m_currentItemList.size());
        for (size_t i = 0; i < count; ++i) {
            Backdrop::ApplyItem(m_itemElements[i], IsItemWorn(m_currentItemList[i]));
        }
    }

    // A gear change has landed on an actor the wheel is showing.
    //
    // The repaint a click does is a prediction: the equip it issued is queued, and the
    // removal the engine does to make room for it has not happened yet, so the plate of
    // whatever was in that biped slot is painted one state out of date and stays that way
    // until something repaints it again - which, before this, was the next click. That is
    // the "the orbs only catch up when I equip the next thing" of it.
    //
    // Arrives on the game thread from inside the engine's own equip processing, and an
    // outfit reapply announces dozens in a row, so the repaint is coalesced onto a single
    // task rather than run here: one repaint per drain no matter how many events fed it.
    // Arrives on whatever thread the engine raised the equip on, so it does no more than
    // raise a flag - see DrainPendingHighlight for why the repaint cannot happen here.
    // The flag coalesces the burst for free: an outfit reapply sets it dozens of times and
    // the drain repaints once.
    void OnWornStateChanged()
    {
        if (!IsMenuOpen()) return;
        m_highlightDirty = true;
    }

    // One repaint just past the window a queued equip is believed for.
    //
    // An equip the engine decides against - the slot is held by something it will not take
    // off, or the item is not really there - lands nowhere and announces nothing, so
    // OnWornStateChanged never hears about it. The plate was lit from the pending marker
    // the click wrote; that marker expires after ItemEquipHelper's grace, and this is what
    // turns the plate back into an offer once it has.
    void ScheduleSettleRepaint()
    {
        PapyrusBridge::RunAfterMs(kSettleRepaintMs, [this]() { m_highlightDirty = true; });
    }

    // Refresh item spiral with armor from the active gallery category (mod or keyword)
    void RefreshItemSpiralWithGalleryArmor()
    {
        m_galleryArmorList = !m_activeModCategory.empty()
            ? ArmorModManager::GetSingleton()->GetArmorForMod(m_activeModCategory)
            : KeywordCategoryManager::GetSingleton()->GetItemsForCategory(m_activeKeywordCategory);

        // The categories are cached once for the whole game, so the actor being dressed is
        // applied here. Same filter the count on the category icon was narrowed by, so the
        // spiral holds exactly as many items as the icon promised.
        NarrowToTarget(m_galleryArmorList);

        // Sort alphabetically by name (gallery mode only - inventory uses game order)
        std::sort(m_galleryArmorList.begin(), m_galleryArmorList.end(),
            [](RE::TESObjectARMO* a, RE::TESObjectARMO* b) {
                const char* nameA = a ? a->GetFullName() : "";
                const char* nameB = b ? b->GetFullName() : "";
                return _stricmp(nameA, nameB) < 0;
            });

        // Then push the base game's own items behind every mod-added one, each half still
        // alphabetical. A category like Boots is otherwise led by the vanilla set the player
        // has worn since level one, and whatever they installed the mod for sits pages away.
        std::stable_partition(m_galleryArmorList.begin(), m_galleryArmorList.end(),
            [](RE::TESObjectARMO* armor) { return !IsVanillaArmor(armor); });

        // Add armor items to spiral
        for (size_t i = 0; i < m_galleryArmorList.size(); ++i) {
            auto* armor = m_galleryArmorList[i];
            if (!armor) {
                // Placeholder, so m_itemElements stays index-aligned with the armour list
                // the clicks and the highlight both index by.
                m_itemElements.push_back(nullptr);
                continue;
            }

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

            // Gallery plates carry the same worn state the inventory wheel does. They did
            // not before, on the assumption that a gallery is a catalogue of things the
            // actor does not have - but a click puts the piece on and leaves it in the
            // list, so without this the one plate you had just used looked no different
            // from the 559 you had not, and there was no way to tell what a second click
            // would take off again.
            Backdrop::ApplyItem(item, IsGalleryArmorWorn(armor));
            m_itemElements.push_back(item);
            if (item) {
                m_itemSpiral->AddChild(item);
            }
        }

        spdlog::debug("DressupMenuManager::RefreshItemSpiralWithGalleryArmor - Showing {} items from '{}'",
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

        // The galleries browse the whole game's wardrobe onto the NPC, which is the one
        // thing player mode is not for - leaving one open just buries your own pack under
        // a category you cannot use from here. Closing it refreshes everything below.
        if (!wasPlayer && IsGalleryVisible()) {
            SetGalleryMode(GalleryMode::None);
            return;
        }

        // Refresh both the item spiral and handle row (to update toggle button text)
        RefreshItemSpiral();
        PopulateHandleRow();
        UpdateInfoText();
    }

    // === Outfit row ===
    //
    // The row of an NPC's saved looks: NPC Default first, then one plate per saved outfit,
    // then Save; Delete on the far left whenever a saved outfit is the one on the NPC.
    // The plate that is lit is the look they have on, read off the lock each time rather
    // than remembered - see OutfitSlotManager. While the row is open and a saved outfit
    // is the one on the NPC, every edit made in the wheel is written into that outfit
    // (AfterGearEdit); close the row and the look stays but the edits stop.

    void SetOutfitRowVisible(bool visible)
    {
        m_outfitRowVisible = visible;
        m_outfitDeleteArmed = false;
        m_switchArmed = kNothingArmed;

        if (visible) {
            m_editingSlot = OutfitSlotManager::GetSingleton()->Worn(GetCurrentTargetActor());
            UpdateBackdropScheme();  // before the row is built - see AfterOutfitChange
            RefreshOutfitRow();      // shows the row once it is full
        } else {
            m_editingSlot.reset();

            ForgetOutfitRowElements();
            if (m_outfitRow) {
                m_outfitRow->Clear();
                m_outfitRow->SetVisible(false);
            }
        }

        spdlog::info("DressupMenuManager::SetOutfitRowVisible - {}{}", visible ? "open" : "closed",
            (visible && m_editingSlot) ? fmt::format(", editing outfit {}", *m_editingSlot) : "");

        LayoutRows();
        UpdateEditingText();
        UpdateInfoText();
        PopulateHandleRow();  // the toggle's icon
    }

    // The row's live elements are destroyed by Clear(); drop every pointer into it first.
    void ForgetOutfitRowElements()
    {
        m_outfitSlotList.clear();
        m_outfitElements.clear();
        m_outfitDefaultElement = nullptr;
        m_outfitDeleteButton = nullptr;
    }

    // Rebuild the outfit row from the store. Resets the scroll position, so the in-place
    // RefreshOutfitHighlight is preferred wherever only the tint changes.
    void RefreshOutfitRow()
    {
        if (!IsMenuOpen() || !m_outfitRow || !m_api || !m_outfitRowVisible) return;

        // Filled hidden and shown at the end. An element created into a visible container
        // spawns at the container's centre and flies out to its slot; created into a hidden
        // one it skips the spawn and is simply there when the row appears.
        m_outfitRow->SetVisible(false);
        ForgetOutfitRowElements();  // before Clear - see RefreshItemSpiral
        m_outfitRow->Clear();

        auto* slots = OutfitSlotManager::GetSingleton();
        RE::Actor* npc = GetCurrentTargetActor();
        m_outfitSlotList = slots->List(npc);

        const std::optional<std::uint32_t> worn = HighlightedOutfit();

        // Delete - only a saved outfit that is on the NPC can be deleted from here, so the
        // button is only there when one is.
        if (worn) {
            P3DUI::ElementConfig deleteConfig = P3DUI::ElementConfig::Default("outfit_delete");
            deleteConfig.texturePath = m_outfitDeleteArmed
                ? "textures\\VRDressup\\question.dds"
                : "textures\\VRDressup\\trash.dds";
            const std::wstring deleteTooltip = DeleteTooltip(*worn);
            deleteConfig.tooltip = deleteTooltip.c_str();
            deleteConfig.scale = 1.2f;
            deleteConfig.facingMode = P3DUI::FacingMode::None;

            m_outfitDeleteButton = m_api->CreateElement(deleteConfig);
            if (m_outfitDeleteButton) {
                Backdrop::ApplyOutfitPlate(m_outfitDeleteButton, Backdrop::OutfitPlate::Idle);
                m_outfitRow->AddChild(m_outfitDeleteButton);
            }
        }

        // NPC Default - the game's own outfit, i.e. unlocked
        {
            P3DUI::ElementConfig defaultConfig = P3DUI::ElementConfig::Default("outfit_default");
            defaultConfig.texturePath = m_switchArmed == kDefaultPlate
                ? "textures\\VRDressup\\question.dds"
                : "textures\\VRDressup\\npc.dds";
            const std::wstring defaultTooltip = SwitchTooltip(kDefaultPlate);
            defaultConfig.tooltip = defaultTooltip.c_str();
            defaultConfig.scale = 1.2f;
            defaultConfig.facingMode = P3DUI::FacingMode::None;

            m_outfitDefaultElement = m_api->CreateElement(defaultConfig);
            if (m_outfitDefaultElement) {
                SetOutfitLabel(m_outfitDefaultElement, L"Default");
                Backdrop::ApplyOutfitPlate(m_outfitDefaultElement,
                    slots->IsDefault(npc) ? Backdrop::OutfitPlate::Worn : Backdrop::OutfitPlate::Idle);
                m_outfitRow->AddChild(m_outfitDefaultElement);
            }
        }

        // Save - a new outfit from whatever they have on. Next to NPC Default rather than
        // after the outfits: the two of them are what the row *does*, and pushing Save past
        // every saved look meant scrolling to reach the button that makes one.
        {
            P3DUI::ElementConfig saveConfig = P3DUI::ElementConfig::Default("outfit_save");
            saveConfig.texturePath = "textures\\VRDressup\\save.dds";
            saveConfig.tooltip = L"Save current look as a new outfit";
            saveConfig.scale = 1.2f;
            saveConfig.facingMode = P3DUI::FacingMode::None;

            auto* saveButton = m_api->CreateElement(saveConfig);
            if (saveButton) {
                Backdrop::ApplyOutfitPlate(saveButton, Backdrop::OutfitPlate::Idle);
                m_outfitRow->AddChild(saveButton);
            }
        }

        // One plate per saved outfit, numbered by position. The preview pieces are picked
        // for the row at once so two outfits sharing a cuirass do not draw the same plate.
        const auto previews = slots->Representatives(npc, m_outfitSlotList);

        for (size_t i = 0; i < m_outfitSlotList.size(); ++i) {
            const std::string elementId = "outfit_" + std::to_string(i);
            const std::wstring number = std::to_wstring(i + 1);
            const std::wstring tooltip = SwitchTooltip(static_cast<int>(i));

            P3DUI::ElementConfig plateConfig = P3DUI::ElementConfig::Default(elementId.c_str());
            plateConfig.tooltip = tooltip.c_str();
            plateConfig.facingMode = P3DUI::FacingMode::None;

            // Shown as one of its own pieces, the way a category is. An empty outfit - an
            // undressed NPC, saved - has no piece to show and gets the undressed figure.
            std::string modelPath;
            if (auto* armor = previews[i]) {
                plateConfig.scale = 1.0f;
                plateConfig.formID = armor->GetFormID();
                modelPath = ItemEquipHelper::GetModelPath(armor);
                plateConfig.modelPath = modelPath.c_str();
            } else {
                plateConfig.scale = 1.2f;
                plateConfig.texturePath = "textures\\VRDressup\\undress-full.dds";
            }

            auto* plate = m_api->CreateElement(plateConfig);
            if (plate) {
                SetOutfitLabel(plate, number.c_str());
                m_outfitRow->AddChild(plate);
            }
            // Recorded even when creation failed, so the vector stays index-aligned with
            // m_outfitSlotList; the highlight walks the two in step.
            m_outfitElements.push_back(plate);
        }

        RefreshOutfitHighlight();
        m_outfitRow->ResetScroll();
        m_outfitRow->SetVisible(true);

        spdlog::debug("DressupMenuManager::RefreshOutfitRow - {} saved outfit(s), {} on",
            m_outfitSlotList.size(), worn ? std::to_string(*worn) : "none");
    }

    // The number under a plate. Smaller and closer than the default label, so it stays
    // clear of the row below when the gallery is open under this one.
    static void SetOutfitLabel(P3DUI::Element* element, const wchar_t* text)
    {
        element->SetLabelText(text);
        element->SetLabelTextScale(0.7f);
        element->SetLabelOffset(0.0f, 0.0f, -6.5f);
    }

    // Which saved outfit the row should light up. The slot being edited, while it still
    // matches the lock - so that of two identical snapshots the one the player picked stays
    // lit, not the older one the store happens to list first.
    std::optional<std::uint32_t> HighlightedOutfit() const
    {
        const auto worn = OutfitSlotManager::GetSingleton()->Worn(GetCurrentTargetActor());
        if (worn && m_editingSlot) return m_editingSlot;
        return worn;
    }

    // Re-tint the row in place.
    void RefreshOutfitHighlight()
    {
        auto* slots = OutfitSlotManager::GetSingleton();
        RE::Actor* npc = GetCurrentTargetActor();
        const auto worn = HighlightedOutfit();

        if (m_outfitDefaultElement) {
            Backdrop::ApplyOutfitPlate(m_outfitDefaultElement,
                m_switchArmed == kDefaultPlate ? Backdrop::OutfitPlate::Armed
                : slots->IsDefault(npc)        ? Backdrop::OutfitPlate::Worn
                                               : Backdrop::OutfitPlate::Idle);
        }

        const size_t count = (std::min)(m_outfitElements.size(), m_outfitSlotList.size());
        for (size_t i = 0; i < count; ++i) {
            const bool isWorn = worn && m_outfitSlotList[i].id == *worn;

            // Red means the same thing on both prompts: press again and something goes.
            Backdrop::ApplyOutfitPlate(m_outfitElements[i],
                m_switchArmed == static_cast<int>(i)   ? Backdrop::OutfitPlate::Armed
                : !isWorn                              ? Backdrop::OutfitPlate::Idle
                : m_outfitDeleteArmed                  ? Backdrop::OutfitPlate::Armed
                                                       : Backdrop::OutfitPlate::Worn);
        }
    }

    // "Outfit N" for a slot id, by its position in the row.
    std::wstring OutfitDisplayName(std::uint32_t id) const
    {
        for (size_t i = 0; i < m_outfitSlotList.size(); ++i) {
            if (m_outfitSlotList[i].id == id) {
                return L"Outfit " + std::to_wstring(i + 1);
            }
        }
        return L"Outfit";
    }

    std::wstring DeleteTooltip(std::uint32_t id) const
    {
        return (m_outfitDeleteArmed ? L"Press again to delete " : L"Delete ") + OutfitDisplayName(id);
    }

    // Is the NPC in a look that no saved outfit holds and nothing will bring back?
    //
    // They are locked to it, so it survives the menu closing, but it lives nowhere the row
    // can reach - and putting an outfit on replaces it with no way back. That is worth a
    // second press, the same as a delete: both throw away something the player made.
    bool NeedsSwitchConfirmation() const
    {
        auto* slots = OutfitSlotManager::GetSingleton();
        RE::Actor* npc = GetCurrentTargetActor();
        if (!npc) return false;

        return !slots->IsDefault(npc) && !slots->Worn(npc);
    }

    // `plate` is an outfit's row index, or kDefaultPlate.
    std::wstring SwitchTooltip(int plate) const
    {
        const std::wstring name = plate == kDefaultPlate
            ? L"NPC Default"
            : OutfitDisplayName(plate >= 0 && plate < static_cast<int>(m_outfitSlotList.size())
                ? m_outfitSlotList[plate].id : 0);

        if (m_switchArmed == plate) {
            return L"Press again for " + name + L" - the look they have on is not saved";
        }
        return name;
    }

    // Take a switch back out of its confirmation state.
    void DisarmOutfitSwitch()
    {
        if (m_switchArmed == kNothingArmed) return;
        const int was = m_switchArmed;
        m_switchArmed = kNothingArmed;

        if (was == kDefaultPlate && m_outfitDefaultElement) {
            m_outfitDefaultElement->SetTexture("textures\\VRDressup\\npc.dds");
        }
        RefreshOutfitHighlight();
        UpdateEditingText();
    }

    // Every prompt the row can be showing, cancelled. Any press that is not the second half
    // of the prompt it belongs to lands here first.
    void DisarmOutfitPrompts()
    {
        DisarmOutfitDelete();
        DisarmOutfitSwitch();
    }

    // The first press of a switch that would throw an unsaved look away. True when the
    // caller should stop and wait for the second press.
    bool ArmSwitchIfNeeded(int plate)
    {
        if (m_switchArmed == plate) {
            m_switchArmed = kNothingArmed;   // the second press: go ahead
            return false;
        }
        if (!NeedsSwitchConfirmation()) return false;

        DisarmOutfitSwitch();
        m_switchArmed = plate;

        if (plate == kDefaultPlate && m_outfitDefaultElement) {
            m_outfitDefaultElement->SetTexture("textures\\VRDressup\\question.dds");
        }
        RetintSwitchTooltips();
        RefreshOutfitHighlight();
        UpdateEditingText();
        return true;
    }

    // The armed plate says what a second press costs; every other plate says its own name.
    void RetintSwitchTooltips()
    {
        if (m_outfitDefaultElement) {
            m_outfitDefaultElement->SetTooltip(SwitchTooltip(kDefaultPlate).c_str());
        }
        for (size_t i = 0; i < m_outfitElements.size(); ++i) {
            if (m_outfitElements[i]) {
                m_outfitElements[i]->SetTooltip(SwitchTooltip(static_cast<int>(i)).c_str());
            }
        }
    }

    // Is the wheel writing into a saved outfit right now?
    bool IsEditingOutfit() const
    {
        return m_outfitRowVisible && m_editingSlot && HighlightedOutfit() == m_editingSlot;
    }

    // The line between the wheel and the tool row. It has three things to say, in order of
    // urgency: a prompt is waiting, the wheel is writing into a saved outfit, or the look
    // on the NPC belongs to no outfit and will not survive the next switch.
    void UpdateEditingText()
    {
        if (!m_editingText) return;

        std::wstring text;
        if (m_outfitRowVisible) {
            if (m_switchArmed != kNothingArmed) {
                text = L"The look they have on is not saved - press again to lose it";
            } else if (IsEditingOutfit()) {
                text = L"Editing " + OutfitDisplayName(*m_editingSlot);
            } else if (NeedsSwitchConfirmation()) {
                text = L"Unsaved look - press save to keep it";
            }
        }

        m_editingText->SetText(text.c_str());
        m_editingText->SetVisible(!text.empty());

        UpdateBackdropScheme();
    }

    // Editing a saved outfit turns every plate in the menu warm.
    //
    // The text above says the same thing, but the player's eyes are on the wheel they are
    // clicking, not on a line above it - and this is the one mode where a click changes
    // something that outlives the session. Repainting is three in-place re-tints; nothing
    // is rebuilt, and rows built while the scheme is on pick it up from Backdrop::Current.
    void UpdateBackdropScheme()
    {
        const auto wanted = IsEditingOutfit() ? Backdrop::Scheme::Editing : Backdrop::Scheme::Normal;
        if (!Backdrop::SetScheme(wanted)) return;

        RefreshItemHighlight();
        RefreshCategoryHighlight();
        RefreshOutfitHighlight();
    }

    // Take the delete button back out of its confirmation state.
    void DisarmOutfitDelete()
    {
        if (!m_outfitDeleteArmed) return;
        m_outfitDeleteArmed = false;

        if (m_outfitDeleteButton) {
            m_outfitDeleteButton->SetTexture("textures\\VRDressup\\trash.dds");
            if (const auto worn = HighlightedOutfit()) {
                m_outfitDeleteButton->SetTooltip(DeleteTooltip(*worn).c_str());
            }
        }
        RefreshOutfitHighlight();
    }

    // Everything that follows a change of which outfit is on: the wheel's worn marks, the
    // undress button, the row itself, and the editing text.
    void AfterOutfitChange()
    {
        // Before the rebuilds, not after: an element is born with whatever palette is
        // current, so flipping first means the new wheel comes up warm instead of being
        // built cool and repainted. The repaint was the half that went missing when the
        // frame that owed it never finished.
        UpdateBackdropScheme();

        RefreshItemSpiral();
        PopulateHandleRow();
        RefreshOutfitRow();
        UpdateEditingText();
        ScheduleSettleRepaint();
    }

    // An edit was just made in the wheel. If the outfit row is open on a saved outfit,
    // that outfit follows the edit.
    //
    // Called at the end of the click handlers - after InventoryManager / UndressManager
    // have returned and the ScopedLockSuspension inside them has folded the change into
    // "locked" - rather than from the equip event, which the engine raises from inside the
    // equip call, before any of that has happened.
    void AfterGearEdit()
    {
        if (!m_outfitRowVisible) return;

        // The look just moved, so a prompt about the look they had is stale.
        DisarmOutfitSwitch();

        RE::Actor* npc = GetCurrentTargetActor();
        auto* slots = OutfitSlotManager::GetSingleton();

        const bool hadDelete = m_outfitDeleteButton != nullptr;

        if (m_editingSlot) {
            slots->SyncEdited(npc, *m_editingSlot);
        }

        // An edit on an unlocked NPC locks them to a look no outfit holds, which changes
        // what the row offers (no delete, nothing lit); otherwise only tints move.
        if (HighlightedOutfit().has_value() != hadDelete) {
            RefreshOutfitRow();
        } else {
            RefreshOutfitHighlight();
        }
        UpdateEditingText();
    }

    void OnOutfitDefaultClicked()
    {
        DisarmOutfitDelete();

        RE::Actor* npc = GetCurrentTargetActor();
        auto* slots = OutfitSlotManager::GetSingleton();
        if (!npc || slots->IsDefault(npc)) {
            DisarmOutfitSwitch();
            RefreshOutfitHighlight();
            return;
        }

        if (ArmSwitchIfNeeded(kDefaultPlate)) return;

        spdlog::info("DressupMenuManager::OnOutfitDefaultClicked - '{}'", npc->GetName());
        slots->SelectDefault(npc);
        m_editingSlot.reset();
        AfterOutfitChange();
    }

    void OnOutfitSelected(int index)
    {
        DisarmOutfitDelete();

        if (index < 0 || index >= static_cast<int>(m_outfitSlotList.size())) {
            spdlog::warn("DressupMenuManager::OnOutfitSelected - Invalid index: {}", index);
            return;
        }

        RE::Actor* npc = GetCurrentTargetActor();
        auto* slots = OutfitSlotManager::GetSingleton();
        const std::uint32_t id = m_outfitSlotList[index].id;

        // Already on: nothing to put on, and the click doubles as a cancel for the prompts.
        if (HighlightedOutfit() == id) {
            DisarmOutfitSwitch();
            RefreshOutfitHighlight();
            return;
        }

        if (ArmSwitchIfNeeded(index)) return;

        spdlog::info("DressupMenuManager::OnOutfitSelected - Outfit {} (id {}) for '{}'",
            index + 1, id, npc ? npc->GetName() : "?");
        if (slots->Select(npc, id)) {
            m_editingSlot = id;
        }
        AfterOutfitChange();
    }

    void OnOutfitSaveClicked()
    {
        DisarmOutfitPrompts();

        RE::Actor* npc = GetCurrentTargetActor();
        if (!npc) return;

        const std::uint32_t id = OutfitSlotManager::GetSingleton()->SaveCurrent(npc);
        m_editingSlot = id;
        AfterOutfitChange();

        // Bring the new plate into view. Ahead of it sit Delete (present now, since the new
        // outfit is the one on), NPC Default and Save.
        if (m_outfitRow) {
            const size_t offset = (m_outfitDeleteButton ? 1u : 0u) + 2u;
            for (size_t i = 0; i < m_outfitSlotList.size(); ++i) {
                if (m_outfitSlotList[i].id == id) {
                    m_outfitRow->ScrollToChild(static_cast<uint32_t>(i + offset));
                    break;
                }
            }
        }
    }

    // First press arms - the icon turns into a question mark and the outfit's plate goes
    // red - and the second press deletes. Anything else pressed in between cancels.
    void OnOutfitDeleteClicked()
    {
        DisarmOutfitSwitch();

        const auto worn = HighlightedOutfit();
        if (!worn) return;

        if (!m_outfitDeleteArmed) {
            m_outfitDeleteArmed = true;
            if (m_outfitDeleteButton) {
                m_outfitDeleteButton->SetTexture("textures\\VRDressup\\question.dds");
                m_outfitDeleteButton->SetTooltip(DeleteTooltip(*worn).c_str());
            }
            RefreshOutfitHighlight();
            return;
        }

        RE::Actor* npc = GetCurrentTargetActor();
        spdlog::info("DressupMenuManager::OnOutfitDeleteClicked - Deleting outfit id {} of '{}'",
            *worn, npc ? npc->GetName() : "?");

        m_outfitDeleteArmed = false;
        auto* slots = OutfitSlotManager::GetSingleton();
        slots->Remove(npc, *worn);
        // Whatever Remove put them into instead is the outfit being edited now - the same
        // as if it had been picked - or nothing, if that was the last one.
        m_editingSlot = slots->Worn(npc);
        AfterOutfitChange();
    }

    // Who the undress button acts on: the player in player mode, the NPC otherwise.
    RE::Actor* UndressTarget() const
    {
        auto* invMgr = InventoryManager::GetSingleton();
        return invMgr->IsTargetPlayer()
            ? RE::PlayerCharacter::GetSingleton()->As<RE::Actor>()
            : invMgr->GetTargetActor();
    }

    // Callback: Undress/Redress button clicked - cycle through states
    void OnUndressButtonClicked()
    {
        RE::Actor* targetActor = UndressTarget();

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
        AfterGearEdit();
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
                m_categoryElements.clear();
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

        LayoutRows();
        RefreshGalleryRow();
        RefreshItemSpiral();
        PopulateHandleRow();
        UpdateInfoText();
    }

    // Stack whatever rows are open under the tool row, in a fixed order - the gallery
    // first, then outfits - and put the info text under the lowest of them.
    static constexpr float kToolRowZ = -10.5f;
    static constexpr float kRowStepZ = 10.0f;
    static constexpr float kEditingTextZ = -5.5f;  // between the wheel's handle and the tool row

    float InfoTextZ() const
    {
        float z = kToolRowZ;
        if (m_outfitRowVisible) z -= kRowStepZ;
        if (IsGalleryVisible()) z -= kRowStepZ;
        // With no row open the text sits a little closer, where it always has.
        return (z == kToolRowZ) ? -18.0f : z - 8.0f;
    }

    void LayoutRows()
    {
        float z = kToolRowZ;

        // The gallery takes the near slot when both are open. Browsing is the thing with
        // the long tail of scrolling, so it wants to be the row nearest the hand; the
        // outfit row is a handful of plates the player picks from and leaves alone.
        if (m_galleryRow && IsGalleryVisible()) {
            z -= kRowStepZ;
            m_galleryRow->SetLocalPosition(0, 0, z);
        }
        if (m_outfitRow && m_outfitRowVisible) {
            z -= kRowStepZ;
            m_outfitRow->SetLocalPosition(0, 0, z);
        }
        if (m_infoText) {
            m_infoText->SetLocalPosition(0, 0, InfoTextZ());
        }
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

        m_categoryElements.clear();  // before Clear - see RefreshItemSpiral
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
            spdlog::debug("DressupMenuManager::RefreshGalleryRow - Showing loading indicator (progress: {:.0f}%)",
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

    // Which entry of the gallery row the spiral is currently showing, or -1 for none.
    // Answered from the active category name rather than a stored index, so it survives the
    // row being narrowed to a different actor and rebuilt underneath.
    int ActiveCategoryIndex() const
    {
        if (m_galleryMode == GalleryMode::Mods && !m_activeModCategory.empty()) {
            for (size_t i = 0; i < m_categoryList.size(); ++i) {
                if (m_categoryList[i].modName == m_activeModCategory) return static_cast<int>(i);
            }
        } else if (m_galleryMode == GalleryMode::Keywords && !m_activeKeywordCategory.empty()) {
            for (size_t i = 0; i < m_keywordCategoryList.size(); ++i) {
                if (m_keywordCategoryList[i].name == m_activeKeywordCategory) return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Re-tint the gallery row so the category the spiral is showing wears the brighter
    // backdrop. Retints the live elements rather than rebuilding the row: rebuilding would
    // reset the scroll position, which on a long mod list throws away the place the player
    // scrolled to in order to make the selection.
    void RefreshCategoryHighlight()
    {
        const int active = ActiveCategoryIndex();

        for (size_t i = 0; i < m_categoryElements.size(); ++i) {
            Backdrop::ApplyCategory(m_categoryElements[i], static_cast<int>(i) == active);
        }
    }

    // Drop from a category's item list everything the actor being dressed has no mesh for.
    void NarrowToTarget(std::vector<RE::TESObjectARMO*>& items) const
    {
        RE::Actor* target = GetCurrentTargetActor();
        if (!target) return;

        std::erase_if(items, [target](RE::TESObjectARMO* armor) {
            return !ItemEquipHelper::FitsActor(armor, target);
        });
    }

    // Narrow a row of cached categories to the actor being dressed: a category with nothing
    // this actor can wear is dropped outright, one that keeps only some of its items has its
    // count corrected, and one whose cached icon is gear this actor cannot wear gets a new
    // icon from what is left. Otherwise a male NPC gets a Wigs category showing a wig and
    // promising forty items, and selecting it draws an empty spiral.
    //
    // Dropping has to happen here, before the row is built - the element id carries its index
    // into m_categoryList / m_keywordCategoryList, so skipping an element inside the loop
    // instead would slide every later category's index onto the wrong entry.
    template <typename CategoryInfo, typename ItemsFor, typename Representative>
    void NarrowCategoriesToTarget(std::vector<CategoryInfo>& categories, ItemsFor itemsFor,
        Representative representative) const
    {
        RE::Actor* target = GetCurrentTargetActor();
        if (!target) return;

        std::vector<CategoryInfo> kept;
        kept.reserve(categories.size());

        for (CategoryInfo category : categories) {
            auto items = itemsFor(category);
            NarrowToTarget(items);
            if (items.empty()) continue;

            category.itemCount = items.size();

            RE::TESObjectARMO*& icon = representative(category);
            if (!ItemEquipHelper::FitsActor(icon, target)) {
                icon = items.front();
            }

            kept.push_back(std::move(category));
        }

        if (kept.size() != categories.size()) {
            spdlog::debug("DressupMenuManager::NarrowCategoriesToTarget - {} of {} categories have "
                "nothing {} can wear",
                categories.size() - kept.size(), categories.size(), target->GetName());
        }

        categories = std::move(kept);
    }

    // Fill the gallery row with one entry per installed mod
    void PopulateModCategories()
    {
        auto* armorModMgr = ArmorModManager::GetSingleton();

        // Get sorted categories from ArmorModManager
        m_categoryList = armorModMgr->GetSortedCategories();
        NarrowCategoriesToTarget(m_categoryList,
            [armorModMgr](const ModCategoryInfo& category) {
                return armorModMgr->GetArmorForMod(category.modName);
            },
            [](ModCategoryInfo& category) -> RE::TESObjectARMO*& {
                return category.representativeArmor;
            });

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
                m_categoryElements.push_back(catElement);
                m_galleryRow->AddChild(catElement);
            }
        }

        RefreshCategoryHighlight();

        spdlog::debug("DressupMenuManager::PopulateModCategories - Populated with {} mods", m_categoryList.size());
    }

    // Fill the gallery row with one entry per keyword category
    void PopulateKeywordCategories()
    {
        auto* keywordMgr = KeywordCategoryManager::GetSingleton();
        m_keywordCategoryList = keywordMgr->GetSortedCategories();
        NarrowCategoriesToTarget(m_keywordCategoryList,
            [keywordMgr](const KeywordCategoryInfo& category) {
                return keywordMgr->GetItemsForCategory(category.name);
            },
            [](KeywordCategoryInfo& category) -> RE::TESObjectARMO*& {
                return category.representative;
            });

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
                m_categoryElements.push_back(catElement);
                m_galleryRow->AddChild(catElement);
            }
        }

        RefreshCategoryHighlight();

        spdlog::debug("DressupMenuManager::PopulateKeywordCategories - Populated with {} categories",
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

        RefreshCategoryHighlight();
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

        RefreshCategoryHighlight();

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

        // Clear item list. Same lock as the rebuilds: a repaint task may be in flight.
        m_currentItemList.clear();
        m_itemElements.clear();

        // Clear gallery state
        m_galleryMode = GalleryMode::None;
        m_galleryLoading = false;
        m_showingHoverText = false;
        m_activeModCategory.clear();
        m_activeKeywordCategory.clear();
        m_galleryArmorList.clear();
        m_categoryList.clear();
        m_keywordCategoryList.clear();
        m_categoryElements.clear();
        if (m_galleryRow) {
            m_galleryRow->SetVisible(false);
        }

        // Clear outfit row state - the row, and with it the editing session, ends here
        m_outfitRowVisible = false;
        m_outfitDeleteArmed = false;
        m_switchArmed = kNothingArmed;
        Backdrop::SetScheme(Backdrop::Scheme::Normal);
        m_editingSlot.reset();
        ForgetOutfitRowElements();
        if (m_outfitRow) {
            m_outfitRow->SetVisible(false);
        }
        if (m_editingText) {
            m_editingText->SetVisible(false);
        }

        // The gallery marks die with the session that made them.
        //
        // A mark means "the player is trying this on"; anything still worn when they close
        // the wheel is a piece they have settled on, and must never be deleted by a later
        // unequip - an undress, a scene, another mod, or a menu session hours afterwards.
        // Before Reset(), which is what clears the target actor.
        if (auto* target = InventoryManager::GetSingleton()->GetTargetActor()) {
            OutfitLockManager::GetSingleton()->ReleaseGalleryItems(target);
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
            spdlog::debug("DressupMenuManager::OnTriggerRelease - Ended initial positioning");
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

        // Under whichever rows are open
        m_infoText->SetLocalPosition(0, 0, InfoTextZ());

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

        // Under whichever rows are open
        m_infoText->SetLocalPosition(0, 0, InfoTextZ());

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
                spdlog::info("DressupMenuManager::OnItemSelected - Toggled gallery armor '{}'", armor->GetFullName());
            }

            // The plate the player just clicked has changed state - and so has any other
            // plate in the same slot, because putting one piece on takes the other off.
            // Both are predictions; OnWornStateChanged corrects them once the engine has
            // actually moved, and the sweep covers an equip it never gets round to.
            RefreshItemHighlight();
            ScheduleSettleRepaint();

            // Refresh handle row if undress state changed
            bool undressStateCleared = hadUndressState && targetActor &&
                !UndressManager::GetSingleton()->HasUndressState(targetActor);
            if (undressStateCleared) {
                PopulateHandleRow();
            }
            AfterGearEdit();
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
        } else {
            // The list is the same; only what the NPC has on has changed.
            RefreshItemHighlight();
        }
        ScheduleSettleRepaint();

        // Refresh handle row if undress state changed
        if (undressStateCleared) {
            PopulateHandleRow();
        }
        AfterGearEdit();
    }

    // Just past ItemEquipHelper's kEquipGrace, so the sweep reads the state the grace has
    // already given up on rather than racing it.
    static constexpr std::int32_t kSettleRepaintMs = 1100;

    // m_switchArmed's two sentinels. An outfit plate arms as its own row index.
    static constexpr int kNothingArmed = -2;
    static constexpr int kDefaultPlate = -1;

    P3DUI::Interface001* m_api = nullptr;
    P3DUI::Root* m_root = nullptr;
    P3DUI::Container* m_itemSpiral = nullptr;
    P3DUI::ScrollableContainer* m_handleRow = nullptr;   // ColumnGrid for handle buttons (single row)
    P3DUI::ScrollableContainer* m_galleryRow = nullptr;  // ColumnGrid for mod categories (horizontal scroll)
    P3DUI::ScrollableContainer* m_outfitRow = nullptr;   // ColumnGrid of saved outfits (horizontal scroll)
    P3DUI::Text* m_infoText = nullptr;         // Context-dependent info text
    P3DUI::Text* m_editingText = nullptr;      // "Editing Outfit N", between wheel and tool row

    // Outfit row state. The slot list and the element vector are index-aligned, same rule
    // as m_categoryElements. m_editingSlot is the saved outfit the wheel's edits are being
    // written into: set when the row opens on a worn outfit, or by selecting or saving one;
    // cleared by NPC Default, a delete, closing the row, or closing the menu. Never
    // persisted - it is a property of this row being open.
    bool m_outfitRowVisible = false;
    bool m_outfitDeleteArmed = false;

    // Which plate is one press away from discarding an unsaved look: the index of an outfit
    // plate, kDefaultPlate for NPC Default, kNothingArmed for none. Only ever set while the
    // NPC is wearing something no saved outfit holds - see NeedsSwitchConfirmation.
    int m_switchArmed = kNothingArmed;
    std::optional<std::uint32_t> m_editingSlot;
    std::vector<OutfitSlotManager::Slot> m_outfitSlotList;
    std::vector<P3DUI::Element*> m_outfitElements;
    P3DUI::Element* m_outfitDefaultElement = nullptr;
    P3DUI::Element* m_outfitDeleteButton = nullptr;

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
    // The gallery row's live elements, in the same order as the category list below. Kept
    // so the selection highlight can be re-tinted in place; cleared whenever the row is,
    // because Clear() destroys the elements these point at.
    std::vector<P3DUI::Element*> m_categoryElements;

    // The wheel's live elements, in the same order as m_currentItemList, so the worn
    // highlight can be re-tinted without rebuilding the spiral. Same lifetime rule as
    // m_categoryElements: cleared wherever the container it holds is.
    std::vector<P3DUI::Element*> m_itemElements;

    // A repaint the wheel owes, raised off the main thread and paid on it. See
    // DrainPendingHighlight.
    std::atomic<bool> m_highlightDirty{false};

    std::vector<ModCategoryInfo> m_categoryList;         // Mods currently in the gallery row
    std::vector<KeywordCategoryInfo> m_keywordCategoryList;  // Keyword categories currently in the gallery row

    bool IsGalleryVisible() const { return m_galleryMode != GalleryMode::None; }

    // True while the spiral shows a category's items rather than an inventory
    bool IsShowingGalleryItems() const
    {
        return !m_activeModCategory.empty() || !m_activeKeywordCategory.empty();
    }
};

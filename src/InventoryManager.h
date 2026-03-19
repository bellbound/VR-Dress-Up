#pragma once

#include <RE/Skyrim.h>
#include <algorithm>
#include <vector>
#include <string>
#include <functional>
#include "log.h"
#include "dressup/OutfitLockManager.h"
#include "dressup/DressupTransaction.h"
#include "dressup/ItemEquipHelper.h"
#include "dressup/UndressManager.h"

// Filter mode for inventory display
enum class InventoryFilterMode
{
    All,      // Show both armor and weapons
    Armor,    // Show only armor
    Weapons   // Show only weapons
};

struct DressupInventoryItem
{
    std::string name;
    std::string nifPath;
    RE::FormID formId = 0;
    RE::TESObjectARMO* armor = nullptr;
    RE::TESObjectWEAP* weapon = nullptr;
    std::function<void()> onSelected;
};

class InventoryManager : public RE::BSTEventSink<RE::TESEquipEvent>
{
public:
    static constexpr size_t MAX_COUNT_FOR_TOGGLE = 16;

    static InventoryManager* GetSingleton()
    {
        static InventoryManager instance;
        return &instance;
    }

    // Initialize event sink
    void Initialize()
    {
        if (m_initialized) return;

        auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (sourceHolder) {
            sourceHolder->AddEventSink<RE::TESEquipEvent>(this);
            spdlog::info("InventoryManager::Initialize - Registered for TESEquipEvent");
            m_initialized = true;
        }
    }

    void Shutdown()
    {
        if (!m_initialized) return;

        auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (sourceHolder) {
            sourceHolder->RemoveEventSink<RE::TESEquipEvent>(this);
        }
        m_initialized = false;
    }

    // Process equip events - return transferred items to player when NPC unequips them
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESEquipEvent* a_event,
        RE::BSTEventSource<RE::TESEquipEvent>*) override
    {
        if (!a_event || m_processingReturn || a_event->equipped) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (!m_targetActor || !m_targetIsPlayer) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* actor = a_event->actor.get();
        if (!actor || actor->GetFormID() != m_targetActor->GetFormID()) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Check if this was a transferred item and return it to player
        if (m_transaction.RemoveFromTransferTracking(a_event->baseObject)) {
            m_processingReturn = true;
            ReturnItemToPlayer(a_event->baseObject);
            m_processingReturn = false;
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    // === Target Management ===

    void SetTargetActor(RE::Actor* actor)
    {
        m_targetActor = actor;
        spdlog::info("InventoryManager::SetTargetActor - Set target to: {}",
            actor ? actor->GetName() : "null");
        m_transaction.CapturePlayerEquipment();
    }

    RE::Actor* GetTargetActor() const { return m_targetActor; }

    void SetTargetIsPlayer(bool isPlayer)
    {
        // When switching back to NPC mode, clear ghost items (transfers are "confirmed")
        if (!isPlayer && m_targetIsPlayer) {
            m_transaction.ClearTransferTracking();
        }
        m_targetIsPlayer = isPlayer;
        spdlog::info("InventoryManager::SetTargetIsPlayer - Target is player: {}", isPlayer);
    }

    bool IsTargetPlayer() const { return m_targetIsPlayer; }

    // === Filter Mode ===

    InventoryFilterMode GetFilterMode() const { return m_filterMode; }

    void SetFilterMode(InventoryFilterMode mode) { m_filterMode = mode; }

    void ToggleFilterMode()
    {
        m_filterMode = (m_filterMode == InventoryFilterMode::Armor)
            ? InventoryFilterMode::Weapons
            : InventoryFilterMode::Armor;
    }

    bool ShouldShowFilterToggle()
    {
        return GetTotalItemCount() >= MAX_COUNT_FOR_TOGGLE;
    }

    size_t GetTotalItemCount()
    {
        RE::Actor* source = GetSourceActor();
        if (!source) return 0;

        return ItemEquipHelper::GetInventoryItems<RE::TESObjectARMO>(source).size() +
               ItemEquipHelper::GetInventoryItems<RE::TESObjectWEAP>(source).size();
    }

    // === Inventory Retrieval ===

    std::vector<DressupInventoryItem> GetDisplayItems()
    {
        auto items = GetInventoryArmor();
        auto weapons = GetInventoryWeapons();
        items.insert(items.end(), weapons.begin(), weapons.end());
        return items;
    }

    std::vector<DressupInventoryItem> GetInventoryArmor()
    {
        std::vector<DressupInventoryItem> result;
        RE::Actor* source = GetSourceActor();
        if (!source) return result;

        // Get inventory with counts for equipped item filtering
        auto inventory = source->GetInventory([](RE::TESBoundObject& obj) {
            return obj.Is(RE::FormType::Armor);
        });

        for (const auto& [item, data] : inventory) {
            if (data.first <= 0) continue;

            auto* armor = item->As<RE::TESObjectARMO>();
            if (!armor) continue;

            // Filter out TNG genital cover items
            if (ShouldFilterItemName(armor->GetFullName())) {
                continue;
            }

            // In player mode, skip equipped items if player only has 1
            // (don't let them give away their only copy of what they're wearing)
            if (m_targetIsPlayer && data.first == 1 && ItemEquipHelper::IsArmorEquipped(source, armor)) {
                continue;
            }

            DressupInventoryItem invItem;
            invItem.name = armor->GetFullName();
            invItem.nifPath = ItemEquipHelper::GetModelPath(armor);
            invItem.formId = armor->GetFormID();
            invItem.armor = armor;
            result.push_back(invItem);
        }

        // In player mode, also show transferred items as "ghost" entries
        if (m_targetIsPlayer) {
            for (auto* armor : m_transaction.GetTransferredArmor()) {
                // Filter out TNG genital cover items
                if (ShouldFilterItemName(armor->GetFullName())) {
                    continue;
                }
                DressupInventoryItem item;
                item.name = armor->GetFullName();
                item.nifPath = ItemEquipHelper::GetModelPath(armor);
                item.formId = armor->GetFormID();
                item.armor = armor;
                result.push_back(item);
            }
            // Sort by FormID so ghost items maintain their original position
            std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
                return a.formId < b.formId;
            });
        }

        return result;
    }

    std::vector<DressupInventoryItem> GetInventoryWeapons()
    {
        std::vector<DressupInventoryItem> result;
        RE::Actor* source = GetSourceActor();
        if (!source) return result;

        // Get inventory with counts for equipped item filtering
        auto inventory = source->GetInventory([](RE::TESBoundObject& obj) {
            return obj.Is(RE::FormType::Weapon);
        });

        for (const auto& [item, data] : inventory) {
            if (data.first <= 0) continue;

            auto* weapon = item->As<RE::TESObjectWEAP>();
            if (!weapon) continue;

            // In player mode, skip equipped weapons if player only has 1
            // (don't let them give away their only copy of what they're holding)
            if (m_targetIsPlayer && data.first == 1 && ItemEquipHelper::IsWeaponEquipped(source, weapon)) {
                continue;
            }

            DressupInventoryItem invItem;
            invItem.name = weapon->GetFullName();
            invItem.nifPath = ItemEquipHelper::GetModelPath(weapon);
            invItem.formId = weapon->GetFormID();
            invItem.weapon = weapon;
            result.push_back(invItem);
        }

        // In player mode, also show transferred items as "ghost" entries
        if (m_targetIsPlayer) {
            for (auto* weapon : m_transaction.GetTransferredWeapons()) {
                DressupInventoryItem item;
                item.name = weapon->GetFullName();
                item.nifPath = ItemEquipHelper::GetModelPath(weapon);
                item.formId = weapon->GetFormID();
                item.weapon = weapon;
                result.push_back(item);
            }
            // Sort by FormID so ghost items maintain their original position
            std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
                return a.formId < b.formId;
            });
        }

        return result;
    }

    // === Item Selection ===

    // Returns true if undress state was cleared (for UI refresh purposes)
    bool OnItemSelected(RE::TESObjectARMO* armor)
    {
        if (!armor || !m_targetActor) return false;

        auto* outfitMgr = OutfitLockManager::GetSingleton();
        bool wasLocked = outfitMgr->IsLocked(m_targetActor);

        if (m_targetIsPlayer) {
            if (m_transaction.IsTransferredItem(armor->GetFormID())) {
                outfitMgr->RemoveFromLockedOutfit(m_targetActor, armor);
                ReverseTransfer(armor);
            } else {
                EquipFromPlayer(armor);
            }
        } else {
            // Toggle: update outfit data before equip/unequip
            if (ItemEquipHelper::IsArmorEquipped(m_targetActor, armor)) {
                outfitMgr->RemoveFromLockedOutfit(m_targetActor, armor);
                ItemEquipHelper::UnequipItem(m_targetActor, armor);
            } else {
                outfitMgr->AddToLockedOutfit(m_targetActor, armor);
                ItemEquipHelper::EquipItem(m_targetActor, armor);
            }
        }

        bool undressStateCleared = ClearUndressStateOnGearChange();

        if (!wasLocked) {
            AutoLockNpc();
        }
        return undressStateCleared;
    }

    // Returns true if undress state was cleared (for UI refresh purposes)
    bool OnItemSelected(RE::TESObjectWEAP* weapon)
    {
        if (!weapon || !m_targetActor) return false;

        auto* outfitMgr = OutfitLockManager::GetSingleton();
        bool wasLocked = outfitMgr->IsLocked(m_targetActor);

        // User explicitly changing weapon → lock weapons
        outfitMgr->SetWeaponUnlocked(m_targetActor, false);

        if (m_targetIsPlayer) {
            if (m_transaction.IsTransferredItem(weapon->GetFormID())) {
                outfitMgr->RemoveFromLockedOutfit(m_targetActor, weapon);
                ReverseTransfer(weapon);
            } else {
                EquipFromPlayer(weapon);
            }
        } else {
            // Toggle: update outfit data before equip/unequip
            if (ItemEquipHelper::IsWeaponEquipped(m_targetActor, weapon)) {
                outfitMgr->RemoveFromLockedOutfit(m_targetActor, weapon);
                ItemEquipHelper::UnequipItem(m_targetActor, weapon);
            } else {
                outfitMgr->AddToLockedOutfit(m_targetActor, weapon);
                ItemEquipHelper::EquipItem(m_targetActor, weapon);
            }
        }

        bool undressStateCleared = ClearUndressStateOnGearChange();

        if (!wasLocked) {
            AutoLockNpc();
        }
        return undressStateCleared;
    }

    void Reset()
    {
        m_targetActor = nullptr;
        m_targetIsPlayer = false;
        m_filterMode = InventoryFilterMode::Armor;
        m_transaction.Clear();
    }

    // === Lock Management (delegates to OutfitLockManager) ===

    bool IsNpcLocked() const
    {
        return m_targetActor && OutfitLockManager::GetSingleton()->IsLocked(m_targetActor);
    }

    // Unlock NPC (removes lock, NPC returns to normal AI outfit control)
    bool UnlockNpc()
    {
        if (!m_targetActor) return false;

        spdlog::info("InventoryManager::UnlockNpc - Unlocking '{}'", m_targetActor->GetName());
        return OutfitLockManager::GetSingleton()->Unlock(m_targetActor);
    }

    // Manually lock NPC (prevents AI outfit changes)
    bool LockNpc()
    {
        if (!m_targetActor) return false;

        spdlog::info("InventoryManager::LockNpc - Locking '{}'", m_targetActor->GetName());
        OutfitLockManager::GetSingleton()->Lock(m_targetActor);
        return true;
    }

    // Check if NPC has any items from player
    bool HasPlayerItems() const
    {
        return m_targetActor && OutfitLockManager::GetSingleton()->HasPlayerItems(m_targetActor);
    }

    // Return all player items back to player's inventory
    void ReturnPlayerItems()
    {
        if (!m_targetActor) return;

        spdlog::info("InventoryManager::ReturnPlayerItems - Returning items to player from '{}'",
            m_targetActor->GetName());
        OutfitLockManager::GetSingleton()->ReturnPlayerItems(m_targetActor);
    }

    // Equip item from mod gallery onto NPC (armor version)
    // Similar to EquipFromPlayer but adds item directly to NPC inventory
    void EquipFromMod(RE::TESObjectARMO* armor)
    {
        if (!armor || !m_targetActor) {
            spdlog::warn("InventoryManager::EquipFromMod - Invalid armor or target actor");
            return;
        }

        auto* outfitMgr = OutfitLockManager::GetSingleton();
        bool wasLocked = outfitMgr->IsLocked(m_targetActor);

        // Check if NPC already has this armor in inventory
        bool hasInInventory = ItemEquipHelper::HasItemInInventory(m_targetActor, armor);

        // Update outfit data before equip/unequip calls
        auto* wornArmor = ItemEquipHelper::GetWornInSlot(m_targetActor, armor);
        if (wornArmor) {
            outfitMgr->RemoveFromLockedOutfit(m_targetActor, wornArmor);
        }
        outfitMgr->AddToLockedOutfit(m_targetActor, armor);

        // Now unequip/equip — hooks validate against updated data
        if (wornArmor) {
            ItemEquipHelper::UnequipItem(m_targetActor, wornArmor);
        }

        if (!hasInInventory) {
            m_targetActor->AddObjectToContainer(armor, nullptr, 1, nullptr);
            outfitMgr->MarkItemAsGallerySpawned(m_targetActor, armor->GetFormID());

            spdlog::info("InventoryManager::EquipFromMod - Added '{}' to {}'s inventory (gallery-spawned)",
                armor->GetFullName(), m_targetActor->GetName());
        }

        ItemEquipHelper::EquipItem(m_targetActor, armor);

        ClearUndressStateOnGearChange();

        if (!wasLocked) {
            AutoLockNpc();
        }

        spdlog::info("InventoryManager::EquipFromMod - Equipped '{}' on {} (was in inventory: {})",
            armor->GetFullName(), m_targetActor->GetName(), hasInInventory);
    }

private:
    // Check if an item name should be filtered out from display
    static bool ShouldFilterItemName(const char* name)
    {
        if (!name) return false;

        std::string itemName(name);
        return itemName == "TNG GenitalsCover" ||
               itemName == "TNG GenitalCover" ||
               itemName == "TNG Genitals Cover" ||
               itemName == "TNG Genital Cover";
    }

    InventoryManager() = default;
    ~InventoryManager() = default;
    InventoryManager(const InventoryManager&) = delete;
    InventoryManager& operator=(const InventoryManager&) = delete;

    RE::Actor* GetSourceActor() const
    {
        return m_targetIsPlayer ? RE::PlayerCharacter::GetSingleton() : m_targetActor;
    }

    // Auto-lock NPC when changes are made
    void AutoLockNpc()
    {
        if (m_targetActor) {
            OutfitLockManager::GetSingleton()->Lock(m_targetActor);
        }
    }

    // Clear undress state when gear is manually changed via menu
    // This resets the NPC to "Dressed" state since they now have a modified outfit
    // Returns true if state was actually cleared (for UI refresh purposes)
    bool ClearUndressStateOnGearChange()
    {
        if (m_targetActor) {
            auto* undressMgr = UndressManager::GetSingleton();
            if (undressMgr->HasUndressState(m_targetActor)) {
                spdlog::info("InventoryManager::ClearUndressStateOnGearChange - Clearing undress state for '{}'",
                    m_targetActor->GetName());
                undressMgr->ClearUndressState(m_targetActor);
                return true;
            }
        }
        return false;
    }

    // Equip item from player's inventory onto NPC (armor version)
    void EquipFromPlayer(RE::TESObjectARMO* armor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* outfitMgr = OutfitLockManager::GetSingleton();
        if (!player || !ItemEquipHelper::HasItemInInventory(player, armor)) {
            spdlog::warn("InventoryManager::EquipFromPlayer - Player doesn't have '{}'", armor->GetFullName());
            return;
        }

        // Update outfit data before equip/unequip
        auto* wornArmor = ItemEquipHelper::GetWornInSlot(m_targetActor, armor);
        if (wornArmor) {
            outfitMgr->RemoveFromLockedOutfit(m_targetActor, wornArmor);
        }
        outfitMgr->AddToLockedOutfit(m_targetActor, armor);

        // Now unequip/equip — hooks validate against updated data
        if (wornArmor) {
            ItemEquipHelper::UnequipItem(m_targetActor, wornArmor);
            if (!m_transaction.IsTransferredItem(wornArmor->GetFormID())) {
                m_transaction.TrackUnequip(wornArmor);
            }
        }

        ItemEquipHelper::TransferItem(player, m_targetActor, armor);
        ItemEquipHelper::EquipItem(m_targetActor, armor);
        m_transaction.TrackTransfer(armor, true);

        outfitMgr->MarkItemAsPlayerGiven(m_targetActor, armor->GetFormID());

        spdlog::info("InventoryManager::EquipFromPlayer - Transferred and equipped '{}' on {}",
            armor->GetFullName(), m_targetActor->GetName());
    }

    // Equip item from player's inventory onto NPC (weapon version)
    void EquipFromPlayer(RE::TESObjectWEAP* weapon)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* outfitMgr = OutfitLockManager::GetSingleton();
        if (!player || !ItemEquipHelper::HasItemInInventory(player, weapon)) {
            spdlog::warn("InventoryManager::EquipFromPlayer - Player doesn't have '{}'", weapon->GetFullName());
            return;
        }

        // Update outfit data before equip/unequip
        auto equippedWeapons = ItemEquipHelper::GetEquippedWeapons(m_targetActor);
        for (auto* currentWeapon : equippedWeapons) {
            outfitMgr->RemoveFromLockedOutfit(m_targetActor, currentWeapon);
        }
        outfitMgr->AddToLockedOutfit(m_targetActor, weapon);

        // Now unequip/equip — hooks validate against updated data
        for (auto* currentWeapon : equippedWeapons) {
            if (!m_transaction.IsTransferredItem(currentWeapon->GetFormID())) {
                m_transaction.TrackUnequip(currentWeapon);
            }
            ItemEquipHelper::UnequipItem(m_targetActor, currentWeapon);
        }

        ItemEquipHelper::TransferItem(player, m_targetActor, weapon);
        ItemEquipHelper::EquipItem(m_targetActor, weapon);
        m_transaction.TrackTransfer(weapon, true);

        outfitMgr->MarkItemAsPlayerGiven(m_targetActor, weapon->GetFormID());

        spdlog::info("InventoryManager::EquipFromPlayer - Transferred and equipped '{}' on {}",
            weapon->GetFullName(), m_targetActor->GetName());
    }

    // Return an item from NPC back to player (called from equip event)
    void ReturnItemToPlayer(RE::FormID formID)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !m_targetActor) return;

        auto* form = RE::TESForm::LookupByID(formID);
        if (!form) return;

        m_targetActor->RemoveItem(form->As<RE::TESBoundObject>(), 1,
            RE::ITEM_REMOVE_REASON::kRemove, nullptr, player);

        spdlog::info("InventoryManager::ReturnItemToPlayer - Returned item 0x{:08X} to player", formID);
    }

    // Reverse a transfer - unequip from NPC and return to player (called when clicking ghost item)
    void ReverseTransfer(RE::TESBoundObject* item)
    {
        if (!item || !m_targetActor) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Outfit data already updated by caller (RemoveFromLockedOutfit called before this)

        // Unequip from NPC (hook allows — item removed from outfit)
        ItemEquipHelper::UnequipItem(m_targetActor, item);

        // Transfer back to player
        m_targetActor->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, player);

        // Remove from transfer tracking
        m_transaction.RemoveFromTransferTracking(item->GetFormID());

        spdlog::info("InventoryManager::ReverseTransfer - Returned '{}' to player", item->GetName());
    }

    RE::Actor* m_targetActor = nullptr;
    bool m_targetIsPlayer = false;
    InventoryFilterMode m_filterMode = InventoryFilterMode::Armor;
    DressupTransaction m_transaction;
    bool m_initialized = false;
    bool m_processingReturn = false;
};

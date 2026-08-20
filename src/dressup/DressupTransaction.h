#pragma once

#include <RE/Skyrim.h>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include "log.h"
#include "OutfitLockManager.h"
#include "ItemEquipHelper.h"
#include "DeviceCompat.h"

// Tracks an item that was transferred from player to NPC during player mode
struct TransferredItem
{
    RE::TESBoundObject* item;
    bool wasEquippedOnNpc;
};

// Tracks an item that was unequipped from NPC during player mode
struct UnequippedNpcItem
{
    RE::TESBoundObject* item;
};

// Manages transaction state for dressup operations - tracks changes for cancel/confirm
class DressupTransaction
{
public:
    DressupTransaction() = default;

    // Track a transfer from player to NPC (with duplicate check)
    void TrackTransfer(RE::TESBoundObject* item, bool equippedOnNpc)
    {
        if (!item) return;

        // Check for duplicate
        if (IsTransferredItem(item->GetFormID())) {
            spdlog::warn("DressupTransaction::TrackTransfer - '{}' already tracked, skipping",
                item->GetName());
            return;
        }

        m_transferredItems.push_back({item, equippedOnNpc});
        spdlog::debug("DressupTransaction::TrackTransfer - Tracking '{}' (equipped={})",
            item->GetName(), equippedOnNpc);
    }

    // Track an NPC item that was unequipped (with duplicate check)
    void TrackUnequip(RE::TESBoundObject* item)
    {
        if (!item) return;

        // Check for duplicate
        if (IsUnequippedItem(item->GetFormID())) {
            spdlog::trace("DressupTransaction::TrackUnequip - '{}' already tracked, skipping",
                item->GetName());
            return;
        }

        m_unequippedNpcItems.push_back({item});
        spdlog::debug("DressupTransaction::TrackUnequip - Tracking unequipped '{}'",
            item->GetName());
    }

    // Remove an item from transfer tracking (called when NPC unequips it via event)
    bool RemoveFromTransferTracking(RE::FormID formID)
    {
        for (auto it = m_transferredItems.begin(); it != m_transferredItems.end(); ++it) {
            if (it->item && it->item->GetFormID() == formID) {
                spdlog::debug("DressupTransaction::RemoveFromTransferTracking - Removed '{}'",
                    it->item->GetName());
                m_transferredItems.erase(it);
                return true;
            }
        }
        return false;
    }

    // Check if an item is a transferred item (not NPC's own)
    bool IsTransferredItem(RE::FormID formID) const
    {
        for (const auto& item : m_transferredItems) {
            if (item.item && item.item->GetFormID() == formID) {
                return true;
            }
        }
        return false;
    }

    // Check if an item is already tracked as unequipped
    bool IsUnequippedItem(RE::FormID formID) const
    {
        for (const auto& item : m_unequippedNpcItems) {
            if (item.item && item.item->GetFormID() == formID) {
                return true;
            }
        }
        return false;
    }

    // Capture player's currently equipped armor and weapons (for restore on cancel)
    void CapturePlayerEquipment()
    {
        m_playerEquippedArmor.clear();
        m_playerEquippedWeapons.clear();

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Capture equipped armor for slots 30-45 (with duplicate check)
        for (int slot = 30; slot <= 45; ++slot) {
            auto slotMask = static_cast<RE::BIPED_MODEL::BipedObjectSlot>(1 << (slot - 30));
            auto* armor = player->GetWornArmor(slotMask);
            if (armor && !HasCapturedArmor(armor->GetFormID())) {
                m_playerEquippedArmor.push_back(armor);
                spdlog::debug("DressupTransaction::CapturePlayerEquipment - Captured armor '{}' in slot {}",
                    armor->GetFullName(), slot);
            }
        }

        // Capture equipped weapons (both hands)
        RE::TESForm* rightHand = player->GetEquippedObject(false);
        RE::TESForm* leftHand = player->GetEquippedObject(true);

        if (rightHand) {
            auto* rightWeapon = rightHand->As<RE::TESObjectWEAP>();
            if (rightWeapon) {
                m_playerEquippedWeapons.push_back(rightWeapon);
                spdlog::debug("DressupTransaction::CapturePlayerEquipment - Captured weapon '{}' (right hand)",
                    rightWeapon->GetFullName());
            }
        }

        if (leftHand) {
            auto* leftWeapon = leftHand->As<RE::TESObjectWEAP>();
            // Only add if different from right hand
            if (leftWeapon && (!rightHand || leftHand->GetFormID() != rightHand->GetFormID())) {
                m_playerEquippedWeapons.push_back(leftWeapon);
                spdlog::debug("DressupTransaction::CapturePlayerEquipment - Captured weapon '{}' (left hand)",
                    leftWeapon->GetFullName());
            }
        }

        spdlog::info("DressupTransaction::CapturePlayerEquipment - Captured {} armor, {} weapons",
            m_playerEquippedArmor.size(), m_playerEquippedWeapons.size());
    }

    // Cancel all changes - revert items to player, re-equip NPC's original items
    void Cancel(RE::Actor* targetActor)
    {
        if (!targetActor) {
            spdlog::warn("DressupTransaction::Cancel - No target actor");
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (!player || !equipManager) {
            return;
        }

        spdlog::info("DressupTransaction::Cancel - Reverting {} transferred items, {} unequipped items",
            m_transferredItems.size(), m_unequippedNpcItems.size());

        // Suspend lock enforcement while we make changes to the NPC
        ScopedLockSuspension suspension(targetActor);

        // Unequip and return transferred items to player
        for (const auto& transfer : m_transferredItems) {
            if (!transfer.item) continue;

            // A Devious Device comes off through DD or not at all - see DeviceCompat. If
            // DD refuses, the device stays on the NPC rather than being handed back with
            // its rendered half still worn.
            if (auto* armor = transfer.item->As<RE::TESObjectARMO>();
                armor && DeviceCompat::IsDevice(armor)) {
                if (!ItemEquipHelper::UnequipArmor(targetActor, armor)) {
                    spdlog::debug("  Left '{}' on the NPC: Devious Devices will not release it",
                        transfer.item->GetName());
                    continue;
                }
            } else {
                equipManager->UnequipObject(targetActor, transfer.item, nullptr, 1, nullptr, false, true);
            }

            targetActor->RemoveItem(transfer.item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, player);
            spdlog::debug("  Returned '{}' to player", transfer.item->GetName());
        }

        // Re-equip NPC's original items that were unequipped (with inventory check)
        for (const auto& unequipped : m_unequippedNpcItems) {
            if (!unequipped.item) continue;

            // Check if NPC still has this item in inventory
            auto inventory = targetActor->GetInventory([&unequipped](RE::TESBoundObject& obj) {
                return obj.GetFormID() == unequipped.item->GetFormID();
            });

            if (inventory.empty()) {
                spdlog::warn("  NPC no longer has '{}', cannot re-equip", unequipped.item->GetName());
                continue;
            }

            if (auto* armor = unequipped.item->As<RE::TESObjectARMO>();
                armor && DeviceCompat::IsDevice(armor)) {
                ItemEquipHelper::EquipArmor(targetActor, armor);
            } else {
                equipManager->EquipObject(targetActor, unequipped.item, nullptr, 1, nullptr, true, false, false);
            }
            spdlog::debug("  Re-equipped '{}' on NPC", unequipped.item->GetName());
        }

        // Restore player's equipped items
        RestorePlayerEquipment();

        // Lock will be re-applied when suspension goes out of scope (if was locked)
        Clear();
    }

    // Confirm changes - just clear tracking without reverting
    void Confirm()
    {
        spdlog::info("DressupTransaction::Confirm - Confirmed {} transferred items",
            m_transferredItems.size());
        Clear();
    }

    // Clear all tracking state
    void Clear()
    {
        m_transferredItems.clear();
        m_unequippedNpcItems.clear();
        m_playerEquippedArmor.clear();
        m_playerEquippedWeapons.clear();
    }

    // Check if we have any pending changes
    bool HasChanges() const
    {
        return !m_transferredItems.empty();
    }

    // Get count of transferred items
    size_t GetTransferCount() const
    {
        return m_transferredItems.size();
    }

    // Get set of transferred item FormIDs (items that came from player)
    std::unordered_set<RE::FormID> GetTransferredItemFormIDs() const
    {
        std::unordered_set<RE::FormID> result;
        for (const auto& item : m_transferredItems) {
            if (item.item) {
                result.insert(item.item->GetFormID());
            }
        }
        return result;
    }

    // Get transferred armor items (for ghost display in player inventory view)
    std::vector<RE::TESObjectARMO*> GetTransferredArmor() const
    {
        std::vector<RE::TESObjectARMO*> result;
        for (const auto& item : m_transferredItems) {
            if (auto* armor = item.item ? item.item->As<RE::TESObjectARMO>() : nullptr) {
                result.push_back(armor);
            }
        }
        return result;
    }

    // Get transferred weapon items (for ghost display in player inventory view)
    std::vector<RE::TESObjectWEAP*> GetTransferredWeapons() const
    {
        std::vector<RE::TESObjectWEAP*> result;
        for (const auto& item : m_transferredItems) {
            if (auto* weapon = item.item ? item.item->As<RE::TESObjectWEAP>() : nullptr) {
                result.push_back(weapon);
            }
        }
        return result;
    }

    // Clear only transfer tracking (ghost items disappear when switching back to NPC view)
    void ClearTransferTracking()
    {
        m_transferredItems.clear();
    }

private:
    // Check if armor is already captured (for duplicate prevention)
    bool HasCapturedArmor(RE::FormID formID) const
    {
        for (const auto* armor : m_playerEquippedArmor) {
            if (armor && armor->GetFormID() == formID) {
                return true;
            }
        }
        return false;
    }

    // Restore player's equipment that was captured
    void RestorePlayerEquipment()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (!player || !equipManager) return;

        spdlog::info("DressupTransaction::RestorePlayerEquipment - Restoring {} armor, {} weapons",
            m_playerEquippedArmor.size(), m_playerEquippedWeapons.size());

        // Restore armor
        for (auto* armor : m_playerEquippedArmor) {
            if (!armor) continue;

            // Check if player still has this item in inventory
            auto inventory = player->GetInventory([armor](RE::TESBoundObject& obj) {
                return obj.GetFormID() == armor->GetFormID();
            });

            if (inventory.empty()) {
                spdlog::debug("  Player no longer has armor '{}', skipping", armor->GetFullName());
                continue;
            }

            // Check if player already has something equipped in this slot
            auto slotMask = armor->GetSlotMask();
            auto* wornArmor = player->GetWornArmor(slotMask);

            if (wornArmor && wornArmor->GetFormID() == armor->GetFormID()) {
                spdlog::trace("  Player already wearing '{}', skipping", armor->GetFullName());
                continue;
            }

            equipManager->EquipObject(player, armor, nullptr, 1, nullptr, true, false, false);
            spdlog::debug("  Re-equipped armor '{}' on player", armor->GetFullName());
        }

        // Restore weapons
        for (auto* weapon : m_playerEquippedWeapons) {
            if (!weapon) continue;

            // Check if player still has this weapon in inventory
            auto inventory = player->GetInventory([weapon](RE::TESBoundObject& obj) {
                return obj.GetFormID() == weapon->GetFormID();
            });

            if (inventory.empty()) {
                spdlog::debug("  Player no longer has weapon '{}', skipping", weapon->GetFullName());
                continue;
            }

            equipManager->EquipObject(player, weapon, nullptr, 1, nullptr, true, false, false);
            spdlog::debug("  Re-equipped weapon '{}' on player", weapon->GetFullName());
        }
    }

    std::vector<TransferredItem> m_transferredItems;
    std::vector<UnequippedNpcItem> m_unequippedNpcItems;
    std::vector<RE::TESObjectARMO*> m_playerEquippedArmor;
    std::vector<RE::TESObjectWEAP*> m_playerEquippedWeapons;
};

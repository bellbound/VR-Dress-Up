#pragma once

#include <RE/Skyrim.h>
#include <vector>
#include "log.h"

// Tracks an item that was transferred from player to NPC during player mode
struct TransferredItem
{
    RE::TESBoundObject* item;
    bool wasEquippedOnNpc;
};

// Remembers what the player has handed the NPC in this menu session, so the player's own
// wheel can keep showing those pieces as ghost entries and a click on one takes it back.
//
// This used to be a full transaction - it also captured the player's gear and the NPC's
// displaced pieces so a cancel could unwind everything, and an NPC taking a handed-over
// piece off returned it to the player by itself. The cancel never had a caller, and the
// automatic return went when outfits arrived: an outfit switch takes pieces off too, and
// handing them back then meant conjuring a second copy when the outfit went back on.
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

    // Remove an item from transfer tracking (the player took it back)
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

    // FormIDs of everything handed over and not yet taken back.
    std::vector<RE::FormID> TransferredFormIDs() const
    {
        std::vector<RE::FormID> result;
        result.reserve(m_transferredItems.size());
        for (const auto& item : m_transferredItems) {
            if (item.item) result.push_back(item.item->GetFormID());
        }
        return result;
    }

    // Clear all tracking state
    void Clear()
    {
        m_transferredItems.clear();
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
    std::vector<TransferredItem> m_transferredItems;
};

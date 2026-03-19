#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <unordered_map>
#include <mutex>
#include "OutfitLockManager.h"
#include "ItemEquipHelper.h"

// State of undress for an NPC
enum class UndressState : uint8_t
{
    Dressed = 0,           // Default - wearing whatever they have
    PartiallyUndressed,    // Outer armor removed, undergarments remain
    FullyUndressed         // Everything removed
};

// Helper namespace for armor classification
namespace UndressHelper
{
    // Armor slots that count as "outer armor" (removed in partial undress)
    // Body, Hands, Feet, Shield, Head, Hair (helmets)
    inline bool IsOuterArmor(RE::TESObjectARMO* armor)
    {
        if (!armor) return false;

        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        using SlotType = std::underlying_type_t<Slot>;

        auto slots = armor->GetSlotMask();

        // Check for body armor slots (30-39 primarily)
        // Slot 30 = Head, 31 = Hair, 32 = Body, 33 = Hands, 37 = Feet, 39 = Shield
        constexpr SlotType outerSlots =
            static_cast<SlotType>(Slot::kHead) |      // 30 - Helmets
            static_cast<SlotType>(Slot::kHair) |      // 31 - Hair/Wigs that are armor
            static_cast<SlotType>(Slot::kBody) |      // 32 - Cuirass/Robes
            static_cast<SlotType>(Slot::kHands) |     // 33 - Gauntlets
            static_cast<SlotType>(Slot::kForearms) |  // 34 - Bracers
            static_cast<SlotType>(Slot::kCalves) |    // 37 - Greaves/Boots
            static_cast<SlotType>(Slot::kShield) |    // 39 - Shields
            static_cast<SlotType>(Slot::kTail);       // 40 - Tails/Cloaks

        return (static_cast<SlotType>(slots) & outerSlots) != 0;
    }

    // Everything else (circlets, rings, amulets, underwear mods, etc.)
    inline bool IsUndergarment(RE::TESObjectARMO* armor)
    {
        return armor && !IsOuterArmor(armor);
    }
}

class UndressManager
{
public:
    static constexpr std::uint32_t kSerializationVersion = 1;
    static constexpr std::uint32_t kUndressRecord = '5UDS';  // 5 + UnDreSs state

    // Reserved outfit name for pre-undress storage
    static constexpr const char* kPreUndressOutfitName = "preundress";

    static UndressManager* GetSingleton()
    {
        static UndressManager instance;
        return &instance;
    }

    // === Undress Operations ===

    // Partial undress: remove outer armor only, store original outfit
    void UndressPartial(RE::Actor* actor)
    {
        if (!actor) return;

        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        auto* outfitMgr = OutfitLockManager::GetSingleton();
        if (!equipManager) {
            spdlog::error("UndressManager::UndressPartial - No ActorEquipManager");
            return;
        }

        spdlog::info("UndressManager::UndressPartial - Partially undressing '{}'", actor->GetName());

        bool wasLocked = outfitMgr->IsLocked(actor);

        // Save original outfit first (only if not already saved)
        SavePreUndressOutfit(actor);

        // Unequip outer armor only — update locked outfit data before each unequip
        auto armors = ItemEquipHelper::GetInventoryItems<RE::TESObjectARMO>(actor);

        for (auto* armor : armors) {
            if (UndressHelper::IsOuterArmor(armor) && ItemEquipHelper::IsArmorEquipped(actor, armor)) {
                outfitMgr->RemoveFromLockedOutfit(actor, armor);
                equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, true);
                spdlog::trace("  - Unequipped outer armor: '{}'", armor->GetFullName());
            }
        }

        // Update state
        SetUndressState(actor, UndressState::PartiallyUndressed);

        // Lock the NPC if this is a fresh undress (wasn't previously locked)
        if (!wasLocked) {
            outfitMgr->Lock(actor);
        }

        spdlog::info("UndressManager::UndressPartial - '{}' is now partially undressed", actor->GetName());
    }

    // Full undress: remove ALL armor
    void UndressFull(RE::Actor* actor)
    {
        if (!actor) return;

        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        auto* outfitMgr = OutfitLockManager::GetSingleton();
        if (!equipManager) {
            spdlog::error("UndressManager::UndressFull - No ActorEquipManager");
            return;
        }

        spdlog::info("UndressManager::UndressFull - Fully undressing '{}'", actor->GetName());

        bool wasLocked = outfitMgr->IsLocked(actor);

        // Save original outfit if not already saved (handles direct full undress)
        SavePreUndressOutfit(actor);

        // Unequip ALL armor — update locked outfit data before each unequip
        auto armors = ItemEquipHelper::GetInventoryItems<RE::TESObjectARMO>(actor);

        for (auto* armor : armors) {
            if (ItemEquipHelper::IsArmorEquipped(actor, armor)) {
                outfitMgr->RemoveFromLockedOutfit(actor, armor);
                equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, true);
                spdlog::trace("  - Unequipped: '{}'", armor->GetFullName());
            }
        }

        // Update state
        SetUndressState(actor, UndressState::FullyUndressed);

        // Lock the NPC if this is a fresh undress (wasn't previously locked)
        if (!wasLocked) {
            outfitMgr->Lock(actor);
        }

        spdlog::info("UndressManager::UndressFull - '{}' is now fully undressed", actor->GetName());
    }

    // Redress: restore pre-undress outfit
    void Redress(RE::Actor* actor)
    {
        if (!actor) return;

        spdlog::info("UndressManager::Redress - Re-dressing '{}'", actor->GetName());

        auto* outfitMgr = OutfitLockManager::GetSingleton();
        bool wasLocked = outfitMgr->IsLocked(actor);

        // Apply saved pre-undress outfit
        if (outfitMgr->HasOutfit(actor, kPreUndressOutfitName)) {
            // Update the locked outfit whitelist to match the target (pre-undress) state
            // so the equip hooks allow the ApplyOutfit calls through
            auto preundressData = outfitMgr->GetOutfitData(actor, kPreUndressOutfitName);
            if (wasLocked && preundressData) {
                outfitMgr->SetLockedOutfitData(actor, *preundressData);
            }

            outfitMgr->ApplyOutfit(actor, kPreUndressOutfitName, true);  // unequipOthers=true
            outfitMgr->DeleteOutfit(actor, kPreUndressOutfitName);
            spdlog::info("  - Restored pre-undress outfit");
        } else {
            spdlog::warn("  - No pre-undress outfit found");
        }

        // Clear undress state (NPC is now "dressed" in whatever they have)
        ClearUndressState(actor);

        spdlog::info("UndressManager::Redress - '{}' is now dressed", actor->GetName());
    }

    // Clear state (called on gear change via menu or redress)
    void ClearUndressState(RE::Actor* actor)
    {
        if (!actor) return;

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_undressStates.find(actor->GetFormID());
        if (it != m_undressStates.end()) {
            m_undressStates.erase(it);
            spdlog::info("UndressManager::ClearUndressState - Cleared state for '{}'", actor->GetName());
        }

        // Also clean up any leftover pre-undress outfit
        OutfitLockManager::GetSingleton()->DeleteOutfit(actor, kPreUndressOutfitName);
    }

    // Query state
    UndressState GetUndressState(RE::Actor* actor) const
    {
        if (!actor) return UndressState::Dressed;

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_undressStates.find(actor->GetFormID());
        return (it != m_undressStates.end()) ? it->second : UndressState::Dressed;
    }

    // Check if actor has any undress state
    bool HasUndressState(RE::Actor* actor) const
    {
        return GetUndressState(actor) != UndressState::Dressed;
    }

    // === Serialization ===

    static void OnGameSave(SKSE::SerializationInterface* a_intfc)
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);

        spdlog::info("UndressManager::OnGameSave - Saving {} undress states", mgr->m_undressStates.size());

        if (!a_intfc->OpenRecord(kUndressRecord, kSerializationVersion)) {
            spdlog::error("UndressManager::OnGameSave - Failed to open record");
            return;
        }

        // Write count
        std::uint32_t count = static_cast<std::uint32_t>(mgr->m_undressStates.size());
        a_intfc->WriteRecordData(&count, sizeof(count));

        // Write each entry
        for (const auto& [actorID, state] : mgr->m_undressStates) {
            a_intfc->WriteRecordData(&actorID, sizeof(actorID));
            a_intfc->WriteRecordData(&state, sizeof(state));

            spdlog::trace("  - Saved state {} for actor 0x{:08X}", static_cast<int>(state), actorID);
        }

        spdlog::info("UndressManager::OnGameSave - Done");
    }

    static void OnGameLoad(SKSE::SerializationInterface* a_intfc)
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);

        mgr->m_undressStates.clear();

        spdlog::info("UndressManager::OnGameLoad - Loading data");

        std::uint32_t type, version, length;
        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kUndressRecord) {
                // Skip unknown records
                if (length > 0) {
                    std::vector<char> skipBuffer(length);
                    a_intfc->ReadRecordData(skipBuffer.data(), length);
                }
                continue;
            }

            if (version != kSerializationVersion) {
                spdlog::warn("UndressManager::OnGameLoad - Version mismatch: {} vs {}", version, kSerializationVersion);
                // Skip this record
                if (length > 0) {
                    std::vector<char> skipBuffer(length);
                    a_intfc->ReadRecordData(skipBuffer.data(), length);
                }
                continue;
            }

            // Read count
            std::uint32_t count = 0;
            a_intfc->ReadRecordData(&count, sizeof(count));

            // Read each entry
            for (std::uint32_t i = 0; i < count; ++i) {
                RE::FormID oldActorID = 0;
                UndressState state = UndressState::Dressed;

                a_intfc->ReadRecordData(&oldActorID, sizeof(oldActorID));
                a_intfc->ReadRecordData(&state, sizeof(state));

                // Resolve FormID for load order changes
                RE::FormID newActorID = 0;
                if (a_intfc->ResolveFormID(oldActorID, newActorID)) {
                    mgr->m_undressStates[newActorID] = state;
                    spdlog::trace("  - Loaded state {} for actor 0x{:08X}", static_cast<int>(state), newActorID);
                } else {
                    spdlog::warn("  - Could not resolve actor 0x{:08X}", oldActorID);
                }
            }
        }

        spdlog::info("UndressManager::OnGameLoad - Loaded {} undress states", mgr->m_undressStates.size());
    }

    static void OnRevert(SKSE::SerializationInterface*)
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);

        spdlog::info("UndressManager::OnRevert - Clearing {} undress states", mgr->m_undressStates.size());
        mgr->m_undressStates.clear();
    }

private:
    UndressManager() = default;
    ~UndressManager() = default;
    UndressManager(const UndressManager&) = delete;
    UndressManager& operator=(const UndressManager&) = delete;

    // Save pre-undress outfit (only if not already saved)
    void SavePreUndressOutfit(RE::Actor* actor)
    {
        auto* outfitMgr = OutfitLockManager::GetSingleton();
        if (!outfitMgr->HasOutfit(actor, kPreUndressOutfitName)) {
            outfitMgr->SaveOutfit(actor, kPreUndressOutfitName);
            spdlog::info("UndressManager::SavePreUndressOutfit - Saved pre-undress outfit for '{}'",
                actor->GetName());
        }
    }

    // Set undress state for an actor
    void SetUndressState(RE::Actor* actor, UndressState state)
    {
        if (!actor) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_undressStates[actor->GetFormID()] = state;
    }

    // Per-actor undress state
    std::unordered_map<RE::FormID, UndressState> m_undressStates;

    // Mutex for thread safety
    mutable std::mutex m_mutex;
};

#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "OutfitLockManager.h"
#include "ItemEquipHelper.h"
#include "DeviceCompat.h"

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

    // Is this a wig?
    //
    // A wig is hair rather than clothing, so neither undress takes one off - the NPC
    // would go bald, which is not what "undress" means. They come off only when the
    // player picks one in the wheel.
    //
    // Slots alone cannot answer this. Slot 31 is the hair slot, but it is also claimed by
    // most hoods, helmets and hats, because covering the head means hiding the hair:
    // across the 2427 plugins in this load order, hair-slot items include "Imperial
    // Helmet", "Mage Hood" and "Colovian Fur Hat". Keeping all of those on would break
    // undress far worse than stripping a wig does.
    //
    // Names alone cannot answer it either: "Lady Ritual Hair Chain" is an accessory worn
    // on slot 42, and "Spellmonger Robe (Black Hair)" is a full robe that happens to bring
    // its own hair.
    //
    // So both have to agree - the name has to read as hair, and the item has to occupy the
    // hair slots and nothing that would make it a garment. On this load order that pairing
    // gets all 413 real wigs and none of the headgear.
    inline bool IsWig(RE::TESObjectARMO* armor)
    {
        if (!armor) return false;

        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        using SlotType = std::underlying_type_t<Slot>;

        constexpr SlotType hairSlots =
            static_cast<SlotType>(Slot::kHair) |       // 31
            static_cast<SlotType>(Slot::kLongHair);    // 41

        // Wearing any of these makes it a garment, whatever it is called.
        constexpr SlotType garmentSlots =
            static_cast<SlotType>(Slot::kHead) |       // 30 - helmets and hoods
            static_cast<SlotType>(Slot::kBody) |       // 32
            static_cast<SlotType>(Slot::kHands) |      // 33
            static_cast<SlotType>(Slot::kForearms) |   // 34
            static_cast<SlotType>(Slot::kFeet) |       // 37
            static_cast<SlotType>(Slot::kCalves) |     // 38
            static_cast<SlotType>(Slot::kShield);      // 39

        const auto slots = static_cast<SlotType>(armor->GetSlotMask());
        if ((slots & hairSlots) == 0) return false;
        if ((slots & garmentSlots) != 0) return false;

        const char* fullName = armor->GetFullName();
        if (!fullName || !*fullName) return false;

        // Mirrors Data\SKSE\Plugins\VRDressup\Categories\01-wigs.json, which is the
        // gallery's definition of a wig. Kept here rather than read from there because
        // undress has to work before the gallery has ever been opened - that JSON is
        // loaded lazily, on first browse, and needs the whole armour cache behind it.
        static constexpr const char* kHairWords[] = {
            "wig", "wigs", "hairdo", "hairdos", "hairstyle", "hairpiece", "peruke",
            "hair", "ponytail", "braid", "braids", "dreadlocks", "updo"
        };
        static constexpr const char* kNotHairWords[] = {
            "band", "headband", "pin", "hairpin", "clip", "ribbon", "net", "hairnet",
            "ornament", "comb", "dye", "tie", "accessory", "brush", "oil"
        };

        std::string name(fullName);
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // Whole words only, so "hair" does not fire on "chair" and "wig" does not fire on
        // "wigwam" - but a name run together as one token, like "LenitayriaWig", still
        // reads as a wig, so that is checked separately below.
        bool isHair = false;
        for (std::size_t start = 0; start <= name.size();) {
            const std::size_t end = name.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyz", start);
            const std::size_t stop = (end == std::string::npos) ? name.size() : end;

            if (stop > start) {
                const std::string_view word(name.data() + start, stop - start);
                for (const char* veto : kNotHairWords) {
                    if (word == veto) return false;
                }
                for (const char* hit : kHairWords) {
                    if (word == hit) isHair = true;
                }
            }

            if (stop >= name.size()) break;
            start = stop + 1;
        }

        return isHair || name.find("wig") != std::string::npos;
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
        if (!equipManager) {
            spdlog::error("UndressManager::UndressPartial - No ActorEquipManager");
            return;
        }

        spdlog::info("UndressManager::UndressPartial - Partially undressing '{}'", actor->GetName());

        // Save original outfit first (only if not already saved)
        SavePreUndressOutfit(actor);

        // Suspend lock enforcement while we make changes
        ScopedLockSuspension suspension(actor);

        // Unequip outer armor only
        auto armors = ItemEquipHelper::GetInventoryItems<RE::TESObjectARMO>(actor);

        for (auto* armor : armors) {
            // The rendered half of a Devious Device sits in an ordinary biped slot, so it
            // looks like outer armour from here. Stripping it leaves DD holding a device
            // that is worn and invisible, which it repairs by putting it straight back.
            // A partial undress leaves devices alone: the inventory half carries no slot,
            // so it is not outer armour, and its rendered half stays with it.
            if (DeviceCompat::IsDevice(armor)) continue;

            // Hair is not clothing - see UndressHelper::IsWig. Wigs come off only when the
            // player picks one in the wheel.
            if (UndressHelper::IsWig(armor)) continue;

            if (UndressHelper::IsOuterArmor(armor) && ItemEquipHelper::IsArmorEquippedOrPending(actor, armor)) {
                equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, true);
                spdlog::trace("  - Unequipped outer armor: '{}'", armor->GetFullName());
            }
        }

        // Update state
        SetUndressState(actor, UndressState::PartiallyUndressed);

        // Lock will be re-applied when suspension goes out of scope (if was locked),
        // or we manually lock if this is a fresh undress. Not for the player: the lock is
        // a grip on an NPC the game would otherwise redress, and nothing redresses the
        // player behind their back.
        if (!suspension.WasLocked() && !actor->IsPlayerRef()) {
            OutfitLockManager::GetSingleton()->Lock(actor);
        }

        spdlog::info("UndressManager::UndressPartial - '{}' is now partially undressed", actor->GetName());
    }

    // Full undress: remove ALL armor
    void UndressFull(RE::Actor* actor)
    {
        if (!actor) return;

        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (!equipManager) {
            spdlog::error("UndressManager::UndressFull - No ActorEquipManager");
            return;
        }

        spdlog::info("UndressManager::UndressFull - Fully undressing '{}'", actor->GetName());

        // Save original outfit if not already saved (handles direct full undress)
        SavePreUndressOutfit(actor);

        // Suspend lock enforcement while we make changes
        ScopedLockSuspension suspension(actor);

        // Unequip ALL armor
        auto armors = ItemEquipHelper::GetInventoryItems<RE::TESObjectARMO>(actor);

        for (auto* armor : armors) {
            // Body parts stay on: TNG owns them and puts them back anyway - see
            // ItemEquipHelper::IsBodyPart.
            if (ItemEquipHelper::IsBodyPart(armor)) continue;

            // The rendered half of a Devious Device comes off with its inventory half,
            // through DD, or not at all.
            if (DeviceCompat::IsRenderedDevice(armor)) continue;

            // Hair stays on even for a full undress - see UndressHelper::IsWig.
            if (UndressHelper::IsWig(armor)) continue;

            if (!ItemEquipHelper::IsArmorEquippedOrPending(actor, armor)) continue;

            if (DeviceCompat::IsInventoryDevice(armor)) {
                // A full undress means everything, devices included - unlocked without a
                // key, because this is the wardrobe tool rather than the player picking at
                // a lock. DD still refuses quest devices, and those stay on.
                if (DeviceCompat::Unequip(actor, armor)) {
                    ItemEquipHelper::NotePendingUnequip(actor, armor);
                } else {
                    spdlog::debug("  - '{}' stays on: Devious Devices will not release it",
                        armor->GetFullName());
                }
                continue;
            }

            equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, true);
            spdlog::trace("  - Unequipped: '{}'", armor->GetFullName());
        }

        // Update state
        SetUndressState(actor, UndressState::FullyUndressed);

        // Lock will be re-applied when suspension goes out of scope (if was locked),
        // or we manually lock if this is a fresh undress. Not for the player: the lock is
        // a grip on an NPC the game would otherwise redress, and nothing redresses the
        // player behind their back.
        if (!suspension.WasLocked() && !actor->IsPlayerRef()) {
            OutfitLockManager::GetSingleton()->Lock(actor);
        }

        spdlog::info("UndressManager::UndressFull - '{}' is now fully undressed", actor->GetName());
    }

    // Redress: restore pre-undress outfit
    void Redress(RE::Actor* actor)
    {
        if (!actor) return;

        spdlog::info("UndressManager::Redress - Re-dressing '{}'", actor->GetName());

        auto* outfitMgr = OutfitLockManager::GetSingleton();

        // Suspend lock enforcement while we make changes
        ScopedLockSuspension suspension(actor);

        // Apply saved pre-undress outfit
        if (outfitMgr->HasOutfit(actor, kPreUndressOutfitName)) {
            outfitMgr->ApplyOutfit(actor, kPreUndressOutfitName, true);  // unequipOthers=true
            outfitMgr->DeleteOutfit(actor, kPreUndressOutfitName);
            spdlog::debug("  - Restored pre-undress outfit");
        } else {
            spdlog::warn("  - No pre-undress outfit found");
        }

        // Clear undress state (NPC is now "dressed" in whatever they have)
        ClearUndressState(actor);

        // Lock will be re-applied when suspension goes out of scope (if was locked)
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

    // Clear state before loading records (called once by central dispatch)
    static void OnPreLoad()
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);
        mgr->m_undressStates.clear();
        spdlog::info("UndressManager::OnPreLoad - Cleared state");
    }

    // Process a single serialization record (called by central dispatch loop)
    static void OnLoadRecord(SKSE::SerializationInterface* a_intfc,
        std::uint32_t type, std::uint32_t version, std::uint32_t length)
    {
        if (type != kUndressRecord) return;

        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);

        if (version != kSerializationVersion) {
            spdlog::warn("UndressManager::OnLoadRecord - Version mismatch: {} vs {}", version, kSerializationVersion);
            if (length > 0) {
                std::vector<char> skipBuffer(length);
                a_intfc->ReadRecordData(skipBuffer.data(), length);
            }
            return;
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

        spdlog::info("UndressManager::OnLoadRecord - Loaded {} undress states", mgr->m_undressStates.size());
    }

    static void OnGameLoad(SKSE::SerializationInterface* a_intfc)
    {
        OnPreLoad();

        std::uint32_t type, version, length;
        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            OnLoadRecord(a_intfc, type, version, length);
        }
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

    // Remember what to put back on a redress. Called by both undress paths, so it has to
    // cope with being handed an NPC who is already half or fully undressed.
    void SavePreUndressOutfit(RE::Actor* actor)
    {
        if (!actor) return;

        auto* outfitMgr = OutfitLockManager::GetSingleton();

        std::vector<std::string> wornKeys;
        for (auto* armor : ItemEquipHelper::GetInventoryItems<RE::TESObjectARMO>(actor)) {
            if (!armor || !ItemEquipHelper::IsArmorEquippedOrPending(actor, armor)) continue;
            if (ItemEquipHelper::IsBodyPart(armor)) continue;
            // Same rule as the outfit snapshot: a device is remembered by its inventory
            // half, and the rendered half comes back with it.
            if (DeviceCompat::IsRenderedDevice(armor)) continue;
            std::string formKey = Persistence::FormKeyUtil::BuildFormKey(armor);
            if (!formKey.empty()) {
                wornKeys.push_back(std::move(formKey));
            }
        }

        if (wornKeys.empty()) {
            // Undressing someone who is already undressed. There is nothing to remember,
            // and writing an empty snapshot over a good one is how a redress ended up
            // restoring nothing at all.
            spdlog::info("UndressManager::SavePreUndressOutfit - '{}' has nothing on, keeping "
                "whatever pre-undress outfit is already stored", actor->GetName());
            return;
        }

        if (!outfitMgr->HasOutfit(actor, kPreUndressOutfitName)) {
            outfitMgr->SaveOutfit(actor, kPreUndressOutfitName);
            spdlog::info("UndressManager::SavePreUndressOutfit - Saved pre-undress outfit for "
                "'{}' ({} item(s))", actor->GetName(), wornKeys.size());
            return;
        }

        // A snapshot already exists, from an earlier partial undress. Fold in what is still
        // worn instead of leaving it alone: those pieces are what this undress is about to
        // remove, and only what is in here comes back on a redress.
        std::vector<std::string> merged = outfitMgr->GetOutfitItemFormKeys(actor, kPreUndressOutfitName);
        for (const auto& formKey : wornKeys) {
            if (std::find(merged.begin(), merged.end(), formKey) == merged.end()) {
                merged.push_back(formKey);
            }
        }

        const std::uint32_t accepted =
            outfitMgr->SetOutfitFromFormKeys(actor, kPreUndressOutfitName, merged);
        spdlog::info("UndressManager::SavePreUndressOutfit - Extended pre-undress outfit for "
            "'{}' to {} item(s)", actor->GetName(), accepted);
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

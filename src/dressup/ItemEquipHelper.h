#pragma once

#include <RE/Skyrim.h>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
// Spelled relative to this file, not to whoever includes it: a .cpp in this folder has no
// src/ in its include chain, so a bare "log.h" only resolves for includers up in src/.
#include "../log.h"
#include "DeviceCompat.h"

// Helper namespace for unified armor/weapon equipment operations
namespace ItemEquipHelper
{
    // Get inventory items of a specific type from an actor
    template<typename T>
    std::vector<T*> GetInventoryItems(RE::Actor* actor)
    {
        std::vector<T*> result;
        if (!actor) return result;

        RE::FormType formType;
        if constexpr (std::is_same_v<T, RE::TESObjectARMO>) {
            formType = RE::FormType::Armor;
        } else if constexpr (std::is_same_v<T, RE::TESObjectWEAP>) {
            formType = RE::FormType::Weapon;
        } else {
            static_assert(std::is_same_v<T, RE::TESObjectARMO> || std::is_same_v<T, RE::TESObjectWEAP>,
                "ItemEquipHelper only supports Armor and Weapon types");
        }

        auto inventory = actor->GetInventory([formType](RE::TESBoundObject& obj) {
            return obj.Is(formType);
        });

        for (const auto& [item, data] : inventory) {
            if (data.first <= 0) continue;  // Skip items with zero count

            auto* typedItem = item->As<T>();
            if (typedItem) {
                result.push_back(typedItem);
            }
        }

        return result;
    }

    // Check if actor has item in inventory
    inline bool HasItemInInventory(RE::Actor* actor, RE::TESBoundObject* item)
    {
        if (!actor || !item) return false;

        auto inventory = actor->GetInventory([item](RE::TESBoundObject& obj) {
            return obj.GetFormID() == item->GetFormID();
        });

        return !inventory.empty();
    }

    // Get model path for an item. Female-only gear - wigs, lingerie - ships with an empty
    // male model, and an empty path draws nothing at all in the wheel, so fall back.
    inline std::string GetModelPath(RE::TESObjectARMO* armor)
    {
        if (!armor) return "";

        const char* modelPath = armor->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
        if (!modelPath || !*modelPath) {
            modelPath = armor->worldModels[RE::TESBipedModelForm::Sexes::kFemale].GetModel();
        }
        return modelPath ? modelPath : "";
    }

    inline std::string GetModelPath(RE::TESObjectWEAP* weapon)
    {
        if (!weapon) return "";
        const char* modelPath = weapon->GetModel();
        return modelPath ? modelPath : "";
    }

    // Whether this armour has a mesh to render on a body of the given sex.
    //
    // Not the ARMO's own world models - those are the ground model, what the item looks
    // like lying on a table. What a worn armour actually renders comes from its armour
    // addons, each of which carries a male and a female mesh, and female-only gear - wigs,
    // lingerie - leaves the male field of every one of them empty.
    inline bool HasWornMeshForSex(RE::TESObjectARMO* armor, RE::SEX sex)
    {
        if (!armor) return false;

        const auto slot = (sex == RE::SEXES::kFemale) ? RE::SEXES::kFemale : RE::SEXES::kMale;
        for (auto* addon : armor->armorAddons) {
            if (!addon) continue;
            const char* model = addon->bipedModels[slot].GetModel();
            if (model && *model) return true;
        }
        return false;
    }

    // Whether this armour is worth offering for a particular actor. Female-only gear put on
    // a man occupies the biped slot and draws nothing - he ends up "wearing" a wig that is
    // not there, and the category it came from counts it and then looks broken.
    //
    // Deliberately one-way: it only ever hides gear from a male actor. The engine falls back
    // to an addon's male mesh when the female one is empty, but not the other way round, so
    // the very many mod armours that fill only the male slot do render on women and hiding
    // them there would throw away most of the wardrobe. Only the male direction is a hole.
    //
    // An armour with no mesh for either sex is NOT hidden either. That one is broken for
    // everybody, so hiding it from one actor and not the next would be the more confusing
    // answer; GetModelPath's fallback and the category build already drop the ones that
    // have nothing to draw at all.
    inline bool FitsActor(RE::TESObjectARMO* armor, RE::Actor* actor)
    {
        if (!armor) return false;
        if (!actor) return true;

        auto* base = actor->GetActorBase();
        if (!base || base->IsFemale()) return true;

        if (HasWornMeshForSex(armor, RE::SEXES::kMale)) return true;
        return !HasWornMeshForSex(armor, RE::SEXES::kFemale);
    }

    // Is The New Gentleman in this load order? Answered once, because the only way to ask
    // walks the whole file list and IsBodyPart below runs per armour per snapshot.
    //
    // Deliberately not cached until the data handler exists, so a call made before load
    // cannot pin the wrong answer for the session.
    inline bool IsTngLoaded()
    {
        static int cached = -1;  // -1 unknown, 0 no, 1 yes
        if (cached >= 0) return cached == 1;

        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) return false;

        cached = (handler->LookupModByName("TheNewGentleman.esp") ||
                  handler->LookupModByName("TheNewGentleman.esm")) ? 1 : 0;
        return cached == 1;
    }

    // Biped slot 52 is where The New Gentleman - and SOS before it - puts its genital
    // addons. Those are body parts rather than clothing: TNG equips and unequips them
    // itself in reaction to what the NPC is wearing, so anything we do with them is
    // fighting the mod that owns them. Worse, they turn up worn a frame or two after the
    // outfit that triggered them, so a snapshot can catch one by accident and then keep
    // putting it back on long after the player removed it in the TNG menu.
    //
    // But slot 52 is not TNG's alone, and testing the slot by itself was wrong far more
    // often than it was right. Underwear mods park panties and briefs there precisely
    // because nothing else claims it: "Mage Novice Panty" (0xB56~[Caenarvon] Magecore.esp)
    // is BOD2 = 0x00400000, slot 52 and nothing else - byte for byte what TNG_GenitalCover
    // (0xAFF~TheNewGentleman.esp) looks like. Every such piece was invisible to the outfit
    // system: never stored in 'locked' or 'preundress', so never put back by a reapply or
    // a redress, while still being destroyed as a gallery item when it came off. The
    // player took an NPC's underwear off once and it was gone.
    //
    // So the slot is the precondition and TNG's own keywords are the test. TNG tags each
    // addon with TNG_AddonMale / TNG_AddonFemale and its cover with TNG_Ignored; nothing
    // else on slot 52 carries those. Without TNG installed there is no such tag to read
    // and nothing of TNG's to protect, so the slot alone has to do - that is the old
    // behaviour, kept for load orders running bare SOS.
    inline bool IsBodyPart(RE::TESObjectARMO* armor)
    {
        constexpr auto kBodyPartSlots = static_cast<std::uint32_t>(1u << (52 - 30));

        if (!armor) return false;
        if ((static_cast<std::uint32_t>(armor->GetSlotMask()) & kBodyPartSlots) == 0) {
            return false;
        }

        if (!IsTngLoaded()) return true;

        return armor->HasKeywordString("TNG_AddonMale") ||
               armor->HasKeywordString("TNG_AddonFemale") ||
               armor->HasKeywordString("TNG_Ignored");
    }

    // Is this exact item worn?
    //
    // Asked of the item's own inventory entry rather than through
    // Actor::GetWornArmor(slotMask), which answers "what is worn in these slots" and only
    // then compares. That indirection got two cases wrong:
    //
    //   * items with no biped slot at all answer no forever, because the slot query
    //     matches on slot bits and there are none to match. Every Devious Devices
    //     inventory device is such an item (BOD2 = 0), so a worn device read as not worn
    //     and got re-equipped on every outfit reapply.
    //   * where two worn items share a slot, the query returns whichever comes first in
    //     the entry list, so the other one reads as not worn.
    //
    // The worn flag lives on the entry's extra data, which is where the engine actually
    // records it, and is right for both.
    inline bool IsArmorEquipped(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;

        auto* changes = actor->GetInventoryChanges();
        if (!changes || !changes->entryList) {
            // No inventory changes yet - nothing has been equipped or moved on this
            // actor, so the base outfit is all there is. Fall back to the slot query.
            auto* wornArmor = actor->GetWornArmor(armor->GetSlotMask());
            return wornArmor && wornArmor->GetFormID() == armor->GetFormID();
        }

        const auto formID = armor->GetFormID();
        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object || entry->object->GetFormID() != formID) continue;
            return entry->IsWorn();
        }

        return false;
    }

    // Check if weapon is equipped (in either hand)
    inline bool IsWeaponEquipped(RE::Actor* actor, RE::TESObjectWEAP* weapon)
    {
        if (!actor || !weapon) return false;
        RE::TESForm* rightHand = actor->GetEquippedObject(false);
        RE::TESForm* leftHand = actor->GetEquippedObject(true);

        bool inRight = rightHand && rightHand->GetFormID() == weapon->GetFormID();
        bool inLeft = leftHand && leftHand->GetFormID() == weapon->GetFormID();

        return inRight || inLeft;
    }

    // Get what's currently worn in an armor's slot
    inline RE::TESObjectARMO* GetWornInSlot(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return nullptr;
        return actor->GetWornArmor(armor->GetSlotMask());
    }

    // Get currently equipped weapon in right hand
    inline RE::TESObjectWEAP* GetEquippedWeaponRight(RE::Actor* actor)
    {
        if (!actor) return nullptr;
        RE::TESForm* rightHand = actor->GetEquippedObject(false);
        return rightHand ? rightHand->As<RE::TESObjectWEAP>() : nullptr;
    }

    // Get currently equipped weapon in left hand
    inline RE::TESObjectWEAP* GetEquippedWeaponLeft(RE::Actor* actor)
    {
        if (!actor) return nullptr;
        RE::TESForm* leftHand = actor->GetEquippedObject(true);
        return leftHand ? leftHand->As<RE::TESObjectWEAP>() : nullptr;
    }

    // Get both equipped weapons (right first, then left if different)
    inline std::vector<RE::TESObjectWEAP*> GetEquippedWeapons(RE::Actor* actor)
    {
        std::vector<RE::TESObjectWEAP*> result;
        if (!actor) return result;

        auto* rightWeapon = GetEquippedWeaponRight(actor);
        auto* leftWeapon = GetEquippedWeaponLeft(actor);

        if (rightWeapon) {
            result.push_back(rightWeapon);
        }
        if (leftWeapon && (!rightWeapon || leftWeapon->GetFormID() != rightWeapon->GetFormID())) {
            result.push_back(leftWeapon);
        }

        return result;
    }

    // === Equips and unequips the engine has not applied yet ===
    //
    // Every equip here goes out with a_queueEquip = true, so the engine does not apply it in
    // the call: it lands a frame or more later, and until it does the actor's inventory
    // changes still say the item is not worn.
    //
    // So anything that snapshots what an actor is wearing in the same breath as dressing
    // them misses the piece it just put on. That is how the outfit lock came to strip a
    // freshly equipped item: the re-save that runs when the edit scope closes did not
    // contain it, and the reapply 750ms later found a piece that was not in the outfit and
    // took it off again. Verified in the log against Sybille Stentor and a Bandolier - the
    // save immediately after the equip listed 5 items, none of them the bandolier, and
    // UnequipArmorExcept removed it 782ms later.
    //
    // Removals have the mirror image of the problem, for a different reason. An engine
    // unequip does take effect in the call, but a Devious Devices one does not: it is a
    // zadlibs.UnlockDevice dispatched into the Papyrus VM, and DD gets round to clearing
    // the worn flag some hundreds of milliseconds later. A snapshot taken in between still
    // counts the device as worn, so it went back into the locked outfit and the reapply put
    // it straight back on - the player took a piercing off and the lock gave it back.
    // Verified in the log against Sybille Stentor and a Genital Piercing at 15:28:21.
    //
    // The intent is therefore recorded here, at the places that issue the change, for
    // readers that want "what is this actor going to be wearing" rather than "what is the
    // engine showing this instant". Recording one direction cancels the other, so an item
    // taken off and put back on again inside the window reads as on.
    namespace Pending
    {
        // Long enough for a round trip through DD's Papyrus to reach the actor even on a
        // loaded frame, short enough that a change which never lands cannot pin a stale
        // item into an outfit. Removals only - see kEquipGrace for the other direction.
        constexpr auto kLifetime = std::chrono::seconds(5);

        // How long a queued equip is believed before the engine's answer is taken as
        // final. Measured: an equip issued at 14:26:56.513 was worn by 14:26:57.033, so a
        // loaded frame wants most of a second; a queued equip that has not landed inside
        // this window is one the engine has decided against rather than one still moving.
        //
        // This used to be kLifetime, and a flat timer was wrong in the one case that
        // mattered most. An equip the engine silently drops - because the biped slot it
        // wants is held by something the engine is not going to take off, or because the
        // item is not in the inventory at all - stayed "in flight" for a full five
        // seconds, and everything reading intent rather than truth called the piece worn
        // for all of it. The wheel lit the plate; the next click read as "take it off";
        // the toggle asked the engine instead, got "not worn", and equipped it again.
        // Every click did that, and the piece never moved. Daegon's Elven Sentry Cuirass
        // and Sabatons spent twenty seconds that way.
        constexpr auto kEquipGrace = std::chrono::milliseconds(1000);

        using Clock = std::chrono::steady_clock;

        struct Registry
        {
            std::mutex mutex;
            std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, Clock::time_point>> byActor;
        };

        inline Registry& Equips()
        {
            static Registry registry;
            return registry;
        }

        inline Registry& Unequips()
        {
            static Registry registry;
            return registry;
        }

        inline void Forget(Registry& registry, RE::FormID actorID, RE::FormID armorID)
        {
            std::lock_guard<std::mutex> lock(registry.mutex);
            const auto actorEntry = registry.byActor.find(actorID);
            if (actorEntry != registry.byActor.end()) {
                actorEntry->second.erase(armorID);
            }
        }

        // Whether this armour is still in flight. Expired entries are dropped on the way.
        inline bool Holds(Registry& registry, RE::FormID actorID, RE::FormID armorID)
        {
            std::lock_guard<std::mutex> lock(registry.mutex);
            const auto actorEntry = registry.byActor.find(actorID);
            if (actorEntry == registry.byActor.end()) return false;

            const auto item = actorEntry->second.find(armorID);
            if (item == actorEntry->second.end()) return false;

            if (Clock::now() - item->second > kLifetime) {
                actorEntry->second.erase(item);
                return false;
            }
            return true;
        }
    }

    // Record that we have asked the engine to put this armour on. Any earlier pending item
    // sharing a biped slot with it is dropped: the engine will have taken that one off to
    // make room, so keeping both would make a snapshot claim the actor wears two things in
    // one slot and the reapply fight itself.
    inline void NotePendingEquip(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return;

        // Whatever this used to be, it is going back on.
        Pending::Forget(Pending::Unequips(), actor->GetFormID(), armor->GetFormID());

        const auto slots = static_cast<std::uint32_t>(armor->GetSlotMask());

        auto& registry = Pending::Equips();
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto& items = registry.byActor[actor->GetFormID()];

        for (auto it = items.begin(); it != items.end();) {
            auto* other = RE::TESForm::LookupByID<RE::TESObjectARMO>(it->first);
            const bool overlaps = other &&
                (static_cast<std::uint32_t>(other->GetSlotMask()) & slots) != 0;
            it = (!other || overlaps) ? items.erase(it) : std::next(it);
        }

        items[armor->GetFormID()] = Pending::Clock::now();
    }

    inline void ClearPendingEquip(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return;
        Pending::Forget(Pending::Equips(), actor->GetFormID(), armor->GetFormID());
    }

    // Record that we have asked for this armour to come off and are waiting on somebody
    // else to do it - in practice Devious Devices, whose removal path runs in Papyrus.
    inline void NotePendingUnequip(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return;

        ClearPendingEquip(actor, armor);

        auto& registry = Pending::Unequips();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.byActor[actor->GetFormID()][armor->GetFormID()] = Pending::Clock::now();
    }

    // Whether a queued equip is still worth believing.
    //
    // Inside the grace the engine has not had a fair chance and the intent stands. Past
    // it the engine's answer is final either way, so the marker is dropped: if the equip
    // landed the worn flag says so and the marker has nothing left to add, and if it did
    // not the marker was a lie. Callers all fold this together with IsArmorEquipped, so
    // dropping it never loses a piece that is genuinely on.
    inline bool PendingEquipInFlight(RE::Actor* actor, RE::TESObjectARMO* armor,
        Pending::Clock::time_point issued)
    {
        if (Pending::Clock::now() - issued <= Pending::kEquipGrace) return true;

        ClearPendingEquip(actor, armor);
        return false;
    }

    inline bool IsPendingEquip(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;

        // Read the timestamp out under the lock and judge it outside: the verdict can
        // erase the entry, and the registry mutex is not recursive.
        Pending::Clock::time_point issued;
        {
            auto& registry = Pending::Equips();
            std::lock_guard<std::mutex> lock(registry.mutex);
            const auto actorEntry = registry.byActor.find(actor->GetFormID());
            if (actorEntry == registry.byActor.end()) return false;

            const auto item = actorEntry->second.find(armor->GetFormID());
            if (item == actorEntry->second.end()) return false;
            issued = item->second;
        }

        return PendingEquipInFlight(actor, armor, issued);
    }

    inline bool IsPendingUnequip(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;
        return Pending::Holds(Pending::Unequips(), actor->GetFormID(), armor->GetFormID());
    }

    // Armour this actor has been told to put on and the engine has not reported yet.
    // Settled and unresolvable entries are pruned on the way out.
    inline std::vector<RE::TESObjectARMO*> GetPendingEquips(RE::Actor* actor)
    {
        std::vector<RE::TESObjectARMO*> result;
        if (!actor) return result;

        // Snapshot under the lock, judge outside it: the verdict re-enters the registry
        // to drop entries the engine has settled, and the mutex is not recursive.
        std::vector<std::pair<RE::TESObjectARMO*, Pending::Clock::time_point>> candidates;
        {
            auto& registry = Pending::Equips();
            std::lock_guard<std::mutex> lock(registry.mutex);
            const auto actorEntry = registry.byActor.find(actor->GetFormID());
            if (actorEntry == registry.byActor.end()) return result;

            auto& items = actorEntry->second;
            for (auto it = items.begin(); it != items.end();) {
                auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(it->first);
                if (!armor) {
                    it = items.erase(it);
                    continue;
                }
                candidates.emplace_back(armor, it->second);
                ++it;
            }
        }

        for (const auto& [armor, issued] : candidates) {
            if (PendingEquipInFlight(actor, armor, issued)) {
                result.push_back(armor);
            }
        }

        return result;
    }

    // What this actor is going to be wearing, for this one item. IsArmorEquipped answers
    // for the frame the engine is showing; this folds in the changes we have asked for and
    // are still waiting on, which is what anything drawing the player's own last click - a
    // highlight in the wheel, say - has to read instead.
    inline bool IsArmorEquippedOrPending(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;
        if (IsPendingUnequip(actor, armor)) return false;
        if (IsPendingEquip(actor, armor)) return true;
        return IsArmorEquipped(actor, armor);
    }

    // Transfer item from one actor to another
    inline void TransferItem(RE::Actor* from, RE::Actor* to, RE::TESBoundObject* item, int count = 1)
    {
        if (!from || !to || !item) return;
        from->RemoveItem(item, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, to);
    }

    // Equip item on actor
    inline void EquipItem(RE::Actor* actor, RE::TESBoundObject* item)
    {
        if (!actor || !item) return;
        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (equipManager) {
            equipManager->EquipObject(actor, item, nullptr, 1, nullptr, true, false, false);
        }
    }

    // Unequip item from actor
    inline void UnequipItem(RE::Actor* actor, RE::TESBoundObject* item)
    {
        if (!actor || !item) return;
        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (equipManager) {
            equipManager->UnequipObject(actor, item, nullptr, 1, nullptr, false, true);
        }
    }

    // Put a piece of armour on, using whatever mechanism owns it.
    //
    // A Devious Devices inventory device is not equippable by hand: the engine will happily
    // set the worn flag, but DD's OnEquipped is what pairs it with its rendered half, and
    // driving it from outside makes DD re-run that whole sequence to repair the mismatch.
    // Ask DD to lock the device on instead. Rendered devices are never ours to equip at all.
    inline void EquipArmor(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return;

        if (DeviceCompat::IsRenderedDevice(armor)) {
            spdlog::trace("ItemEquipHelper::EquipArmor - '{}' (0x{:08X}) is a rendered device; "
                "leaving it to Devious Devices", armor->GetFullName(), armor->GetFormID());
            return;
        }

        if (DeviceCompat::IsInventoryDevice(armor)) {
            DeviceCompat::Equip(actor, armor);
            NotePendingEquip(actor, armor);
            return;
        }

        EquipItem(actor, armor);
        NotePendingEquip(actor, armor);
    }

    // Take a piece of armour off. Same reasoning as EquipArmor: a device comes off through
    // DD's unlock path, which removes the rendered half, stops its effects and fires the
    // events other mods listen for. Returns false when the item is still on afterwards -
    // a quest device DD refuses to remove.
    inline bool UnequipArmor(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;

        if (DeviceCompat::IsRenderedDevice(armor)) {
            spdlog::trace("ItemEquipHelper::UnequipArmor - '{}' (0x{:08X}) is a rendered device; "
                "leaving it to Devious Devices", armor->GetFullName(), armor->GetFormID());
            return false;
        }

        ClearPendingEquip(actor, armor);

        if (DeviceCompat::IsInventoryDevice(armor)) {
            // DD's removal runs in Papyrus and clears the worn flag long after this
            // returns, so the intent is recorded for anything snapshotting in between.
            if (!DeviceCompat::Unequip(actor, armor)) return false;
            NotePendingUnequip(actor, armor);
            return true;
        }

        UnequipItem(actor, armor);
        return true;
    }

    // Toggle equip state for armor on actor
    inline bool ToggleArmorEquip(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;

        // Asked of the intent, not of the engine, so that the click acts on the state the
        // player can see. The wheel tints its plates from IsArmorEquippedOrPending; a
        // toggle reading the worn flag instead disagreed with the plate for as long as an
        // equip was in flight, and a click on a lit plate came out as another equip
        // rather than the removal it looked like. A second click inside the window now
        // cancels the equip, which is what clicking a lit plate means.
        bool wasEquipped = IsArmorEquippedOrPending(actor, armor);

        if (wasEquipped) {
            if (!UnequipArmor(actor, armor)) {
                // Refused - a locked-on quest device, or a rendered half we do not drive.
                // It is still worn, and saying otherwise would leave the wheel out of sync.
                return true;
            }
            spdlog::info("ItemEquipHelper::ToggleArmorEquip - Unequipped '{}' from {}",
                armor->GetFullName(), actor->GetName());
        } else {
            EquipArmor(actor, armor);
            spdlog::info("ItemEquipHelper::ToggleArmorEquip - Equipped '{}' on {}",
                armor->GetFullName(), actor->GetName());
        }

        return !wasEquipped;  // Return new equipped state
    }

    // Toggle equip state for weapon on actor
    inline bool ToggleWeaponEquip(RE::Actor* actor, RE::TESObjectWEAP* weapon)
    {
        if (!actor || !weapon) return false;

        bool wasEquipped = IsWeaponEquipped(actor, weapon);

        if (wasEquipped) {
            UnequipItem(actor, weapon);
            spdlog::info("ItemEquipHelper::ToggleWeaponEquip - Unequipped '{}' from {}",
                weapon->GetFullName(), actor->GetName());
        } else {
            EquipItem(actor, weapon);
            spdlog::info("ItemEquipHelper::ToggleWeaponEquip - Equipped '{}' on {}",
                weapon->GetFullName(), actor->GetName());
        }

        return !wasEquipped;  // Return new equipped state
    }
}

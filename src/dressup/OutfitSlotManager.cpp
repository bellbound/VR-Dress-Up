#include "OutfitSlotManager.h"
#include "OutfitLockManager.h"
#include "UndressManager.h"
#include "WeaponLockManager.h"
#include "ItemEquipHelper.h"
#include "FormKeyUtil.h"

#include <algorithm>
#include <charconv>
#include <spdlog/spdlog.h>

namespace
{
    // Sorted, so two stored outfits can be compared as sets.
    std::vector<std::string> SortedKeys(RE::Actor* actor, const std::string& outfitName)
    {
        auto keys = OutfitLockManager::GetSingleton()->GetOutfitItemFormKeys(actor, outfitName);
        std::sort(keys.begin(), keys.end());
        return keys;
    }
}

std::string OutfitSlotManager::NameOf(std::uint32_t id)
{
    return std::string(kSlotPrefix) + std::to_string(id);
}

std::vector<OutfitSlotManager::Slot> OutfitSlotManager::List(RE::Actor* actor) const
{
    std::vector<Slot> slots;
    if (!actor) return slots;

    const std::string_view prefix = kSlotPrefix;
    for (const auto& name : OutfitLockManager::GetSingleton()->EnumerateOutfitNames(actor)) {
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }

        std::uint32_t id = 0;
        const char* first = name.data() + prefix.size();
        const char* last = name.data() + name.size();
        const auto parsed = std::from_chars(first, last, id);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            spdlog::warn("OutfitSlotManager::List - Ignoring oddly named outfit '{}'", name);
            continue;
        }

        slots.push_back({id, name});
    }

    std::sort(slots.begin(), slots.end(),
        [](const Slot& a, const Slot& b) { return a.id < b.id; });
    return slots;
}

std::optional<std::uint32_t> OutfitSlotManager::Worn(RE::Actor* actor) const
{
    auto* lockMgr = OutfitLockManager::GetSingleton();
    if (!actor || !lockMgr->IsLocked(actor)) {
        return std::nullopt;
    }

    const auto locked = SortedKeys(actor, "locked");
    const bool handsEmpty = WeaponLockManager::GetSingleton()->IsEnforcing(actor);

    // Two identical snapshots: the older one wins. Nothing distinguishes them anyway.
    for (const auto& slot : List(actor)) {
        if (lockMgr->GetOutfitNoWeapon(actor, slot.name) != handsEmpty) continue;
        if (SortedKeys(actor, slot.name) == locked) {
            return slot.id;
        }
    }
    return std::nullopt;
}

bool OutfitSlotManager::IsDefault(RE::Actor* actor) const
{
    return actor && !OutfitLockManager::GetSingleton()->IsLocked(actor);
}

bool OutfitSlotManager::SelectDefault(RE::Actor* actor)
{
    if (!actor) return false;

    spdlog::info("OutfitSlotManager::SelectDefault - '{}' goes back to the game's outfit",
        actor->GetName());

    // The look an undress would re-dress into is about to be replaced; the button goes back
    // to "Undress" with it.
    UndressManager::GetSingleton()->ClearUndressState(actor);

    // Unlock also releases the weapon lock, so the hands half needs nothing here.
    return OutfitLockManager::GetSingleton()->Unlock(actor);
}

bool OutfitSlotManager::Select(RE::Actor* actor, std::uint32_t id)
{
    if (!actor) return false;

    auto* lockMgr = OutfitLockManager::GetSingleton();
    const std::string name = NameOf(id);
    if (!lockMgr->HasOutfit(actor, name)) {
        spdlog::warn("OutfitSlotManager::Select - '{}' has no outfit {}", actor->GetName(), id);
        return false;
    }

    spdlog::info("OutfitSlotManager::Select - Putting outfit {} on '{}'", id, actor->GetName());

    UndressManager::GetSingleton()->ClearUndressState(actor);

    if (!lockMgr->AdoptStoredOutfit(actor, name)) {
        return false;
    }

    ApplyHands(actor, lockMgr->GetOutfitNoWeapon(actor, name));
    return true;
}

std::uint32_t OutfitSlotManager::SaveCurrent(RE::Actor* actor)
{
    auto* lockMgr = OutfitLockManager::GetSingleton();

    std::uint32_t id = 0;
    for (const auto& slot : List(actor)) {
        id = (std::max)(id, slot.id + 1);
    }
    const std::string name = NameOf(id);

    // The locked set is the look we are defending - what the player put together, minus
    // anything another mod slipped on while the lock was backed off. Only an unlocked NPC
    // is snapshotted off their body, and for them what they wear *is* the look.
    if (lockMgr->IsLocked(actor)) {
        lockMgr->CopyOutfit(actor, "locked", name);
    } else {
        lockMgr->SaveOutfit(actor, name);
    }
    lockMgr->SetOutfitNoWeapon(actor, name, HandsEmpty(actor));

    spdlog::info("OutfitSlotManager::SaveCurrent - Saved '{}' as outfit {} ({} item(s), noWeapon={})",
        actor->GetName(), id, lockMgr->GetOutfitItemFormKeys(actor, name).size(),
        lockMgr->GetOutfitNoWeapon(actor, name));

    // On, and locked to it. For an already-locked NPC this moves nothing - the set is the
    // one they wear - but it is what makes the new outfit the one being edited.
    Select(actor, id);
    return id;
}

bool OutfitSlotManager::Remove(RE::Actor* actor, std::uint32_t id)
{
    if (!actor) return false;

    const std::string name = NameOf(id);
    auto* lockMgr = OutfitLockManager::GetSingleton();
    if (!lockMgr->HasOutfit(actor, name)) {
        return false;
    }

    if (Worn(actor) == id) {
        // Off the body first, and into another saved look if there is one - the one before
        // it in the row, else the one after. Only the last outfit hands them back to the
        // game. Either way the deleted outfit's pieces stay in the inventory.
        const auto slots = List(actor);
        std::optional<std::uint32_t> next;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].id != id) continue;
            if (i > 0) {
                next = slots[i - 1].id;
            } else if (i + 1 < slots.size()) {
                next = slots[i + 1].id;
            }
            break;
        }

        if (next) {
            Select(actor, *next);
        } else {
            SelectDefault(actor);
        }
    }

    spdlog::info("OutfitSlotManager::Remove - Deleting outfit {} of '{}'", id, actor->GetName());
    return lockMgr->DeleteOutfit(actor, name);
}

void OutfitSlotManager::SyncEdited(RE::Actor* actor, std::uint32_t id)
{
    if (!actor) return;

    auto* lockMgr = OutfitLockManager::GetSingleton();
    const std::string name = NameOf(id);
    if (!lockMgr->HasOutfit(actor, name)) {
        return;
    }

    // Nothing to write into it from an unlocked actor: an edit locks them, so this only
    // happens when the edit was undone by an unlock in between.
    if (!lockMgr->IsLocked(actor)) {
        return;
    }

    lockMgr->CopyOutfit(actor, "locked", name);
    // The menu's weapon clicks switch enforcement on and off as they go (see
    // NoteMenuWeaponChange), so this is the intent of the click, not the state of the
    // hands a frame later.
    lockMgr->SetOutfitNoWeapon(actor, name, WeaponLockManager::GetSingleton()->IsEnforcing(actor));

    spdlog::debug("OutfitSlotManager::SyncEdited - Outfit {} of '{}' now holds {} item(s), noWeapon={}",
        id, actor->GetName(), lockMgr->GetOutfitItemFormKeys(actor, name).size(),
        lockMgr->GetOutfitNoWeapon(actor, name));
}

RE::TESObjectARMO* OutfitSlotManager::Representative(RE::Actor* actor, std::uint32_t id) const
{
    if (!actor) return nullptr;

    std::vector<RE::TESObjectARMO*> pieces;
    for (const auto& key : OutfitLockManager::GetSingleton()->GetOutfitItemFormKeys(actor, NameOf(id))) {
        auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(
            Persistence::FormKeyUtil::ResolveToRuntimeFormID(key));
        if (armor) pieces.push_back(armor);
    }
    if (pieces.empty()) return nullptr;

    // The piece that says most about the look first: the body, then the head, then the
    // rest of the armour slots, then whatever is left - an amulet says little.
    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    static constexpr Slot kPreferred[] = {
        Slot::kBody, Slot::kHead, Slot::kHair, Slot::kFeet, Slot::kHands, Slot::kCalves,
        Slot::kForearms, Slot::kShield, Slot::kTail, Slot::kCirclet,
    };
    for (const Slot slot : kPreferred) {
        for (auto* armor : pieces) {
            if ((static_cast<std::uint32_t>(armor->GetSlotMask()) &
                 static_cast<std::uint32_t>(slot)) != 0) {
                return armor;
            }
        }
    }
    return pieces.front();
}

void OutfitSlotManager::ApplyHands(RE::Actor* actor, bool noWeapon) const
{
    auto* weaponLock = WeaponLockManager::GetSingleton();

    if (!noWeapon) {
        // Whatever they hold, or pick up, is theirs. We do not hand a weapon back: an
        // outfit does not remember which one, and the NPC's own AI re-arms them from their
        // inventory when it wants to.
        weaponLock->StopEnforcing(actor);
        return;
    }

    // Same as the player taking the last weapon off in the wheel: off now, and kept off.
    for (auto* weapon : ItemEquipHelper::GetEquippedWeapons(actor)) {
        if (weapon && weapon->GetWeaponType() != RE::WEAPON_TYPE::kHandToHandMelee) {
            ItemEquipHelper::UnequipItem(actor, weapon);
        }
    }
    weaponLock->Enforce(actor);
}

bool OutfitSlotManager::HandsEmpty(RE::Actor* actor)
{
    // Either we are already keeping them empty, or they simply are.
    return WeaponLockManager::GetSingleton()->IsEnforcing(actor) ||
           !WeaponLockManager::HoldsWeapon(actor);
}

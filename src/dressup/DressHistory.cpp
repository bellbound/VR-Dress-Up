#include "DressHistory.h"

#include <algorithm>
#include "../InventoryManager.h"
#include "../log.h"
#include "FormKeyUtil.h"
#include "ItemEquipHelper.h"
#include "OutfitFormBackend.h"
#include "OutfitLockManager.h"
#include "WeaponLockManager.h"

namespace
{
    template <class T>
    bool Contains(const std::vector<T>& sorted, const T& value)
    {
        return std::binary_search(sorted.begin(), sorted.end(), value);
    }

    template <class T>
    void Sort(std::vector<T>& v)
    {
        std::sort(v.begin(), v.end());
    }

    std::string KeyOf(RE::FormID id)
    {
        auto* form = RE::TESForm::LookupByID(id);
        return form ? Persistence::FormKeyUtil::BuildFormKey(form) : std::string{};
    }

    bool IsRealWeapon(RE::TESObjectWEAP* weapon)
    {
        return weapon && weapon->GetWeaponType() != RE::WEAPON_TYPE::kHandToHandMelee;
    }

    const char* KindName(DressActionKind kind)
    {
        switch (kind) {
            case DressActionKind::WheelToggle: return "toggle";
            case DressActionKind::Outfit:      return "outfit";
            case DressActionKind::Default:     return "default";
            case DressActionKind::Undress:     return "undress";
            case DressActionKind::Redress:     return "redress";
        }
        return "?";
    }
}

DressSnapshot DressHistory::Capture(RE::Actor* actor)
{
    DressSnapshot snap;
    if (!actor) return snap;

    auto* lockMgr = OutfitLockManager::GetSingleton();
    const bool isPlayer = actor->IsPlayerRef();

    for (const RE::FormID id : lockMgr->SnapshotWornArmor(actor)) {
        std::string key = KeyOf(id);
        if (key.empty()) continue;
        if (lockMgr->IsGallerySpawnedItem(actor, id)) {
            snap.galleryMarked.push_back(key);
        }
        snap.wornKeys.push_back(std::move(key));
    }
    Sort(snap.wornKeys);
    Sort(snap.galleryMarked);

    snap.locked = lockMgr->IsLocked(actor);
    if (snap.locked) {
        snap.lockedKeys = lockMgr->GetOutfitItemFormKeys(actor, "locked");
        Sort(snap.lockedKeys);
        snap.lockedNoWeapon = lockMgr->GetOutfitNoWeapon(actor, "locked");
    }

    // Hand-overs and hands are the NPC's side of the menu. The player's own wheel only
    // ever reaches the player through the undress button, and reading the hand-over list
    // into a player snapshot would make undoing a player undress take pieces back off
    // the NPC.
    if (!isPlayer) {
        auto* invMgr = InventoryManager::GetSingleton();
        if (invMgr->GetTargetActor() == actor) {
            snap.transferred = invMgr->TransferredItems();
            Sort(snap.transferred);
        }

        for (auto* weapon : ItemEquipHelper::GetEquippedWeapons(actor)) {
            if (IsRealWeapon(weapon)) {
                snap.hands.push_back(weapon->GetFormID());
            }
        }
        Sort(snap.hands);
        snap.weaponLock = WeaponLockManager::GetSingleton()->IsEnforcing(actor);
    }

    auto* undressMgr = UndressManager::GetSingleton();
    snap.undressState = undressMgr->GetUndressState(actor);
    if (snap.undressState != UndressState::Dressed) {
        snap.preundressKeys = lockMgr->GetOutfitItemFormKeys(actor, UndressManager::kPreUndressOutfitName);
        Sort(snap.preundressKeys);
    }

    return snap;
}

bool DressHistory::Restore(RE::Actor* actor, const DressSnapshot& snap)
{
    if (!actor) return false;

    auto* lockMgr = OutfitLockManager::GetSingleton();
    auto* invMgr = InventoryManager::GetSingleton();
    auto* player = RE::PlayerCharacter::GetSingleton();
    const bool isPlayer = actor->IsPlayerRef();
    const bool isMenuTarget = !isPlayer && invMgr->GetTargetActor() == actor;
    const bool lockedNow = lockMgr->IsLocked(actor);

    // Locking or unlocking hands the actor to or from the outfit backend, and the backend
    // sets outfits through Papyrus: the previous SetOutfit is still in flight for a second
    // or so after a Default or an outfit pick. Flipping the lock inside that window makes
    // the backend read its own pool record back as the actor's original outfit, and the
    // real one is gone for good. Refuse rather than race it; the player presses again.
    if (!isPlayer && snap.locked != lockedNow && OutfitFormBackend::GetSingleton()->IsApplying()) {
        spdlog::info("DressHistory::Restore - '{}': outfit backend still applying; try again in a moment",
            actor->GetName());
        return false;
    }

    spdlog::info("DressHistory::Restore - '{}': {} item(s), locked={}, undress={}, {} hand-over(s)",
        actor->GetName(), snap.wornKeys.size(), snap.locked,
        static_cast<int>(snap.undressState), snap.transferred.size());

    // Read before anything moves: the gallery cleanup below needs what was on.
    const std::vector<RE::FormID> wornBefore = lockMgr->SnapshotWornArmor(actor);

    // Ours from here to the end, so the watcher does not read the restore as somebody
    // else undressing the actor.
    lockMgr->BeginUserEdit();

    UndressManager::GetSingleton()->ClearUndressState(actor);

    // Hand them back to the game first, where the snapshot has them unlocked. The worn set
    // still goes on below: the backend's restore is asynchronous, and a no-op for actors
    // it never took, so "unlocked" on its own would leave them in whatever they had on.
    if (!isPlayer && !snap.locked && lockedNow) {
        lockMgr->Unlock(actor);
    }

    // Everything the snapshot wears has to be in the inventory before it is applied:
    // ApplyOutfit prunes what the actor lacks, and the pruning rewrites the stored outfit.
    // A piece that came from the player's pack goes back across from there, so a redo of a
    // hand-over moves the one copy rather than conjuring a second; anything else is
    // re-created, and re-armed as a gallery try-on if that is what it was.
    auto ensure = [&](RE::TESBoundObject* item, const std::string& key) {
        if (!item || ItemEquipHelper::HasItemInInventory(actor, item)) return;

        if (isMenuTarget && Contains(snap.transferred, item->GetFormID()) &&
            player && ItemEquipHelper::HasItemInInventory(player, item)) {
            invMgr->GiveToTarget(item);
            return;
        }

        actor->AddObjectToContainer(item, nullptr, 1, nullptr);
        if (!key.empty() && Contains(snap.galleryMarked, key)) {
            lockMgr->MarkItemAsGallerySpawned(actor, item->GetFormID());
        }
        spdlog::debug("DressHistory::Restore - re-created '{}' for '{}'", item->GetName(), actor->GetName());
    };

    for (const auto& key : snap.wornKeys) {
        auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(
            Persistence::FormKeyUtil::ResolveToRuntimeFormID(key));
        if (armor && !ItemEquipHelper::IsBodyPart(armor)) {
            ensure(armor, key);
        }
    }
    for (const RE::FormID id : snap.hands) {
        ensure(RE::TESForm::LookupByID<RE::TESObjectWEAP>(id), {});
    }

    // The armour, through the same path a saved outfit takes. A locked snapshot is adopted
    // as the lock - copied over "locked", applied with everything else taken off, defended
    // from then on; an unlocked one, or the player, is simply applied.
    lockMgr->SetOutfitFromFormKeys(actor, kScratchOutfit, snap.wornKeys);
    if (!isPlayer && snap.locked) {
        lockMgr->AdoptStoredOutfit(actor, kScratchOutfit);
        // The lock and the body can differ by a piece the lock could not take off - a
        // device DD refused to release - so the lock is put back as it was, not as worn.
        if (snap.lockedKeys != snap.wornKeys) {
            lockMgr->SetOutfitFromFormKeys(actor, "locked", snap.lockedKeys);
            lockMgr->PromoteLockToOutfitForm(actor);
        }
        lockMgr->SetOutfitNoWeapon(actor, "locked", snap.lockedNoWeapon);
    } else {
        lockMgr->ApplyOutfit(actor, kScratchOutfit, true);
    }
    lockMgr->DeleteOutfit(actor, kScratchOutfit);

    // Gallery try-ons the apply took off. ApplyOutfit unequips as self-driven, which the
    // equip sink ignores before it gets to its gallery cleanup, so the piece would stay in
    // the inventory, still marked, and become real loot when the wheel closes. Same
    // removal the sink does, a frame later and with the same "went back on? keep it"
    // check, since a slot cleared and refilled with the same piece never really emptied.
    if (!isPlayer) {
        for (const RE::FormID id : wornBefore) {
            if (!lockMgr->IsGallerySpawnedItem(actor, id)) continue;
            if (Contains(snap.wornKeys, KeyOf(id))) continue;

            lockMgr->UnmarkGalleryItem(actor, id);
            const RE::FormID actorID = actor->GetFormID();
            SKSE::GetTaskInterface()->AddTask([actorID, id]() {
                auto* target = RE::TESForm::LookupByID<RE::Actor>(actorID);
                auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
                if (!target || !armor) return;

                if (ItemEquipHelper::IsArmorEquippedOrPending(target, armor)) {
                    OutfitLockManager::GetSingleton()->MarkItemAsGallerySpawned(target, id);
                    return;
                }
                target->RemoveItem(armor, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                spdlog::info("DressHistory::Restore - removed gallery try-on '{}' from '{}'",
                    armor->GetFullName(), target->GetName());
            });
        }
    }

    if (!isPlayer) {
        // Hands, then the no-weapon lock - which has to be right before the menu repaints,
        // because which saved outfit counts as "on" is judged by it as well as by the keys.
        for (auto* weapon : ItemEquipHelper::GetEquippedWeapons(actor)) {
            if (IsRealWeapon(weapon) && !Contains(snap.hands, weapon->GetFormID())) {
                ItemEquipHelper::UnequipItem(actor, weapon);
            }
        }
        for (const RE::FormID id : snap.hands) {
            auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(id);
            if (weapon && !ItemEquipHelper::IsWeaponEquipped(actor, weapon)) {
                ItemEquipHelper::EquipItem(actor, weapon);
            }
        }
        auto* weaponLock = WeaponLockManager::GetSingleton();
        if (snap.weaponLock) {
            weaponLock->Enforce(actor);
        } else {
            weaponLock->StopEnforcing(actor);
        }

        // Pieces the player handed over after this snapshot was taken go back to their
        // pack, the way clicking their ghost in the player's wheel would send them.
        if (isMenuTarget) {
            for (const RE::FormID id : invMgr->TransferredItems()) {
                if (Contains(snap.transferred, id)) continue;
                if (auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(id)) {
                    invMgr->TakeBackFromTarget(item);
                }
            }
        }
    }

    UndressManager::GetSingleton()->RestoreState(actor, snap.undressState, snap.preundressKeys);

    lockMgr->EndUserEdit();
    return true;
}

void DressHistory::Record(DressActionKind kind, RE::Actor* actor, RE::FormID formID, DressSnapshot before)
{
    if (!actor) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_redo.clear();

    if (kind == DressActionKind::WheelToggle && !m_undo.empty()) {
        const auto& top = m_undo.back();
        if (top.kind == DressActionKind::WheelToggle &&
            top.actorID == actor->GetFormID() && top.formID == formID) {
            spdlog::debug("DressHistory::Record - toggle of 0x{:08X} folded into the previous entry", formID);
            return;
        }
    }

    m_undo.push_back({kind, actor->GetFormID(), formID, std::move(before), std::nullopt});
    spdlog::debug("DressHistory::Record - {} on '{}' ({} to undo)", KindName(kind), actor->GetName(), m_undo.size());
}

bool DressHistory::CanUndo() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_undo.empty();
}

bool DressHistory::CanRedo() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_redo.empty();
}

std::optional<DressSnapshot> DressHistory::Undo(std::optional<std::uint32_t> editingSlot)
{
    while (true) {
        DressHistoryEntry entry;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_undo.empty()) return std::nullopt;
            entry = m_undo.back();
        }

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(entry.actorID);
        if (!actor) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_undo.pop_back();
            continue;
        }

        DressSnapshot now = Capture(actor);
        now.editingSlot = editingSlot;

        // A press that left things as they were - a piece clicked on and off again, a
        // click the mod owning the piece refused - is not a step worth taking back.
        if (now == entry.before) {
            spdlog::debug("DressHistory::Undo - {} on '{}' changed nothing; skipping",
                KindName(entry.kind), actor->GetName());
            std::lock_guard<std::mutex> lock(m_mutex);
            m_undo.pop_back();
            continue;
        }

        spdlog::info("DressHistory::Undo - {} on '{}'", KindName(entry.kind), actor->GetName());
        if (!Restore(actor, entry.before)) {
            return std::nullopt;   // refused; the entry stays for another press
        }

        entry.after = std::move(now);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_undo.pop_back();
        m_redo.push_back(std::move(entry));
        return m_redo.back().before;
    }
}

std::optional<DressSnapshot> DressHistory::Redo()
{
    DressHistoryEntry entry;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_redo.empty()) return std::nullopt;
        entry = m_redo.back();
    }

    auto* actor = RE::TESForm::LookupByID<RE::Actor>(entry.actorID);
    if (!actor || !entry.after) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_redo.pop_back();
        return std::nullopt;
    }

    spdlog::info("DressHistory::Redo - {} on '{}'", KindName(entry.kind), actor->GetName());
    if (!Restore(actor, *entry.after)) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_redo.pop_back();
    m_undo.push_back(std::move(entry));
    return *m_undo.back().after;
}

void DressHistory::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_undo.empty() && m_redo.empty()) return;
    m_undo.clear();
    m_redo.clear();
    spdlog::debug("DressHistory: Cleared");
}

void DressHistory::OnPreLoad()
{
    GetSingleton()->Clear();
}

void DressHistory::OnRevert(SKSE::SerializationInterface*)
{
    GetSingleton()->Clear();
}

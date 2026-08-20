#pragma once

#include <RE/Skyrim.h>
#include <algorithm>
#include <vector>
#include <string>
#include <functional>
#include "log.h"
#include "dressup/OutfitLockManager.h"
#include "dressup/OutfitFormBackend.h"
#include "dressup/DressupTransaction.h"
#include "dressup/ItemEquipHelper.h"
#include "dressup/DeviceCompat.h"
#include "dressup/UndressManager.h"
#include "dressup/WeaponLockManager.h"

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

    // Told whenever a gear change has actually landed on an actor the wheel is painting
    // plates for. Set by the menu, which is above this class and cannot be called directly.
    using WornStateChangedCallback = std::function<void()>;

    void SetWornStateChangedCallback(WornStateChangedCallback callback)
    {
        m_onWornStateChanged = std::move(callback);
    }

    // Process equip events - return transferred items to player when NPC unequips them
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESEquipEvent* a_event,
        RE::BSTEventSource<RE::TESEquipEvent>*) override
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // The wheel paints its plates from what these actors have on, so every gear change
        // that reaches one of them moves a plate - including the ones nobody clicked for.
        // Equipping into an occupied biped slot makes the engine take the occupant off,
        // and that removal is announced here and nowhere else: the click that caused it
        // repainted the wheel a frame or two earlier, back when the displaced piece was
        // still on, and left its plate reading "worn" until the next click happened to
        // repaint it.
        //
        // Said ahead of every filter below because those exist for the ghost-item
        // bookkeeping. Equips are none of its business but they are the wheel's, and an
        // outfit reapply's unequips are real changes to the plates even when they are ours.
        if (const auto* changed = a_event->actor.get(); IsShownActor(changed)) {
            if (m_onWornStateChanged) {
                m_onWornStateChanged();
            }
        }

        if (m_processingReturn || a_event->equipped) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // An outfit assignment makes the engine queue an UnequipAll. Those unequips are
        // ours, not the NPC shedding gear, and acting on them would hand the player's
        // items back mid-dress.
        if (OutfitFormBackend::GetSingleton()->IsApplying()) {
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
        spdlog::debug("InventoryManager::SetTargetActor - Set target to: {}",
            actor ? actor->GetName() : "null");

        // Before the spiral is built, not after: if something else has dressed this NPC
        // since we last looked - a scene, a bath mod, the engine re-equipping their own
        // gear while the equip-storm breaker was standing down - then the wheel would
        // list that gear as theirs and every edit would be made on top of it. Puts the
        // locked outfit back first, so the player edits what they locked. No-op for an
        // unlocked actor, or one already wearing the locked set.
        OutfitLockManager::GetSingleton()->ReconcileBeforeUserEdit(actor);

        m_transaction.CapturePlayerEquipment();
    }

    RE::Actor* GetTargetActor() const { return m_targetActor; }

    // Whose inventory the wheel is currently showing: the player in player mode, the NPC
    // being dressed otherwise. Public because the wheel has to ask what this actor is
    // wearing in order to mark those items.
    RE::Actor* GetSourceActor() const
    {
        return m_targetIsPlayer ? RE::PlayerCharacter::GetSingleton() : m_targetActor;
    }

    // Whether this reference is one the open wheel is showing state for: the actor whose
    // inventory the spiral lists, and the actor a gallery pick lands on. The same actor
    // most of the time, and different exactly in player mode.
    bool IsShownActor(const RE::TESObjectREFR* ref) const
    {
        if (!ref || !m_targetActor) return false;

        const RE::FormID id = ref->GetFormID();
        if (id == m_targetActor->GetFormID()) return true;

        const auto* source = GetSourceActor();
        return source && id == source->GetFormID();
    }

    void SetTargetIsPlayer(bool isPlayer)
    {
        // When switching back to NPC mode, clear ghost items (transfers are "confirmed")
        if (!isPlayer && m_targetIsPlayer) {
            m_transaction.ClearTransferTracking();
        }
        m_targetIsPlayer = isPlayer;
        spdlog::debug("InventoryManager::SetTargetIsPlayer - Target is player: {}", isPlayer);
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

            // The rendered half of a Devious Device is a nameless armour record that DD
            // puts on and takes off itself. It would show up as a blank entry that does
            // nothing useful when picked - the inventory half next to it is the device.
            if (DeviceCompat::IsRenderedDevice(armor)) {
                continue;
            }

            // Filtered against the actor being dressed, not against whoever's inventory this
            // is: in player mode the items come out of the player's pack and go onto the NPC,
            // and it is the NPC who has no mesh for female-only gear.
            if (!ItemEquipHelper::FitsActor(armor, m_targetActor)) {
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
                if (!ItemEquipHelper::FitsActor(armor, m_targetActor)) {
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

        // Suspend lock enforcement while we make changes
        ScopedLockSuspension suspension(m_targetActor);

        if (m_targetIsPlayer) {
            // Check if this is a transferred item (ghost entry) - if so, reverse the transfer
            if (m_transaction.IsTransferredItem(armor->GetFormID())) {
                ReverseTransfer(armor);
            } else {
                EquipFromPlayer(armor);
            }
        } else {
            ItemEquipHelper::ToggleArmorEquip(m_targetActor, armor);
        }

        // Clear undress state when gear is manually changed
        bool undressStateCleared = ClearUndressStateOnGearChange();

        // Lock will be re-applied when suspension goes out of scope if was already locked,
        // otherwise explicitly lock since this is a new change
        if (!suspension.WasLocked()) {
            AutoLockNpc();
        }
        return undressStateCleared;
    }

    // Returns true if undress state was cleared (for UI refresh purposes)
    bool OnItemSelected(RE::TESObjectWEAP* weapon)
    {
        if (!weapon || !m_targetActor) return false;

        // Suspend lock enforcement while we make changes
        ScopedLockSuspension suspension(m_targetActor);

        // Whether the click leaves the NPC holding this weapon. Recorded as intent rather
        // than read back off the actor afterwards, because an equip is queued and does not
        // show up in their worn items for another frame or two.
        bool nowHolding = false;

        if (m_targetIsPlayer) {
            // Check if this is a transferred item (ghost entry) - if so, reverse the transfer
            if (m_transaction.IsTransferredItem(weapon->GetFormID())) {
                ReverseTransfer(weapon);
            } else {
                nowHolding = EquipFromPlayer(weapon);
            }
        } else {
            nowHolding = ItemEquipHelper::ToggleWeaponEquip(m_targetActor, weapon);
        }

        // Taking an NPC's last weapon off is the whole way the no-weapon lock is asked for,
        // and handing one back is the whole way it is called off.
        WeaponLockManager::GetSingleton()->NoteMenuWeaponChange(m_targetActor, nowHolding);

        // Clear undress state when gear is manually changed
        bool undressStateCleared = ClearUndressStateOnGearChange();

        // Lock will be re-applied when suspension goes out of scope if was already locked,
        // otherwise explicitly lock since this is a new change
        if (!suspension.WasLocked()) {
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
        // Relock rather than Lock: this is the lock button, the one place a lock can be
        // undoing the unlock that came before it.
        OutfitLockManager::GetSingleton()->Relock(m_targetActor);
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

        // Suspend lock enforcement while we make changes
        ScopedLockSuspension suspension(m_targetActor);

        // Click a piece the actor already has on to take it off again.
        //
        // Without this branch the gallery had no way to remove anything: GetWornInSlot
        // below hands back the very item that was clicked, ClearSlotFor takes it off, and
        // the equip at the end puts it straight back - so the piece never came off, while
        // the unequip in the middle had already told OutfitLockManager to destroy it as a
        // gallery item. The player got a piece that would not come off and then vanished
        // out of the NPC's inventory a frame later. Both galleries - mod categories and
        // keyword categories - come through here, so both get the toggle.
        if (ItemEquipHelper::IsArmorEquippedOrPending(m_targetActor, armor)) {
            if (!ItemEquipHelper::UnequipArmor(m_targetActor, armor)) {
                spdlog::info("InventoryManager::EquipFromMod - '{}' stays on {}: the mod that "
                    "owns it will not release it", armor->GetFullName(), m_targetActor->GetName());
                return;
            }

            ClearUndressStateOnGearChange();

            if (!suspension.WasLocked()) {
                AutoLockNpc();
            }

            spdlog::info("InventoryManager::EquipFromMod - Unequipped '{}' from {}",
                armor->GetFullName(), m_targetActor->GetName());
            return;
        }

        // Check if NPC already has this armor in inventory
        bool hasInInventory = ItemEquipHelper::HasItemInInventory(m_targetActor, armor);

        // Unequip what NPC is wearing in this slot
        auto* wornArmor = ItemEquipHelper::GetWornInSlot(m_targetActor, armor);
        if (wornArmor) {
            ClearSlotFor(wornArmor);
        }

        if (!hasInInventory) {
            // Add armor to NPC inventory (creates a new instance)
            m_targetActor->AddObjectToContainer(armor, nullptr, 1, nullptr);

            // Mark as gallery-spawned so it gets destroyed when unequipped
            OutfitLockManager::GetSingleton()->MarkItemAsGallerySpawned(m_targetActor, armor->GetFormID());

            spdlog::info("InventoryManager::EquipFromMod - Added '{}' to {}'s inventory (gallery-spawned)",
                armor->GetFullName(), m_targetActor->GetName());
        }

        // Equip the armor
        ItemEquipHelper::EquipArmor(m_targetActor, armor);

        // Clear undress state when gear is manually changed
        ClearUndressStateOnGearChange();

        // Lock will be re-applied when suspension goes out of scope if was already locked,
        // otherwise explicitly lock since this is a new change
        if (!suspension.WasLocked()) {
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

    // Take off whatever is occupying the slot we are about to fill. Returns false when the
    // slot could not be cleared, so the caller does not record an unequip that never
    // happened.
    //
    // The rendered half of a Devious Device owns a real biped slot, so it is what
    // GetWornInSlot hands back when a device is on. Pulling it off directly desyncs DD,
    // which puts it straight back - the new piece and the device then trade the slot for
    // as long as the NPC is loaded. A locked-on device wins the slot instead, and the
    // player gets told rather than left wondering.
    bool ClearSlotFor(RE::TESObjectARMO* wornArmor)
    {
        if (!wornArmor || !m_targetActor) return false;

        if (DeviceCompat::IsRenderedDevice(wornArmor)) {
            spdlog::info("InventoryManager::ClearSlotFor - Slot is held by a Devious Device on "
                "'{}'; leaving it on", m_targetActor->GetName());
            return false;
        }

        if (DeviceCompat::IsInventoryDevice(wornArmor)) {
            if (!DeviceCompat::Unequip(m_targetActor, wornArmor)) return false;
            // DD clears the worn flag from Papyrus, well after this returns, so the
            // re-save at the end of this edit would otherwise put the device back into
            // the locked outfit and the reapply would lock it on again.
            ItemEquipHelper::NotePendingUnequip(m_targetActor, wornArmor);
            return true;
        }

        ItemEquipHelper::UnequipItem(m_targetActor, wornArmor);
        return true;
    }

    // Equip item from player's inventory onto NPC (armor version)
    void EquipFromPlayer(RE::TESObjectARMO* armor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !ItemEquipHelper::HasItemInInventory(player, armor)) {
            spdlog::warn("InventoryManager::EquipFromPlayer - Player doesn't have '{}'", armor->GetFullName());
            return;
        }

        // Unequip what NPC is wearing in this slot
        auto* wornArmor = ItemEquipHelper::GetWornInSlot(m_targetActor, armor);
        if (wornArmor && ClearSlotFor(wornArmor)) {
            if (!m_transaction.IsTransferredItem(wornArmor->GetFormID())) {
                m_transaction.TrackUnequip(wornArmor);
            }
        }

        // Transfer and equip
        ItemEquipHelper::TransferItem(player, m_targetActor, armor);
        ItemEquipHelper::EquipArmor(m_targetActor, armor);
        m_transaction.TrackTransfer(armor, true);

        // Mark as player-given for persistent tracking
        OutfitLockManager::GetSingleton()->MarkItemAsPlayerGiven(m_targetActor, armor->GetFormID());

        spdlog::info("InventoryManager::EquipFromPlayer - Transferred and equipped '{}' on {}",
            armor->GetFullName(), m_targetActor->GetName());
    }

    // Equip item from player's inventory onto NPC (weapon version)
    // Returns whether the NPC ended up holding the weapon, so the caller can tell a real
    // handover from a click that could not be honoured.
    bool EquipFromPlayer(RE::TESObjectWEAP* weapon)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !ItemEquipHelper::HasItemInInventory(player, weapon)) {
            spdlog::warn("InventoryManager::EquipFromPlayer - Player doesn't have '{}'", weapon->GetFullName());
            return false;
        }

        // Unequip NPC's current weapons in both hands
        auto equippedWeapons = ItemEquipHelper::GetEquippedWeapons(m_targetActor);
        for (auto* currentWeapon : equippedWeapons) {
            if (!m_transaction.IsTransferredItem(currentWeapon->GetFormID())) {
                m_transaction.TrackUnequip(currentWeapon);
            }
            ItemEquipHelper::UnequipItem(m_targetActor, currentWeapon);
        }

        // Transfer and equip
        ItemEquipHelper::TransferItem(player, m_targetActor, weapon);
        ItemEquipHelper::EquipItem(m_targetActor, weapon);
        m_transaction.TrackTransfer(weapon, true);

        // Mark as player-given for persistent tracking
        OutfitLockManager::GetSingleton()->MarkItemAsPlayerGiven(m_targetActor, weapon->GetFormID());

        spdlog::info("InventoryManager::EquipFromPlayer - Transferred and equipped '{}' on {}",
            weapon->GetFullName(), m_targetActor->GetName());
        return true;
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

        spdlog::debug("InventoryManager::ReturnItemToPlayer - Returned item 0x{:08X} to player", formID);
    }

    // Reverse a transfer - unequip from NPC and return to player (called when clicking ghost item)
    void ReverseTransfer(RE::TESBoundObject* item)
    {
        if (!item || !m_targetActor) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Unequip from NPC. A device has to be unlocked by DD first, and DD may refuse -
        // in which case it stays where it is rather than going back half-removed.
        if (auto* armor = item->As<RE::TESObjectARMO>(); armor && DeviceCompat::IsDevice(armor)) {
            if (!ItemEquipHelper::UnequipArmor(m_targetActor, armor)) {
                spdlog::info("InventoryManager::ReverseTransfer - '{}' stays on '{}': Devious "
                    "Devices will not release it", item->GetName(), m_targetActor->GetName());
                return;
            }
        } else {
            ItemEquipHelper::UnequipItem(m_targetActor, item);
        }

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

    // Set once, by the menu, and read from the equip event on the game thread.
    WornStateChangedCallback m_onWornStateChanged;
};

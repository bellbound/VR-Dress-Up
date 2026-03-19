#include "OutfitLockManager.h"
#include <spdlog/spdlog.h>

// Biped slots for body armor (30-45)
// These map to RE::BIPED_MODEL::BipedObjectSlot enum values
static constexpr std::uint32_t kArmorSlots[] = {
    30, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45
};

void OutfitLockManager::Initialize()
{
    if (m_initialized) {
        spdlog::warn("OutfitLockManager::Initialize - Already initialized");
        return;
    }

    auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
    if (sourceHolder) {
        sourceHolder->AddEventSink<RE::TESEquipEvent>(this);
        spdlog::info("OutfitLockManager::Initialize - Registered for TESEquipEvent");
        m_initialized = true;
    } else {
        spdlog::error("OutfitLockManager::Initialize - Failed to get ScriptEventSourceHolder");
    }

    // Register for location change events (fires when player changes location)
    if (!m_cellEventRegistered && sourceHolder) {
        sourceHolder->AddEventSink<RE::TESActorLocationChangeEvent>(this);
        m_cellEventRegistered = true;
        spdlog::info("OutfitLockManager::Initialize - Registered for TESActorLocationChangeEvent");
    }
}

RE::BSEventNotifyControl OutfitLockManager::ProcessEvent(
    const RE::TESEquipEvent* a_event,
    RE::BSTEventSource<RE::TESEquipEvent>*)
{
    if (!a_event) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Get actor reference
    auto* actor = a_event->actor ? a_event->actor->As<RE::Actor>() : nullptr;

    // Get actor name if available
    const char* actorName = actor ? actor->GetName() : "unknown";

    // Get base object name if available
    std::string baseObjectName = "unknown";
    if (auto* form = RE::TESForm::LookupByID(a_event->baseObject)) {
        if (auto* name = form->GetName(); name && name[0]) {
            baseObjectName = name;
        } else {
            baseObjectName = fmt::format("FormID:{:08X}", a_event->baseObject);
        }
    }

    spdlog::trace("OutfitLockManager::ProcessEvent - Actor: {}, BaseObject: {}, Equipped: {}, UniqueID: {}",
        actorName,
        baseObjectName,
        a_event->equipped ? "true" : "false",
        a_event->uniqueID);

    // Handle gallery item destruction on unequip
    if (!a_event->equipped && actor && !actor->IsPlayerRef()) {
        // Check if this item was gallery-spawned
        if (IsGallerySpawnedItem(actor, a_event->baseObject)) {
            // Capture IDs for deferred task (pointers may become invalid)
            RE::FormID actorID = actor->GetFormID();
            RE::FormID itemID = a_event->baseObject;
            std::string itemName = baseObjectName;

            // Unmark immediately to prevent double-processing if re-equipped quickly
            UnmarkGalleryItem(actor, itemID);

            // Defer the actual removal to the next frame to avoid race condition
            // The unequip operation may still be modifying inventory state when this event fires
            SKSE::GetTaskInterface()->AddTask([actorID, itemID, itemName]() {
                auto* targetActor = RE::TESForm::LookupByID<RE::Actor>(actorID);
                auto* form = RE::TESForm::LookupByID(itemID);

                if (targetActor && form) {
                    auto* boundObj = form->As<RE::TESBoundObject>();
                    if (boundObj) {
                        targetActor->RemoveItem(boundObj, 1,
                            RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                        spdlog::info("OutfitLockManager - Destroyed gallery-spawned item '{}' (0x{:08X}) from '{}'",
                            itemName, itemID, targetActor->GetName());
                    }
                }
            });

            spdlog::info("OutfitLockManager::ProcessEvent - Queued destruction of gallery-spawned item '{}' (0x{:08X}) from '{}'",
                baseObjectName, a_event->baseObject, actorName);
        }
    }

    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl OutfitLockManager::ProcessEvent(
    const RE::TESActorLocationChangeEvent* a_event,
    RE::BSTEventSource<RE::TESActorLocationChangeEvent>*)
{
    if (!a_event || !a_event->actor) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* actor = a_event->actor->As<RE::Actor>();
    if (!actor) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Only process when the PLAYER changes location
    // When player enters a new location, apply locked outfits to NPCs in the area
    if (!actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    std::string newLocName = "Unknown";
    if (a_event->newLoc && a_event->newLoc->fullName.c_str()) {
        newLocName = a_event->newLoc->fullName.c_str();
    }

    spdlog::info("OutfitLockManager::ProcessEvent(LocationChange) - Player entered location '{}', applying locked outfits",
        newLocName);

    // Apply locked outfits to NPCs in the new location
    ApplyLockedOutfitsInLocation(a_event->newLoc);

    return RE::BSEventNotifyControl::kContinue;
}

std::vector<RE::TESObjectARMO*> OutfitLockManager::GetEquippedArmor(RE::Actor* actor) const
{
    std::vector<RE::TESObjectARMO*> result;
    if (!actor) return result;

    // Check each armor slot for equipped items
    for (std::uint32_t slot : kArmorSlots) {
        // Convert slot number to biped mask
        // Slot 30 = Head, 31 = Hair, 32 = Body, etc.
        // The slot mask is 1 << (slot - 30)
        auto slotMask = static_cast<RE::BIPED_MODEL::BipedObjectSlot>(1 << (slot - 30));
        auto* armor = actor->GetWornArmor(slotMask);
        if (armor) {
            // Avoid duplicates (an armor piece can cover multiple slots)
            bool alreadyAdded = false;
            for (auto* existing : result) {
                if (existing->GetFormID() == armor->GetFormID()) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded) {
                result.push_back(armor);
            }
        }
    }

    return result;
}

void OutfitLockManager::UnequipArmorSlots(RE::Actor* actor)
{
    if (!actor) return;

    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) {
        spdlog::error("OutfitLockManager::UnequipArmorSlots - No ActorEquipManager");
        return;
    }

    // Get all equipped armor first
    auto equipped = GetEquippedArmor(actor);

    // Unequip each piece
    for (auto* armor : equipped) {
        equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, true);
        spdlog::trace("OutfitLockManager::UnequipArmorSlots - Unequipped '{}'", armor->GetFullName());
    }
}

void OutfitLockManager::EquipArmorList(RE::Actor* actor, const std::vector<RE::TESObjectARMO*>& items)
{
    if (!actor) return;

    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) {
        spdlog::error("OutfitLockManager::EquipArmorList - No ActorEquipManager");
        return;
    }

    for (auto* armor : items) {
        if (!armor) continue;

        // Check if actor has this item in inventory
        auto inventory = actor->GetInventory([armor](RE::TESBoundObject& obj) {
            return obj.GetFormID() == armor->GetFormID();
        });

        if (!inventory.empty()) {
            equipManager->EquipObject(actor, armor, nullptr, 1, nullptr, true, false, false);
            spdlog::trace("OutfitLockManager::EquipArmorList - Equipped '{}'", armor->GetFullName());
        } else {
            spdlog::warn("OutfitLockManager::EquipArmorList - Actor doesn't have '{}' in inventory",
                armor->GetFullName());
        }
    }
}

std::string OutfitLockManager::GetModName(RE::TESForm* form) const
{
    if (!form) return "";

    std::uint8_t fullIndex = form->GetFormID() >> 24;
    const RE::TESFile* file = nullptr;

    if (fullIndex == 0xFE) {
        // Light mod
        std::uint16_t lightIndex = (form->GetFormID() & 0x00FFFFFF) >> 12;
        file = RE::TESDataHandler::GetSingleton()->LookupLoadedLightModByIndex(lightIndex);
    } else {
        file = RE::TESDataHandler::GetSingleton()->LookupLoadedModByIndex(fullIndex);
    }

    return file ? std::string(file->GetFilename()) : "";
}

bool OutfitLockManager::SaveOutfit(RE::Actor* actor, const std::string& outfitName)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::SaveOutfit - No actor provided");
        return false;
    }

    spdlog::info("OutfitLockManager::SaveOutfit - Saving outfit '{}' for actor '{}' (0x{:08X})",
        outfitName, actor->GetName(), actor->GetFormID());

    auto equipped = GetEquippedArmor(actor);
    if (equipped.empty()) {
        spdlog::warn("OutfitLockManager::SaveOutfit - Actor has no equipped armor");
    }

    SavedOutfit outfit;
    for (auto* armor : equipped) {
        std::string formKey = Persistence::FormKeyUtil::BuildFormKey(armor);
        if (formKey.empty()) {
            spdlog::warn("  - Skipping armor '{}' - no source file (dynamic item?)",
                armor->GetFullName());
            continue;
        }

        SavedArmorItem item;
        item.formKey = formKey;
        outfit.items.push_back(item);

        spdlog::info("  - Saved armor '{}' as '{}'",
            armor->GetFullName(), formKey);
    }

    // Also capture equipped weapons
    RE::TESForm* rightHand = actor->GetEquippedObject(false);
    RE::TESForm* leftHand = actor->GetEquippedObject(true);

    auto addWeapon = [&outfit](RE::TESForm* form) {
        if (!form) return;
        auto* weapon = form->As<RE::TESObjectWEAP>();
        if (!weapon) return;

        std::string wFormKey = Persistence::FormKeyUtil::BuildFormKey(weapon);
        if (wFormKey.empty()) {
            spdlog::warn("  - Skipping weapon '{}' - no source file (dynamic item?)", weapon->GetFullName());
            return;
        }

        // Avoid duplicates (dual-wielding same weapon)
        for (const auto& existing : outfit.weapons) {
            if (existing.formKey == wFormKey) return;
        }

        SavedArmorItem item;
        item.formKey = wFormKey;
        outfit.weapons.push_back(item);
        spdlog::info("  - Saved weapon '{}' as '{}'", weapon->GetFullName(), wFormKey);
    };

    addWeapon(rightHand);
    addWeapon(leftHand);

    size_t weaponCount = outfit.weapons.size();

    // Preserve weaponUnlocked from existing locked outfit if re-saving
    OutfitKey key{actor->GetFormID(), outfitName};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (outfitName == "locked") {
            auto existingIt = m_outfits.find(key);
            if (existingIt != m_outfits.end()) {
                outfit.weaponUnlocked = existingIt->second.weaponUnlocked;
            }
        }
        m_outfits[key] = std::move(outfit);
    }

    spdlog::info("OutfitLockManager::SaveOutfit - Saved {} armor, {} weapons for outfit '{}'",
        equipped.size(), weaponCount, outfitName);

    return true;
}

bool OutfitLockManager::ApplyOutfit(RE::Actor* actor, const std::string& outfitName, bool unequipOthers)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::ApplyOutfit - No actor provided");
        return false;
    }

    OutfitKey key{actor->GetFormID(), outfitName};
    SavedOutfit outfit;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_outfits.find(key);
        if (it == m_outfits.end()) {
            spdlog::warn("OutfitLockManager::ApplyOutfit - No outfit '{}' found for actor '{}'",
                outfitName, actor->GetName());
            return false;
        }
        outfit = it->second;  // Copy so we can work outside the lock
    }

    spdlog::info("OutfitLockManager::ApplyOutfit - Applying outfit '{}' to actor '{}' (unequipOthers={})",
        outfitName, actor->GetName(), unequipOthers);

    // Unequip existing armor if requested
    if (unequipOthers) {
        UnequipArmorSlots(actor);
    }

    // Build list of valid armor to equip, removing invalid items or items NPC doesn't have
    std::vector<RE::TESObjectARMO*> validArmor;
    std::vector<std::string> keysToRemove;

    for (const auto& item : outfit.items) {
        if (!item.IsValid()) {
            keysToRemove.push_back(item.formKey);
            spdlog::warn("  - Item '{}' is no longer valid (mod uninstalled?), removing from outfit",
                item.formKey);
            continue;
        }

        auto* armor = item.GetArmor();

        // Check if NPC actually has this item in their inventory
        auto inventory = actor->GetInventory([armor](RE::TESBoundObject& obj) {
            return obj.GetFormID() == armor->GetFormID();
        });

        if (inventory.empty()) {
            keysToRemove.push_back(item.formKey);
            spdlog::warn("  - NPC doesn't have '{}' ({}) in inventory, removing from outfit",
                armor->GetFullName(), item.formKey);
            continue;
        }

        validArmor.push_back(armor);
        spdlog::info("  - Equipping '{}' ({})",
            armor->GetFullName(), item.formKey);
    }

    // If we had items to remove, update the stored outfit
    if (!keysToRemove.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_outfits.find(key);
        if (it != m_outfits.end()) {
            // Remove items that are no longer valid or NPC doesn't have
            it->second.items.erase(
                std::remove_if(it->second.items.begin(), it->second.items.end(),
                    [&keysToRemove](const SavedArmorItem& item) {
                        return std::find(keysToRemove.begin(), keysToRemove.end(), item.formKey) != keysToRemove.end();
                    }),
                it->second.items.end());
            spdlog::info("OutfitLockManager::ApplyOutfit - Removed {} items from outfit",
                keysToRemove.size());
        }
    }

    // Equip valid armor
    EquipArmorList(actor, validArmor);

    // Equip weapons if weapon lock is active
    if (!outfit.weaponUnlocked) {
        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (equipManager) {
            // Collect valid weapons
            std::vector<std::string> weaponKeysToRemove;
            for (const auto& wItem : outfit.weapons) {
                auto* weapon = wItem.GetWeapon();
                if (!weapon) {
                    weaponKeysToRemove.push_back(wItem.formKey);
                    spdlog::warn("  - Weapon '{}' is no longer valid, removing", wItem.formKey);
                    continue;
                }

                // Check if NPC has weapon in inventory
                auto inventory = actor->GetInventory([weapon](RE::TESBoundObject& obj) {
                    return obj.GetFormID() == weapon->GetFormID();
                });

                if (inventory.empty()) {
                    weaponKeysToRemove.push_back(wItem.formKey);
                    spdlog::warn("  - NPC doesn't have weapon '{}' in inventory, removing", weapon->GetFullName());
                    continue;
                }

                equipManager->EquipObject(actor, weapon, nullptr, 1, nullptr, true, false, false);
                spdlog::info("  - Equipped weapon '{}'", weapon->GetFullName());
            }

            // Remove invalid weapons from stored outfit
            if (!weaponKeysToRemove.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_outfits.find(key);
                if (it != m_outfits.end()) {
                    it->second.weapons.erase(
                        std::remove_if(it->second.weapons.begin(), it->second.weapons.end(),
                            [&weaponKeysToRemove](const SavedArmorItem& item) {
                                return std::find(weaponKeysToRemove.begin(), weaponKeysToRemove.end(), item.formKey) != weaponKeysToRemove.end();
                            }),
                        it->second.weapons.end());
                }
            }
        }
    }

    spdlog::info("OutfitLockManager::ApplyOutfit - Applied {} armor, {} weapons from outfit '{}'",
        validArmor.size(), outfit.weapons.size(), outfitName);

    return true;
}

bool OutfitLockManager::DeleteOutfit(RE::Actor* actor, const std::string& outfitName)
{
    if (!actor) return false;

    OutfitKey key{actor->GetFormID(), outfitName};

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_outfits.find(key);
    if (it == m_outfits.end()) {
        spdlog::warn("OutfitLockManager::DeleteOutfit - No outfit '{}' found for actor '{}'",
            outfitName, actor->GetName());
        return false;
    }

    m_outfits.erase(it);
    spdlog::info("OutfitLockManager::DeleteOutfit - Deleted outfit '{}' for actor '{}'",
        outfitName, actor->GetName());

    return true;
}

bool OutfitLockManager::HasOutfit(RE::Actor* actor, const std::string& outfitName) const
{
    if (!actor) return false;

    OutfitKey key{actor->GetFormID(), outfitName};

    std::lock_guard<std::mutex> lock(m_mutex);
    return m_outfits.find(key) != m_outfits.end();
}

bool OutfitLockManager::Lock(RE::Actor* actor)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::Lock - No actor provided");
        return false;
    }

    if (actor->IsPlayerRef()) {
        spdlog::warn("OutfitLockManager::Lock - Cannot lock player");
        return false;
    }

    spdlog::info("OutfitLockManager::Lock - Locking actor '{}' (0x{:08X})",
        actor->GetName(), actor->GetFormID());

    return SaveOutfit(actor, "locked");
}

bool OutfitLockManager::Unlock(RE::Actor* actor)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::Unlock - No actor provided");
        return false;
    }

    spdlog::info("OutfitLockManager::Unlock - Unlocking actor '{}' (0x{:08X})",
        actor->GetName(), actor->GetFormID());

    return DeleteOutfit(actor, "locked");
}

bool OutfitLockManager::IsLocked(RE::Actor* actor) const
{
    return HasOutfit(actor, "locked");
}

void OutfitLockManager::AddToLockedOutfit(RE::Actor* actor, RE::TESBoundObject* item)
{
    if (!actor || !item) return;
    if (!IsLocked(actor)) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    OutfitKey key{actor->GetFormID(), "locked"};
    auto it = m_outfits.find(key);
    if (it == m_outfits.end()) return;

    std::string formKey = Persistence::FormKeyUtil::BuildFormKey(item);
    if (formKey.empty()) {
        spdlog::warn("OutfitLockManager::AddToLockedOutfit - Cannot build FormKey for '{}'", item->GetName());
        return;
    }

    if (item->Is(RE::FormType::Armor)) {
        auto* armor = item->As<RE::TESObjectARMO>();
        if (!armor) return;

        // Remove any existing item in the same slot to avoid conflicts
        auto slotMask = armor->GetSlotMask();
        using SlotType = std::underlying_type_t<RE::BIPED_MODEL::BipedObjectSlot>;
        it->second.items.erase(
            std::remove_if(it->second.items.begin(), it->second.items.end(),
                [slotMask](const SavedArmorItem& existing) {
                    auto* existingArmor = existing.GetArmor();
                    return existingArmor &&
                        (static_cast<SlotType>(existingArmor->GetSlotMask()) & static_cast<SlotType>(slotMask)) != 0;
                }),
            it->second.items.end());

        SavedArmorItem newItem;
        newItem.formKey = formKey;
        it->second.items.push_back(newItem);

        spdlog::info("OutfitLockManager::AddToLockedOutfit - Added armor '{}' to locked outfit for '{}'",
            item->GetName(), actor->GetName());
    }
    else if (item->Is(RE::FormType::Weapon)) {
        // Check if already in weapon list
        for (const auto& w : it->second.weapons) {
            if (w.GetRuntimeFormID() == item->GetFormID()) {
                return;  // already in list
            }
        }

        SavedArmorItem newItem;
        newItem.formKey = formKey;
        it->second.weapons.push_back(newItem);
        it->second.weaponUnlocked = false;  // explicit weapon change locks weapons

        spdlog::info("OutfitLockManager::AddToLockedOutfit - Added weapon '{}' to locked outfit for '{}'",
            item->GetName(), actor->GetName());
    }
}

void OutfitLockManager::RemoveFromLockedOutfit(RE::Actor* actor, RE::TESBoundObject* item)
{
    if (!actor || !item) return;
    if (!IsLocked(actor)) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    OutfitKey key{actor->GetFormID(), "locked"};
    auto it = m_outfits.find(key);
    if (it == m_outfits.end()) return;

    RE::FormID targetID = item->GetFormID();

    if (item->Is(RE::FormType::Armor)) {
        it->second.items.erase(
            std::remove_if(it->second.items.begin(), it->second.items.end(),
                [targetID](const SavedArmorItem& existing) {
                    return existing.GetRuntimeFormID() == targetID;
                }),
            it->second.items.end());

        spdlog::info("OutfitLockManager::RemoveFromLockedOutfit - Removed armor '{}' from locked outfit for '{}'",
            item->GetName(), actor->GetName());
    }
    else if (item->Is(RE::FormType::Weapon)) {
        it->second.weapons.erase(
            std::remove_if(it->second.weapons.begin(), it->second.weapons.end(),
                [targetID](const SavedArmorItem& existing) {
                    return existing.GetRuntimeFormID() == targetID;
                }),
            it->second.weapons.end());

        spdlog::info("OutfitLockManager::RemoveFromLockedOutfit - Removed weapon '{}' from locked outfit for '{}'",
            item->GetName(), actor->GetName());
    }
}

void OutfitLockManager::SetLockedOutfitData(RE::Actor* actor, const SavedOutfit& outfit)
{
    if (!actor) return;

    OutfitKey key{actor->GetFormID(), "locked"};

    std::lock_guard<std::mutex> lock(m_mutex);
    m_outfits[key] = outfit;

    spdlog::info("OutfitLockManager::SetLockedOutfitData - Set locked outfit for '{}' ({} armor, {} weapons, weaponUnlocked={})",
        actor->GetName(), outfit.items.size(), outfit.weapons.size(), outfit.weaponUnlocked);
}

std::optional<SavedOutfit> OutfitLockManager::GetOutfitData(RE::Actor* actor, const std::string& outfitName) const
{
    if (!actor) return std::nullopt;

    OutfitKey key{actor->GetFormID(), outfitName};

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_outfits.find(key);
    if (it == m_outfits.end()) return std::nullopt;

    return it->second;
}

bool OutfitLockManager::IsInLockedOutfit(RE::Actor* actor, RE::TESBoundObject* item) const
{
    if (!actor || !item) return false;

    OutfitKey key{actor->GetFormID(), "locked"};
    RE::FormID targetID = item->GetFormID();

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_outfits.find(key);
    if (it == m_outfits.end()) return false;

    if (item->Is(RE::FormType::Armor)) {
        for (const auto& saved : it->second.items) {
            if (saved.GetRuntimeFormID() == targetID) return true;
        }
        return false;
    }

    if (item->Is(RE::FormType::Weapon)) {
        for (const auto& saved : it->second.weapons) {
            if (saved.GetRuntimeFormID() == targetID) return true;
        }
        return false;
    }

    return false;
}

bool OutfitLockManager::IsWeaponUnlocked(RE::Actor* actor) const
{
    if (!actor) return true;

    OutfitKey key{actor->GetFormID(), "locked"};

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_outfits.find(key);
    if (it == m_outfits.end()) return true;

    return it->second.weaponUnlocked;
}

void OutfitLockManager::SetWeaponUnlocked(RE::Actor* actor, bool unlocked)
{
    if (!actor) return;

    OutfitKey key{actor->GetFormID(), "locked"};

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_outfits.find(key);
    if (it == m_outfits.end()) return;

    it->second.weaponUnlocked = unlocked;

    spdlog::info("OutfitLockManager::SetWeaponUnlocked - Set weaponUnlocked={} for '{}'",
        unlocked, actor->GetName());
}

void OutfitLockManager::MarkItemAsPlayerGiven(RE::Actor* actor, RE::FormID itemID)
{
    if (!actor || itemID == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_playerGivenItems[actor->GetFormID()].insert(itemID);

    spdlog::info("OutfitLockManager::MarkItemAsPlayerGiven - Marked item 0x{:08X} as player-given for '{}'",
        itemID, actor->GetName());
}

bool OutfitLockManager::HasPlayerItems(RE::Actor* actor) const
{
    if (!actor) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_playerGivenItems.find(actor->GetFormID());
    if (it == m_playerGivenItems.end() || it->second.empty()) {
        return false;
    }

    // Check if any tracked items are still in NPC's inventory
    for (RE::FormID itemID : it->second) {
        auto* form = RE::TESForm::LookupByID(itemID);
        if (!form) continue;

        auto inventory = actor->GetInventory([itemID](RE::TESBoundObject& obj) {
            return obj.GetFormID() == itemID;
        });

        if (!inventory.empty()) {
            return true;  // At least one player item is still in inventory
        }
    }
    return false;
}

void OutfitLockManager::ReturnPlayerItems(RE::Actor* actor)
{
    if (!actor) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!player || !equipManager) return;

    std::unordered_set<RE::FormID> itemsToReturn;
    RE::FormID actorID = actor->GetFormID();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_playerGivenItems.find(actorID);
        if (it == m_playerGivenItems.end() || it->second.empty()) {
            spdlog::info("OutfitLockManager::ReturnPlayerItems - No player items tracked for '{}'",
                actor->GetName());
            return;
        }
        itemsToReturn = it->second;  // Copy to work outside lock
    }

    spdlog::info("OutfitLockManager::ReturnPlayerItems - Checking {} tracked items for '{}'",
        itemsToReturn.size(), actor->GetName());

    std::uint32_t returnedCount = 0;
    std::uint32_t missingCount = 0;
    std::vector<RE::FormID> returnedItems;

    for (RE::FormID itemID : itemsToReturn) {
        auto* form = RE::TESForm::LookupByID(itemID);
        if (!form) {
            ++missingCount;
            continue;
        }

        // Check if NPC still has this item in inventory
        auto inventory = actor->GetInventory([itemID](RE::TESBoundObject& obj) {
            return obj.GetFormID() == itemID;
        });

        if (inventory.empty()) {
            spdlog::trace("  - NPC no longer has item 0x{:08X}", itemID);
            ++missingCount;
            continue;
        }

        // Unequip if it's armor
        if (auto* armor = form->As<RE::TESObjectARMO>()) {
            equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, true);
        }

        // Transfer to player
        actor->RemoveItem(form->As<RE::TESBoundObject>(), 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, player);

        spdlog::info("  - Returned '{}' (0x{:08X}) to player", form->GetName(), itemID);
        returnedItems.push_back(itemID);
        ++returnedCount;
    }

    // Remove returned items from tracking
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Clean up returned items from tracking
        if (!returnedItems.empty()) {
            auto it = m_playerGivenItems.find(actorID);
            if (it != m_playerGivenItems.end()) {
                for (RE::FormID itemID : returnedItems) {
                    it->second.erase(itemID);
                }
                // Clean up empty entries
                if (it->second.empty()) {
                    m_playerGivenItems.erase(it);
                }
            }
        }
    }

    spdlog::info("OutfitLockManager::ReturnPlayerItems - Returned {} items to player from '{}' ({} missing/invalid)",
        returnedCount, actor->GetName(), missingCount);
}

void OutfitLockManager::MarkItemAsGallerySpawned(RE::Actor* actor, RE::FormID itemID)
{
    if (!actor || itemID == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_gallerySpawnedItems[actor->GetFormID()].insert(itemID);

    spdlog::info("OutfitLockManager::MarkItemAsGallerySpawned - Marked item 0x{:08X} as gallery-spawned for '{}'",
        itemID, actor->GetName());
}

bool OutfitLockManager::IsGallerySpawnedItem(RE::Actor* actor, RE::FormID itemID) const
{
    if (!actor || itemID == 0) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gallerySpawnedItems.find(actor->GetFormID());
    if (it == m_gallerySpawnedItems.end()) return false;

    return it->second.contains(itemID);
}

void OutfitLockManager::UnmarkGalleryItem(RE::Actor* actor, RE::FormID itemID)
{
    if (!actor || itemID == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gallerySpawnedItems.find(actor->GetFormID());
    if (it != m_gallerySpawnedItems.end()) {
        it->second.erase(itemID);
        // Clean up empty entries
        if (it->second.empty()) {
            m_gallerySpawnedItems.erase(it);
        }
    }
}

bool OutfitLockManager::HasDefaultOutfit(RE::Actor* actor) const
{
    return HasOutfit(actor, "default");
}

bool OutfitLockManager::RestoreAndClearDefault(RE::Actor* actor)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::RestoreAndClearDefault - No actor provided");
        return false;
    }

    if (!HasOutfit(actor, "default")) {
        spdlog::warn("OutfitLockManager::RestoreAndClearDefault - No default outfit for '{}'",
            actor->GetName());
        return false;
    }

    spdlog::info("OutfitLockManager::RestoreAndClearDefault - Restoring '{}' to vanilla",
        actor->GetName());

    // Return player items before restoring
    ReturnPlayerItems(actor);

    // Apply the default outfit (unequip others first)
    bool applied = ApplyOutfit(actor, "default", true);

    // Delete the default outfit after restoring
    DeleteOutfit(actor, "default");

    // Also unlock the NPC since they're back to vanilla
    Unlock(actor);

    spdlog::info("OutfitLockManager::RestoreAndClearDefault - '{}' restored to vanilla (success={})",
        actor->GetName(), applied);

    return applied;
}

void OutfitLockManager::OnGameSave(SKSE::SerializationInterface* a_intfc)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    spdlog::info("OutfitLockManager::OnGameSave - Saving {} outfits, {} actors with player items",
        mgr->m_outfits.size(), mgr->m_playerGivenItems.size());

    // === Save Outfits ===
    if (!a_intfc->OpenRecord(kOutfitRecord, kSerializationVersion)) {
        spdlog::error("OutfitLockManager::OnGameSave - Failed to open outfit record");
        return;
    }

    // Write number of outfits
    std::uint32_t outfitCount = static_cast<std::uint32_t>(mgr->m_outfits.size());
    a_intfc->WriteRecordData(&outfitCount, sizeof(outfitCount));

    for (const auto& [key, outfit] : mgr->m_outfits) {
        // Write actor ref ID
        a_intfc->WriteRecordData(&key.actorRefID, sizeof(key.actorRefID));

        // Write outfit name (length + chars)
        std::uint32_t nameLen = static_cast<std::uint32_t>(key.outfitName.size());
        a_intfc->WriteRecordData(&nameLen, sizeof(nameLen));
        a_intfc->WriteRecordData(key.outfitName.data(), nameLen);

        // Write number of items
        std::uint32_t itemCount = static_cast<std::uint32_t>(outfit.items.size());
        a_intfc->WriteRecordData(&itemCount, sizeof(itemCount));

        // Write each armor item's formKey string (length-prefixed)
        for (const auto& item : outfit.items) {
            std::uint32_t keyLen = static_cast<std::uint32_t>(item.formKey.size());
            a_intfc->WriteRecordData(&keyLen, sizeof(keyLen));
            a_intfc->WriteRecordData(item.formKey.data(), keyLen);
        }

        // v5: Write weapon items
        std::uint32_t weaponCount = static_cast<std::uint32_t>(outfit.weapons.size());
        a_intfc->WriteRecordData(&weaponCount, sizeof(weaponCount));

        for (const auto& weapon : outfit.weapons) {
            std::uint32_t keyLen = static_cast<std::uint32_t>(weapon.formKey.size());
            a_intfc->WriteRecordData(&keyLen, sizeof(keyLen));
            a_intfc->WriteRecordData(weapon.formKey.data(), keyLen);
        }

        // v5: Write weaponUnlocked flag
        std::uint8_t weaponUnlocked = outfit.weaponUnlocked ? 1 : 0;
        a_intfc->WriteRecordData(&weaponUnlocked, sizeof(weaponUnlocked));

        spdlog::info("  - Saved outfit '{}' for actor 0x{:08X} with {} armor, {} weapons (weaponUnlocked={})",
            key.outfitName, key.actorRefID, itemCount, weaponCount, outfit.weaponUnlocked);
    }

    // === Save Player Given Items ===
    if (!a_intfc->OpenRecord(kPlayerItemsRecord, kSerializationVersion)) {
        spdlog::error("OutfitLockManager::OnGameSave - Failed to open player items record");
        return;
    }

    // Write number of actors with player items
    std::uint32_t actorCount = static_cast<std::uint32_t>(mgr->m_playerGivenItems.size());
    a_intfc->WriteRecordData(&actorCount, sizeof(actorCount));

    for (const auto& [actorID, items] : mgr->m_playerGivenItems) {
        // Write actor ref ID
        a_intfc->WriteRecordData(&actorID, sizeof(actorID));

        // Write number of items
        std::uint32_t itemCount = static_cast<std::uint32_t>(items.size());
        a_intfc->WriteRecordData(&itemCount, sizeof(itemCount));

        // Write each item FormID
        for (RE::FormID itemID : items) {
            a_intfc->WriteRecordData(&itemID, sizeof(itemID));
        }

        spdlog::info("  - Saved {} player items for actor 0x{:08X}", itemCount, actorID);
    }

    spdlog::info("OutfitLockManager::OnGameSave - Done");
}

void OutfitLockManager::OnPreLoad()
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    mgr->m_outfits.clear();
    mgr->m_playerGivenItems.clear();

    spdlog::info("OutfitLockManager::OnPreLoad - Cleared state");
}

void OutfitLockManager::OnLoadRecord(SKSE::SerializationInterface* a_intfc,
    std::uint32_t type, std::uint32_t version, std::uint32_t length)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    if (type == kOutfitRecord) {
            // Support v4 (armor-only FormKeys) and v5 (armor + weapons + weaponUnlocked)
            if (version != kSerializationVersion && version != 4) {
                spdlog::warn("OutfitLockManager::OnLoadRecord - Incompatible outfit version {} (expected 4 or {}), skipping",
                    version, kSerializationVersion);
                if (length > 0) {
                    std::vector<char> skipBuffer(length);
                    a_intfc->ReadRecordData(skipBuffer.data(), length);
                }
                return;
            }

            // Read number of outfits
            std::uint32_t outfitCount = 0;
            a_intfc->ReadRecordData(&outfitCount, sizeof(outfitCount));

            for (std::uint32_t i = 0; i < outfitCount; ++i) {
                // Read actor ref ID
                RE::FormID oldActorID = 0;
                a_intfc->ReadRecordData(&oldActorID, sizeof(oldActorID));

                RE::FormID newActorID = 0;
                if (!a_intfc->ResolveFormID(oldActorID, newActorID)) {
                    spdlog::warn("  - Cannot resolve actor 0x{:08X}, skipping outfit", oldActorID);

                    // Skip outfit name
                    std::uint32_t nameLen = 0;
                    a_intfc->ReadRecordData(&nameLen, sizeof(nameLen));
                    std::string dummyName(nameLen, '\0');
                    a_intfc->ReadRecordData(dummyName.data(), nameLen);

                    // Skip armor items
                    std::uint32_t itemCount = 0;
                    a_intfc->ReadRecordData(&itemCount, sizeof(itemCount));
                    for (std::uint32_t j = 0; j < itemCount; ++j) {
                        std::uint32_t keyLen = 0;
                        a_intfc->ReadRecordData(&keyLen, sizeof(keyLen));
                        std::string dummyKey(keyLen, '\0');
                        a_intfc->ReadRecordData(dummyKey.data(), keyLen);
                    }

                    // v5: also skip weapon items and weaponUnlocked
                    if (version >= 5) {
                        std::uint32_t weaponCount = 0;
                        a_intfc->ReadRecordData(&weaponCount, sizeof(weaponCount));
                        for (std::uint32_t j = 0; j < weaponCount; ++j) {
                            std::uint32_t keyLen = 0;
                            a_intfc->ReadRecordData(&keyLen, sizeof(keyLen));
                            std::string dummyKey(keyLen, '\0');
                            a_intfc->ReadRecordData(dummyKey.data(), keyLen);
                        }
                        std::uint8_t dummyFlag = 0;
                        a_intfc->ReadRecordData(&dummyFlag, sizeof(dummyFlag));
                    }
                    continue;
                }

                // Read outfit name
                std::uint32_t nameLen = 0;
                a_intfc->ReadRecordData(&nameLen, sizeof(nameLen));
                std::string outfitName(nameLen, '\0');
                a_intfc->ReadRecordData(outfitName.data(), nameLen);

                // Read armor items (formKey strings)
                std::uint32_t itemCount = 0;
                a_intfc->ReadRecordData(&itemCount, sizeof(itemCount));

                SavedOutfit outfit;

                for (std::uint32_t j = 0; j < itemCount; ++j) {
                    std::uint32_t keyLen = 0;
                    a_intfc->ReadRecordData(&keyLen, sizeof(keyLen));
                    std::string formKey(keyLen, '\0');
                    a_intfc->ReadRecordData(formKey.data(), keyLen);

                    SavedArmorItem item;
                    item.formKey = formKey;
                    outfit.items.push_back(item);

                    spdlog::trace("    - Loaded armor key: {}", formKey);
                }

                // v5: Read weapon items and weaponUnlocked flag
                if (version >= 5) {
                    std::uint32_t weaponCount = 0;
                    a_intfc->ReadRecordData(&weaponCount, sizeof(weaponCount));

                    for (std::uint32_t j = 0; j < weaponCount; ++j) {
                        std::uint32_t keyLen = 0;
                        a_intfc->ReadRecordData(&keyLen, sizeof(keyLen));
                        std::string formKey(keyLen, '\0');
                        a_intfc->ReadRecordData(formKey.data(), keyLen);

                        SavedArmorItem wItem;
                        wItem.formKey = formKey;
                        outfit.weapons.push_back(wItem);

                        spdlog::trace("    - Loaded weapon key: {}", formKey);
                    }

                    std::uint8_t weaponUnlocked = 1;
                    a_intfc->ReadRecordData(&weaponUnlocked, sizeof(weaponUnlocked));
                    outfit.weaponUnlocked = (weaponUnlocked != 0);
                } else {
                    // v4 migration: no weapon data, default to unlocked
                    outfit.weapons.clear();
                    outfit.weaponUnlocked = true;
                    spdlog::info("    - v4→v5 migration: defaulting weapons unlocked");
                }

                // Store the outfit
                OutfitKey key{newActorID, outfitName};
                mgr->m_outfits[key] = std::move(outfit);

                spdlog::info("  - Loaded outfit '{}' for actor 0x{:08X} ({} armor, {} weapons, weaponUnlocked={})",
                    outfitName, newActorID, itemCount,
                    version >= 5 ? mgr->m_outfits[key].weapons.size() : 0,
                    mgr->m_outfits[key].weaponUnlocked);
            }
        }
        else if (type == kPlayerItemsRecord) {
            // Read number of actors
            std::uint32_t actorCount = 0;
            a_intfc->ReadRecordData(&actorCount, sizeof(actorCount));

            for (std::uint32_t i = 0; i < actorCount; ++i) {
                // Read actor ref ID
                RE::FormID oldActorID = 0;
                a_intfc->ReadRecordData(&oldActorID, sizeof(oldActorID));

                // Read number of items
                std::uint32_t itemCount = 0;
                a_intfc->ReadRecordData(&itemCount, sizeof(itemCount));

                // Resolve actor form ID
                RE::FormID newActorID = 0;
                if (!a_intfc->ResolveFormID(oldActorID, newActorID)) {
                    spdlog::warn("  - Cannot resolve actor 0x{:08X}, skipping player items", oldActorID);
                    // Skip items
                    for (std::uint32_t j = 0; j < itemCount; ++j) {
                        RE::FormID dummyID = 0;
                        a_intfc->ReadRecordData(&dummyID, sizeof(dummyID));
                    }
                    continue;
                }

                std::unordered_set<RE::FormID> items;
                std::uint32_t validCount = 0;

                for (std::uint32_t j = 0; j < itemCount; ++j) {
                    RE::FormID oldItemID = 0;
                    a_intfc->ReadRecordData(&oldItemID, sizeof(oldItemID));

                    RE::FormID newItemID = 0;
                    if (a_intfc->ResolveFormID(oldItemID, newItemID)) {
                        items.insert(newItemID);
                        ++validCount;
                    } else {
                        spdlog::warn("    - Cannot resolve item 0x{:08X}", oldItemID);
                    }
                }

                if (!items.empty()) {
                    mgr->m_playerGivenItems[newActorID] = std::move(items);
                    spdlog::info("  - Loaded {} player items for actor 0x{:08X}", validCount, newActorID);
                }
            }
        }
    else {
        // Unknown record type — skip its data
        if (length > 0) {
            std::vector<char> skipBuffer(length);
            a_intfc->ReadRecordData(skipBuffer.data(), length);
        }
        spdlog::warn("OutfitLockManager::OnLoadRecord - Unknown record type: {:08X}, skipped {} bytes", type, length);
    }
}

void OutfitLockManager::OnGameLoad(SKSE::SerializationInterface* a_intfc)
{
    OnPreLoad();

    spdlog::info("OutfitLockManager::OnGameLoad - Loading data");

    std::uint32_t type, version, length;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        OnLoadRecord(a_intfc, type, version, length);
    }

    auto* mgr = GetSingleton();
    spdlog::info("OutfitLockManager::OnGameLoad - Loaded {} outfits, {} actors with player items",
        mgr->m_outfits.size(), mgr->m_playerGivenItems.size());
}

void OutfitLockManager::OnRevert(SKSE::SerializationInterface*)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    spdlog::info("OutfitLockManager::OnRevert - Clearing {} outfits, {} actors with player items, {} actors with gallery items",
        mgr->m_outfits.size(), mgr->m_playerGivenItems.size(), mgr->m_gallerySpawnedItems.size());
    mgr->m_outfits.clear();
    mgr->m_playerGivenItems.clear();
    mgr->m_gallerySpawnedItems.clear();
}

void OutfitLockManager::OnPostLoadGame()
{
    spdlog::info("OutfitLockManager::OnPostLoadGame - Scanning for locked NPCs");

    // On post load game, try to get player's current location
    auto* player = RE::PlayerCharacter::GetSingleton();
    RE::BGSLocation* playerLoc = nullptr;
    if (player) {
        playerLoc = player->GetCurrentLocation();
    }

    ApplyLockedOutfitsInLocation(playerLoc);
}

void OutfitLockManager::ApplyLockedOutfitsInLocation(RE::BGSLocation* location)
{
    std::string locName = "Unknown";
    if (location && location->fullName.c_str()) {
        locName = location->fullName.c_str();
    }

    spdlog::info("OutfitLockManager::ApplyLockedOutfitsInLocation - Scanning location '{}'", locName);

    // Collect actors to process while holding the lock
    std::vector<RE::Actor*> actorsToProcess;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_outfits.empty()) {
            spdlog::trace("OutfitLockManager::ApplyLockedOutfitsInLocation - No outfits stored");
            return;
        }

        for (const auto& [key, outfit] : m_outfits) {
            if (key.outfitName != "locked") continue;

            // Look up the actor
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(key.actorRefID);
            if (!actor) {
                spdlog::trace("  - Actor 0x{:08X} not found", key.actorRefID);
                continue;
            }

            // Check if actor is in the same location
            auto* actorLoc = actor->GetCurrentLocation();
            if (location && actorLoc != location) {
                spdlog::trace("  - Actor '{}' is in different location", actor->GetName());
                continue;
            }

            // If no location specified, just apply to all locked actors that are loaded
            if (!location && !actor->Is3DLoaded()) {
                spdlog::trace("  - Actor '{}' is not 3D loaded", actor->GetName());
                continue;
            }

            spdlog::info("  - Found locked actor '{}' in location",
                actor->GetName());
            actorsToProcess.push_back(actor);
        }
    }  // Lock released here

    // Apply outfits outside the lock to avoid deadlock
    for (auto* actor : actorsToProcess) {
        ApplyOutfit(actor, "locked", true);
    }
}

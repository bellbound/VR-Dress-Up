#include "OutfitLockManager.h"
#include "OutfitFormBackend.h"
#include "SeverActionsCompat.h"
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

    // Assigning an outfit makes the engine queue an UnequipAll, so the events that
    // arrive just after are our own doing, not the player's.
    if (OutfitFormBackend::GetSingleton()->IsApplying()) {
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

    OutfitKey key{actor->GetFormID(), outfitName};

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outfits[key] = std::move(outfit);
    }

    spdlog::info("OutfitLockManager::SaveOutfit - Saved {} items for outfit '{}'",
        equipped.size(), outfitName);

    return true;
}

bool OutfitLockManager::ApplyOutfit(RE::Actor* actor, const std::string& outfitName, bool unequipOthers)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::ApplyOutfit - No actor provided");
        return false;
    }

    // Restore anything the actor is missing before the loop below gets to it. That
    // loop prunes items the actor does not have, and the pruning rewrites the stored
    // outfit - so a SPID reapply, which strips the previous outfit's items out of the
    // inventory, would otherwise make the lock delete itself one piece at a time.
    if (outfitName == "locked") {
        EnsureOutfitItemsInInventory(actor, outfitName);
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

    spdlog::info("OutfitLockManager::ApplyOutfit - Applied {} items from outfit '{}'",
        validArmor.size(), outfitName);

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

    if (!SaveOutfit(actor, "locked")) {
        return false;
    }

    // Hand the look to whoever will defend it. SeverActions maintains it through its
    // own alias if it is installed; the outfit record is what makes SPID stand down.
    SeverActionsCompat::HandOffOutfit(actor);
    PromoteLockToOutfitForm(actor);

    return true;
}

bool OutfitLockManager::Unlock(RE::Actor* actor)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::Unlock - No actor provided");
        return false;
    }

    spdlog::info("OutfitLockManager::Unlock - Unlocking actor '{}' (0x{:08X})",
        actor->GetName(), actor->GetFormID());

    // Give the NPC back to whoever had them: restoring the original outfit is what
    // lets SPID resume distributing to this actor.
    SeverActionsCompat::ReleaseOutfit(actor);
    OutfitFormBackend::GetSingleton()->Restore(actor);

    return DeleteOutfit(actor, "locked");
}

bool OutfitLockManager::IsLocked(RE::Actor* actor) const
{
    return HasOutfit(actor, "locked");
}

void OutfitLockManager::PromoteLockToOutfitForm(RE::Actor* actor)
{
    if (!actor) return;

    auto* backend = OutfitFormBackend::GetSingleton();
    if (!backend->IsAvailable() || !backend->IsEligible(actor)) return;

    // Under SeverActions the record we assign is one of its own, which it only creates
    // once it has finished building the preset. Ask for it, and assign whatever comes
    // back - our own pool if it never produces one.
    if (SeverActionsCompat::IsActive() && !backend->HasOutfitRecord(actor)) {
        backend->ReacquireExternal(actor);
        return;
    }

    backend->Apply(actor, GetOutfitItemFormKeys(actor, "locked"));
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

        // Write each item's formKey string (length-prefixed)
        for (const auto& item : outfit.items) {
            std::uint32_t keyLen = static_cast<std::uint32_t>(item.formKey.size());
            a_intfc->WriteRecordData(&keyLen, sizeof(keyLen));
            a_intfc->WriteRecordData(item.formKey.data(), keyLen);
        }

        // v5: the actor's own FormKey. `key.actorRefID` above is a *runtime* ref ID
        // and is only meaningful inside the load order that produced it, so on its
        // own an outfit can end up attached to a different NPC after a plugin is
        // added or removed. Derived fresh here rather than trusting a cached value,
        // so it is always consistent with the ID beside it.
        std::string actorFormKey = outfit.actorFormKey;
        if (actorFormKey.empty()) {
            if (auto* actorForm = RE::TESForm::LookupByID(key.actorRefID)) {
                actorFormKey = Persistence::FormKeyUtil::BuildFormKey(actorForm);
            }
        }
        std::uint32_t actorKeyLen = static_cast<std::uint32_t>(actorFormKey.size());
        a_intfc->WriteRecordData(&actorKeyLen, sizeof(actorKeyLen));
        if (actorKeyLen > 0) {
            a_intfc->WriteRecordData(actorFormKey.data(), actorKeyLen);
        }

        spdlog::info("  - Saved outfit '{}' for actor 0x{:08X} ('{}') with {} items",
            key.outfitName, key.actorRefID,
            actorFormKey.empty() ? "dynamic/unknown" : actorFormKey, itemCount);
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
            // v4 and v5 are both readable: v5 only appends a field per outfit, so an
            // older co-save loads fine and simply carries no actorFormKey.
            if (version < kMinReadableVersion || version > kSerializationVersion) {
                spdlog::warn("OutfitLockManager::OnLoadRecord - Incompatible outfit version {} (readable range {}..{}), skipping",
                    version, kMinReadableVersion, kSerializationVersion);
                // Skip by consuming remaining bytes (already read type/version/length)
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
                // Read actor ref ID (still uses SKSE resolution for NPCs)
                RE::FormID oldActorID = 0;
                a_intfc->ReadRecordData(&oldActorID, sizeof(oldActorID));

                RE::FormID newActorID = 0;
                const bool skseResolved = a_intfc->ResolveFormID(oldActorID, newActorID);

                // The record still has to be consumed in full even when we intend to
                // drop it, or every subsequent outfit reads from the wrong offset.
                // Read first, decide afterwards.
                std::uint32_t nameLen = 0;
                a_intfc->ReadRecordData(&nameLen, sizeof(nameLen));
                std::string outfitName(nameLen, '\0');
                a_intfc->ReadRecordData(outfitName.data(), nameLen);

                std::uint32_t itemCount = 0;
                a_intfc->ReadRecordData(&itemCount, sizeof(itemCount));

                SavedOutfit outfit;
                for (std::uint32_t j = 0; j < itemCount; ++j) {
                    std::uint32_t keyLen = 0;
                    a_intfc->ReadRecordData(&keyLen, sizeof(keyLen));
                    std::string formKey(keyLen, '\0');
                    a_intfc->ReadRecordData(formKey.data(), keyLen);

                    // FormKey resolution happens lazily in IsValid()/GetArmor().
                    SavedArmorItem item;
                    item.formKey = formKey;
                    outfit.items.push_back(item);
                    spdlog::trace("    - Loaded armor key: {}", formKey);
                }

                if (version >= 5) {
                    std::uint32_t actorKeyLen = 0;
                    a_intfc->ReadRecordData(&actorKeyLen, sizeof(actorKeyLen));
                    if (actorKeyLen > 0) {
                        outfit.actorFormKey.resize(actorKeyLen);
                        a_intfc->ReadRecordData(outfit.actorFormKey.data(), actorKeyLen);
                    }
                }

                // Prefer the FormKey over SKSE's ref-ID resolution. SKSE can only
                // remap an ID whose *plugin* is still at a known index; the FormKey
                // carries the plugin name, so it survives reordering that the raw ID
                // does not.
                if (!outfit.actorFormKey.empty()) {
                    const RE::FormID fromKey =
                        Persistence::FormKeyUtil::ResolveToRuntimeFormID(outfit.actorFormKey);
                    if (fromKey != 0) {
                        if (skseResolved && fromKey != newActorID) {
                            spdlog::info(
                                "  - Actor key '{}' resolves to 0x{:08X}, SKSE said 0x{:08X}; "
                                "trusting the FormKey",
                                outfit.actorFormKey, fromKey, newActorID);
                        }
                        newActorID = fromKey;
                    }
                }

                if (newActorID == 0) {
                    spdlog::warn("  - Cannot resolve actor 0x{:08X} ('{}'), dropping outfit '{}'",
                        oldActorID, outfit.actorFormKey, outfitName);
                    continue;
                }

                const std::uint32_t validCount = static_cast<std::uint32_t>(outfit.items.size());
                OutfitKey key{newActorID, outfitName};
                mgr->m_outfits[key] = std::move(outfit);

                spdlog::info("  - Loaded outfit '{}' for actor 0x{:08X} with {} items",
                    outfitName, newActorID, validCount);
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

    // Outfit records carry their item list in memory only, so every one of them comes
    // back from a load empty. Rebuild before anything reads them.
    OutfitFormBackend::GetSingleton()->ReapplyAll();

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
        // Under SeverActions the unequip half of this would trip its alias debounce
        // and have it re-dress the NPC on top of us. Its own apply path brackets
        // itself the same way.
        SeverActionsCompat::SuspendLock(actor);
        ApplyOutfit(actor, "locked", true);
        SeverActionsCompat::ResumeLock(actor);
    }
}

// =============================================================================
// Interface002 support: enumeration and FormKey injection
// =============================================================================
//
// These are the surface Save Migration drives. They deal only in FormKeys, so an
// outfit set can be read out of one savegame and written into another without
// either side touching a runtime form ID.

std::vector<std::string> OutfitLockManager::EnumerateOutfitNames(RE::Actor* actor) const
{
    std::vector<std::string> names;
    if (!actor) {
        return names;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const RE::FormID actorID = actor->GetFormID();
    for (const auto& [key, outfit] : m_outfits) {
        if (key.actorRefID == actorID) {
            names.push_back(key.outfitName);
        }
    }
    return names;
}

std::vector<std::string> OutfitLockManager::GetOutfitItemFormKeys(RE::Actor* actor,
    const std::string& outfitName) const
{
    std::vector<std::string> keys;
    if (!actor) {
        return keys;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_outfits.find(OutfitKey{actor->GetFormID(), outfitName});
    if (it == m_outfits.end()) {
        return keys;
    }
    keys.reserve(it->second.items.size());
    for (const auto& item : it->second.items) {
        keys.push_back(item.formKey);
    }
    return keys;
}

std::vector<std::string> OutfitLockManager::GetPlayerGivenFormKeys(RE::Actor* actor) const
{
    std::vector<std::string> keys;
    if (!actor) {
        return keys;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_playerGivenItems.find(actor->GetFormID());
    if (it == m_playerGivenItems.end()) {
        return keys;
    }
    // The set holds runtime FormIDs; converting to FormKeys here is what makes the
    // list meaningful outside this session.
    for (const RE::FormID itemID : it->second) {
        if (auto* form = RE::TESForm::LookupByID(itemID)) {
            auto key = Persistence::FormKeyUtil::BuildFormKey(form);
            if (!key.empty()) {
                keys.push_back(std::move(key));
            }
        }
    }
    return keys;
}

std::uint32_t OutfitLockManager::SetOutfitFromFormKeys(RE::Actor* actor,
    const std::string& outfitName, const std::vector<std::string>& formKeys)
{
    if (!actor || outfitName.empty()) {
        return 0;
    }

    SavedOutfit outfit;
    outfit.actorFormKey = Persistence::FormKeyUtil::BuildFormKey(actor);

    std::uint32_t accepted = 0;
    for (const auto& formKey : formKeys) {
        if (formKey.empty()) {
            continue;
        }
        // Verify the key resolves to an actual armour piece before storing it.
        // Storing an unresolvable key would be pruned on the first apply anyway, and
        // the pruning rewrites the map - so it is cheaper and safer to reject here.
        const RE::FormID runtimeID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey);
        if (runtimeID == 0 || !RE::TESForm::LookupByID<RE::TESObjectARMO>(runtimeID)) {
            spdlog::warn("SetOutfitFromFormKeys - '{}' does not resolve to armour here, skipping",
                formKey);
            continue;
        }
        SavedArmorItem item;
        item.formKey = formKey;
        outfit.items.push_back(std::move(item));
        ++accepted;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outfits[OutfitKey{actor->GetFormID(), outfitName}] = std::move(outfit);
    }

    spdlog::info("SetOutfitFromFormKeys - '{}' for 0x{:08X}: {}/{} key(s) accepted",
        outfitName, actor->GetFormID(), accepted, formKeys.size());
    return accepted;
}

std::uint32_t OutfitLockManager::MarkPlayerGivenFromFormKeys(RE::Actor* actor,
    const std::vector<std::string>& formKeys)
{
    if (!actor) {
        return 0;
    }
    std::uint32_t accepted = 0;
    for (const auto& formKey : formKeys) {
        const RE::FormID runtimeID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey);
        if (runtimeID == 0) {
            continue;
        }
        MarkItemAsPlayerGiven(actor, runtimeID);
        ++accepted;
    }
    return accepted;
}

std::uint32_t OutfitLockManager::EnsureOutfitItemsInInventory(RE::Actor* actor,
    const std::string& outfitName)
{
    if (!actor) {
        return 0;
    }

    // Snapshot the keys under the lock, then add outside it: AddObjectToContainer can
    // fire equip events that re-enter this manager.
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_outfits.find(OutfitKey{actor->GetFormID(), outfitName});
        if (it == m_outfits.end()) {
            return 0;
        }
        for (const auto& item : it->second.items) {
            keys.push_back(item.formKey);
        }
    }

    std::uint32_t added = 0;
    for (const auto& formKey : keys) {
        auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(
            Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey));
        if (!armor) {
            continue;
        }
        const auto counts = actor->GetInventoryCounts();
        const auto found = counts.find(static_cast<RE::TESBoundObject*>(armor));
        if (found != counts.end() && found->second > 0) {
            continue;  // already has it
        }
        actor->AddObjectToContainer(armor, nullptr, 1, nullptr);
        ++added;
        spdlog::info("EnsureOutfitItemsInInventory - added '{}' ({}) to 0x{:08X}",
            armor->GetFullName(), formKey, actor->GetFormID());
    }

    if (added > 0) {
        spdlog::info("EnsureOutfitItemsInInventory - '{}': {} item(s) added. Without this, "
            "ApplyOutfit would prune them from the stored outfit permanently.",
            outfitName, added);
    }
    return added;
}

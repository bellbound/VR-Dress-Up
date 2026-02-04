#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include "FormKeyUtil.h"

// Stores a single armor item in a load-order-independent way
// Uses FormKey format: "0x[LocalFormID]~[PluginName]"
struct SavedArmorItem
{
    std::string formKey;  // e.g., "0x10C0E3~Skyrim.esm"

    // Returns true if this item is valid and can be equipped
    bool IsValid() const {
        if (formKey.empty()) return false;
        RE::FormID runtimeId = Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey);
        return runtimeId != 0 && RE::TESForm::LookupByID<RE::TESObjectARMO>(runtimeId) != nullptr;
    }

    RE::TESObjectARMO* GetArmor() const {
        if (formKey.empty()) return nullptr;
        RE::FormID runtimeId = Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey);
        return RE::TESForm::LookupByID<RE::TESObjectARMO>(runtimeId);
    }
};

// Stores an outfit (collection of armor items)
struct SavedOutfit
{
    std::vector<SavedArmorItem> items;
};

// Key for outfit storage: actor ref ID + outfit name
struct OutfitKey
{
    RE::FormID actorRefID = 0;
    std::string outfitName;

    bool operator==(const OutfitKey& other) const
    {
        return actorRefID == other.actorRefID && outfitName == other.outfitName;
    }
};

// Hash function for OutfitKey
struct OutfitKeyHash
{
    std::size_t operator()(const OutfitKey& key) const
    {
        return std::hash<RE::FormID>()(key.actorRefID) ^
               (std::hash<std::string>()(key.outfitName) << 1);
    }
};

class OutfitLockManager : public RE::BSTEventSink<RE::TESEquipEvent>,
                          public RE::BSTEventSink<RE::TESActorLocationChangeEvent>
{
public:
    static constexpr std::uint32_t kSerializationVersion = 4;  // v4: FormKey strings for armor items
    static constexpr std::uint32_t kOutfitRecord = '5OLF';  // 5 + OutfitLock outFit
    static constexpr std::uint32_t kPlayerItemsRecord = '5OPI';  // 5 + OutfitLock Player Items

    static OutfitLockManager* GetSingleton()
    {
        static OutfitLockManager instance;
        return &instance;
    }

    // Register for equip events
    void Initialize();

    // BSTEventSink<TESEquipEvent> interface
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESEquipEvent* a_event,
        RE::BSTEventSource<RE::TESEquipEvent>* a_eventSource) override;

    // BSTEventSink<TESActorLocationChangeEvent> interface
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESActorLocationChangeEvent* a_event,
        RE::BSTEventSource<RE::TESActorLocationChangeEvent>* a_eventSource) override;

    // === Outfit Management ===

    // Save current equipped armor as an outfit for the given actor
    bool SaveOutfit(RE::Actor* actor, const std::string& outfitName);

    // Apply a saved outfit to an actor
    // If unequipOthers is true, unequips armor in slots 30-45 first
    // Returns true on success, false if outfit not found or actor invalid
    bool ApplyOutfit(RE::Actor* actor, const std::string& outfitName, bool unequipOthers = false);

    // Delete a saved outfit
    bool DeleteOutfit(RE::Actor* actor, const std::string& outfitName);

    // Check if an actor has a saved outfit with the given name
    bool HasOutfit(RE::Actor* actor, const std::string& outfitName) const;

    // Lock an actor (creates "locked" outfit from current equipped armor)
    bool Lock(RE::Actor* actor);

    // Unlock an actor (deletes "locked" outfit)
    bool Unlock(RE::Actor* actor);

    // Check if an actor is locked
    bool IsLocked(RE::Actor* actor) const;

    // === Player Item Tracking ===

    // Mark an item as given from player to this actor
    void MarkItemAsPlayerGiven(RE::Actor* actor, RE::FormID itemID);

    // Check if actor has any items from player (in their inventory)
    bool HasPlayerItems(RE::Actor* actor) const;

    // Return all player-originated items back to player's inventory
    void ReturnPlayerItems(RE::Actor* actor);

    // === Gallery Item Tracking (Session-based, not persisted) ===

    // Mark an item as spawned from gallery for this actor (should be destroyed on unequip)
    void MarkItemAsGallerySpawned(RE::Actor* actor, RE::FormID itemID);

    // Check if an item was spawned from gallery
    bool IsGallerySpawnedItem(RE::Actor* actor, RE::FormID itemID) const;

    // Unmark a gallery item (called after destruction)
    void UnmarkGalleryItem(RE::Actor* actor, RE::FormID itemID);

    // === Default Outfit (Vanilla Restore) ===

    // Check if actor has a saved default (vanilla) outfit
    bool HasDefaultOutfit(RE::Actor* actor) const;

    // Restore actor to their default outfit and delete the saved default
    // Returns true if restored, false if no default exists
    bool RestoreAndClearDefault(RE::Actor* actor);

    // === Serialization ===

    // Called by SKSE on game save
    static void OnGameSave(SKSE::SerializationInterface* a_intfc);

    // Called by SKSE on game load
    static void OnGameLoad(SKSE::SerializationInterface* a_intfc);

    // Called by SKSE on game revert (new game or load)
    static void OnRevert(SKSE::SerializationInterface* a_intfc);

    // === Cell Change Handling ===

    // Called on post load game to scan for locked NPCs in current cell
    void OnPostLoadGame();

    // Apply locked outfits to NPCs in the given location (or current cell if null)
    void ApplyLockedOutfitsInLocation(RE::BGSLocation* location = nullptr);

private:
    OutfitLockManager() = default;
    ~OutfitLockManager() = default;
    OutfitLockManager(const OutfitLockManager&) = delete;
    OutfitLockManager& operator=(const OutfitLockManager&) = delete;

    // Get all currently equipped armor on an actor
    std::vector<RE::TESObjectARMO*> GetEquippedArmor(RE::Actor* actor) const;

    // Unequip armor in specified slots (30-45 for body armor)
    void UnequipArmorSlots(RE::Actor* actor);

    // Equip a list of armor items on an actor
    void EquipArmorList(RE::Actor* actor, const std::vector<RE::TESObjectARMO*>& items);

    // Get mod name for a form (for logging)
    std::string GetModName(RE::TESForm* form) const;

    bool m_initialized = false;
    bool m_cellEventRegistered = false;

    // Outfit storage: key = (actor ref ID, outfit name), value = outfit data
    std::unordered_map<OutfitKey, SavedOutfit, OutfitKeyHash> m_outfits;

    // Player item tracking: actor ref ID -> set of item FormIDs given from player
    std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> m_playerGivenItems;

    // Gallery item tracking: actor ref ID -> set of item FormIDs spawned from gallery
    // These items are destroyed when unequipped (session-based, not persisted)
    std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> m_gallerySpawnedItems;

    // Mutex for thread safety
    mutable std::mutex m_mutex;
};

// RAII helper for outfit modifications on potentially locked actors
// - Tracks whether actor was locked before changes
// - Re-saves the locked outfit on scope exit to capture changes
// Usage: ScopedLockSuspension suspension(actor);
//        ... make outfit changes ...
//        if (!suspension.WasLocked()) { AutoLockNpc(); }
class ScopedLockSuspension
{
public:
    explicit ScopedLockSuspension(RE::Actor* actor)
        : m_actor(actor)
        , m_wasLocked(actor ? OutfitLockManager::GetSingleton()->IsLocked(actor) : false)
    {
    }

    ~ScopedLockSuspension()
    {
        // Re-save the locked outfit to capture any changes made during this scope
        if (m_actor && m_wasLocked) {
            OutfitLockManager::GetSingleton()->SaveOutfit(m_actor, "locked");
        }
    }

    // Was the actor locked before we started?
    bool WasLocked() const { return m_wasLocked; }

    // Non-copyable
    ScopedLockSuspension(const ScopedLockSuspension&) = delete;
    ScopedLockSuspension& operator=(const ScopedLockSuspension&) = delete;

private:
    RE::Actor* m_actor;
    bool m_wasLocked;
};

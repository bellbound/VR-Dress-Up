#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>
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

    // FormKey of the actor this outfit belongs to, e.g. "0x13BBF~Skyrim.esm".
    //
    // Stored alongside OutfitKey::actorRefID from serialization v5 onwards. The
    // in-memory key stays the runtime ref ID (so nothing else has to change), but
    // the *persisted* identity is now load-order independent: a runtime ref ID is
    // only meaningful within the load order that produced it, so an outfit saved
    // under one load order used to attach to a different NPC - or to nobody - after
    // a plugin was added or removed.
    //
    // Empty for records loaded from v4 or earlier, and for dynamic actors.
    std::string actorFormKey;
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
    // v4: FormKey strings for armor items
    // v5: + actorFormKey per outfit, so the *actor* identity is load-order
    //     independent too. v4 records still load; they simply have no actorFormKey.
    static constexpr std::uint32_t kSerializationVersion = 5;
    static constexpr std::uint32_t kMinReadableVersion = 4;
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

    // Push the actor's "locked" outfit into a real Outfit form and assign it, which is
    // what makes SPID suspend distribution for them. Safe to call repeatedly: it
    // no-ops when neither the item set nor the assigned outfit has changed.
    void PromoteLockToOutfitForm(RE::Actor* actor);

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

    // Called by SKSE on game load (standalone — includes its own while loop)
    static void OnGameLoad(SKSE::SerializationInterface* a_intfc);

    // Clear state before loading records (called once by central dispatch)
    static void OnPreLoad();

    // Process a single serialization record (called by central dispatch loop)
    static void OnLoadRecord(SKSE::SerializationInterface* a_intfc,
        std::uint32_t type, std::uint32_t version, std::uint32_t length);

    // Called by SKSE on game revert (new game or load)
    static void OnRevert(SKSE::SerializationInterface* a_intfc);

    // === Cell Change Handling ===

    // Called on post load game to scan for locked NPCs in current cell
    void OnPostLoadGame();

    // Apply locked outfits to NPCs in the given location (or current cell if null)
    void ApplyLockedOutfitsInLocation(RE::BGSLocation* location = nullptr);

    // === Interface002 support: enumeration and FormKey injection ===
    //
    // These exist so an outfit set can be read out of one savegame and written into
    // another without either side handling runtime form IDs.

    // Names of every outfit stored for this actor, including "locked" and "default".
    std::vector<std::string> EnumerateOutfitNames(RE::Actor* actor) const;

    // FormKeys of the armour in one stored outfit, in stored order.
    std::vector<std::string> GetOutfitItemFormKeys(RE::Actor* actor,
                                                  const std::string& outfitName) const;

    // FormKeys of the items this actor was given by the player.
    std::vector<std::string> GetPlayerGivenFormKeys(RE::Actor* actor) const;

    // Replace (or create) an outfit from FormKeys. Map only - equips nothing and
    // does not touch inventory. Returns the number of keys accepted.
    uint32_t SetOutfitFromFormKeys(RE::Actor* actor, const std::string& outfitName,
                                   const std::vector<std::string>& formKeys);

    // Re-register player-given items from FormKeys. Returns the number accepted.
    uint32_t MarkPlayerGivenFromFormKeys(RE::Actor* actor,
                                         const std::vector<std::string>& formKeys);

    // === User-edit bracket ===
    //
    // Held for the duration of a menu-driven outfit change. The external-change watcher
    // ignores equip traffic while one is open, because the change is ours and the lock is
    // about to be re-saved to match it - see ScopedLockSuspension.
    void BeginUserEdit();
    void EndUserEdit();

    // Add any item of a stored outfit that the actor does not already have.
    // Returns the number added.
    //
    // MUST be called before applying a freshly injected outfit: ApplyOutfit prunes
    // items the actor does not have, and that pruning rewrites the stored map - so
    // applying against an empty inventory destroys the outfit rather than just
    // failing to dress the actor.
    uint32_t EnsureOutfitItemsInInventory(RE::Actor* actor, const std::string& outfitName);

private:
    OutfitLockManager() = default;
    ~OutfitLockManager() = default;
    OutfitLockManager(const OutfitLockManager&) = delete;
    OutfitLockManager& operator=(const OutfitLockManager&) = delete;

    // Get all currently equipped armor on an actor
    std::vector<RE::TESObjectARMO*> GetEquippedArmor(RE::Actor* actor) const;

    // Unequip every worn armour piece that is not in `keep`. Passing an empty `keep`
    // strips the actor, which is what the old UnequipArmorSlots did unconditionally.
    void UnequipArmorExcept(RE::Actor* actor, const std::vector<RE::TESObjectARMO*>& keep);

    // Equip a list of armor items on an actor
    void EquipArmorList(RE::Actor* actor, const std::vector<RE::TESObjectARMO*>& items);

    // Get mod name for a form (for logging)
    std::string GetModName(RE::TESForm* form) const;

    // === Re-entrancy ===
    //
    // Our own UnequipObject/EquipObject calls come straight back to us as TESEquipEvents.
    // OutfitFormBackend::IsApplying only covers the SetOutfit path (it is a time window
    // opened in DispatchSetOutfit), so ApplyOutfit's direct equip-manager calls used to
    // re-enter ProcessEvent - and a gallery-marked piece would be destroyed by the very
    // apply that was putting it back on.
    void BeginSelfDriven(RE::FormID actorID);
    void EndSelfDriven(RE::FormID actorID);
    bool IsSelfDriven(RE::FormID actorID) const;

    // RAII bracket for anything that drives equips on one actor.
    class SelfDrivenScope
    {
    public:
        SelfDrivenScope(OutfitLockManager* mgr, RE::FormID actorID)
            : m_mgr(mgr), m_actorID(actorID)
        {
            if (m_mgr && m_actorID) m_mgr->BeginSelfDriven(m_actorID);
        }
        ~SelfDrivenScope()
        {
            if (m_mgr && m_actorID) m_mgr->EndSelfDriven(m_actorID);
        }
        SelfDrivenScope(const SelfDrivenScope&) = delete;
        SelfDrivenScope& operator=(const SelfDrivenScope&) = delete;

    private:
        OutfitLockManager* m_mgr;
        RE::FormID m_actorID;
    };

    // === Equip-storm circuit breaker ===
    //
    // When another mod puts an NPC into an equip/unequip loop, every event fans out into
    // Papyrus (XPMSE restyle, Devious Devices slotmasks, OBody presets) and the suspended
    // stack count climbs until the VM tries to freeze itself to dump stacks - a 30s stall
    // per attempt. We cannot stop the loop, but we can refuse to feed it and we can name
    // the actor responsible in our own log instead of leaving it to be reconstructed from
    // six different logs afterwards.
    static constexpr std::int64_t kStormWindowMs = 1000;
    static constexpr std::uint32_t kStormThreshold = 40;   // events per actor per window
    static constexpr std::int64_t kStormBackoffMs = 30000;

    // Record one equip event for this actor. Returns true when the actor is in backoff.
    bool NoteEquipAndCheckStorm(RE::FormID actorID, const char* actorName);

    // Is this actor currently backed off? Cheap; used to skip reapplies.
    bool IsInEquipBackoff(RE::FormID actorID) const;

    // Coalesce location-change reapplies. Door transitions and fast travel fire these in
    // bursts, and a full reapply per burst is what turns 2 followers into 24 equip events.
    static constexpr std::int64_t kLocationDebounceMs = 1500;

    // === External-change enforcement (bReapplyOnExternalChange) ===
    //
    // A lock only survived a location change or a save before this: anything that undressed
    // a locked NPC in place - a scene, a bath mod, an outfit manager we did not detect -
    // stayed undressed until the player walked them through a door.
    //
    // Two limiters keep that from becoming a fight. The delay coalesces a burst (one outfit
    // swap is a dozen equip events) into a single reapply. The leaky bucket below then caps
    // the *sustained* rate: credit drains at one reapply per iReapplySustainedIntervalSec,
    // so a handful of reapplies close together passes as a fluke, while anything that keeps
    // undressing the NPC faster than we are willing to redress them fills the bucket and
    // trips a long stand-down. The alternative is trading equips with the other mod forever,
    // and every exchange runs XPMSE/DD/OBody Papyrus for both of us.

    // Something other than us changed a locked actor's armour. Schedules a reapply.
    void NoteExternalOutfitChange(RE::Actor* actor, RE::FormID baseObject);

    // Fires on the main thread once the coalescing delay is up.
    void RunPendingReapply(RE::FormID actorID);

    // Charge one reapply against this actor's budget. False means we are backing off.
    bool ClaimReapplySlot(RE::FormID actorID, const char* actorName);

    // Is what the actor is wearing still the locked set? Another mod re-equipping a piece
    // that is already part of the outfit is not a divergence, and must not cost either
    // equip traffic or a slot in the budget above - otherwise a mod that merely refreshes
    // gear would trip the breaker without ever having undressed anyone.
    bool DiffersFromLockedOutfit(RE::Actor* actor) const;

    struct ReapplyState
    {
        bool pending = false;              // a delayed reapply is already on its way
        std::int64_t lastReapplyMs = 0;    // for draining the bucket
        double burst = 0.0;                // bucket level, in reapplies
        std::int64_t backoffUntilMs = 0;
        bool reported = false;
    };
    std::unordered_map<RE::FormID, ReapplyState> m_reapply;
    mutable std::mutex m_reapplyMutex;

    // Number of stored "locked" outfits. Read on the equip-event path, which runs for every
    // equip in the loaded area, so the no-locked-NPC case costs an atomic load rather than
    // m_mutex on the game thread. Maintained by RefreshLockedCount.
    std::atomic<int> m_lockedCount{0};

    // Recount m_lockedCount from m_outfits. Caller must hold m_mutex.
    void RefreshLockedCount();

    // Menu edits in flight. Their equips are ours, and the lock is re-saved when they end.
    std::atomic<int> m_userEditCount{0};

    bool m_initialized = false;
    bool m_cellEventRegistered = false;

    // Outfit storage: key = (actor ref ID, outfit name), value = outfit data
    std::unordered_map<OutfitKey, SavedOutfit, OutfitKeyHash> m_outfits;

    // Player item tracking: actor ref ID -> set of item FormIDs given from player
    std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> m_playerGivenItems;

    // Gallery item tracking: actor ref ID -> set of item FormIDs spawned from gallery
    // These items are destroyed when unequipped (session-based, not persisted)
    std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> m_gallerySpawnedItems;

    // Lock-free "is there any gallery item at all" flag. TESEquipEvent lands on the game
    // thread for every equip in the loaded area - hundreds a second once some other mod
    // starts thrashing - and taking m_mutex on each one puts that contention on the frame.
    // Gallery items are rare and short-lived, so this is false almost always.
    std::atomic<bool> m_hasGalleryItems{false};

    // Mutex for thread safety
    mutable std::mutex m_mutex;

    // Actors whose equip events are our own doing. Separate lock: ApplyOutfit holds
    // m_mutex in places and the equip handler must never wait on it.
    std::unordered_set<RE::FormID> m_selfDriven;
    mutable std::mutex m_selfDrivenMutex;
    std::atomic<int> m_selfDrivenCount{0};

    struct EquipRate
    {
        std::int64_t windowStartMs = 0;
        std::uint32_t count = 0;
        std::int64_t backoffUntilMs = 0;
        bool reported = false;
    };
    std::unordered_map<RE::FormID, EquipRate> m_equipRate;
    mutable std::mutex m_equipRateMutex;

    // Last time we reapplied a locked outfit, per actor, for the debounce above.
    std::unordered_map<RE::FormID, std::int64_t> m_lastApplyMs;
    mutable std::mutex m_lastApplyMutex;
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
        OutfitLockManager::GetSingleton()->BeginUserEdit();
    }

    ~ScopedLockSuspension()
    {
        // Re-save the locked outfit to capture any changes made during this scope
        if (m_actor && m_wasLocked) {
            auto* mgr = OutfitLockManager::GetSingleton();
            mgr->SaveOutfit(m_actor, "locked");
            mgr->PromoteLockToOutfitForm(m_actor);
        }

        // Ends last: the re-save above is what makes the new look the locked one, and the
        // watcher must not see the scope's own equips before that has happened.
        OutfitLockManager::GetSingleton()->EndUserEdit();
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

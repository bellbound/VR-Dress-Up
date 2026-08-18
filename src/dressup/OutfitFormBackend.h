#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Assigns each locked NPC a real Outfit form and hands it to the engine through
// Papyrus, which is what makes SPID stand down.
//
// SPID's Outfit Manager treats its distributed outfit as authoritative and re-asserts
// it on Load3D, cell reset and save load. It stops only when another mod sets the
// actor's outfit - "Suspending outfit distribution for {} due to manual change of the
// outfit". That check lives on the Papyrus native, so the assignment has to go through
// the VM; see PapyrusBridge.
//
// Outfit records come from our own pool: VRDU_Outfit_000..127 in VRDressUp.esp. They
// ship empty; their item list is written here at runtime and rebuilt from the stored
// FormKeys after every load, because outfitItems lives only in memory.
class OutfitFormBackend
{
public:
    // v1: actor key, pool index, original outfit, last applied item set
    // v2: + whether the record came from another mod, so a load re-acquires it
    // v3: v2's flag dropped. Borrowing another mod's records is gone - the only ones
    //     that were ever available were SeverActions' dead OTFT scaffolding, and it
    //     hands its NPCs over through Native_SetOutfitExcluded instead.
    static constexpr std::uint32_t kSerializationVersion = 3;
    static constexpr std::uint32_t kMinReadableVersion = 1;
    static constexpr std::uint32_t kRecord = '5OSF';  // 5 + OutfitLock outfit Form

    // Pool layout in VRDressUp.esp. Records are contiguous from kFirstOutfitID.
    static constexpr const char* kPluginName = "VRDressUp.esp";
    static constexpr RE::FormID  kBlankOutfitID = 0x800;
    static constexpr RE::FormID  kFirstOutfitID = 0x801;
    static constexpr std::size_t kPoolSize = 128;

    // No free pool slot was available for this assignment.
    static constexpr std::size_t kNoPoolSlot = static_cast<std::size_t>(-1);

    static OutfitFormBackend* GetSingleton()
    {
        static OutfitFormBackend instance;
        return &instance;
    }

    // Resolve the outfit pool. Called at kDataLoaded.
    void Initialize();

    // True if a pool was resolved and the feature is switched on.
    bool IsAvailable() const;

    // True while a SetOutfit dispatch of ours is still settling. The equip-event sinks
    // use this to ignore the UnequipAll the engine queues behind an outfit change.
    //
    // A time window, not a flag: the dispatch is asynchronous and the engine's queued
    // unequips land 0.5-1.5s afterwards, long after any frame-scoped flag would have
    // been cleared. SeverActions hit the same thing and guards it the same way.
    bool IsApplying() const;

    // Actors we hand to SPID: not the player, has a base NPC, and - unless
    // bOutfitBackendUniqueOnly is off - a unique base. A non-unique base record is
    // shared by every copy of that NPC, so dressing one generic bandit would dress
    // all of them.
    bool IsEligible(RE::Actor* actor) const;

    // Give the actor an outfit record and assign it. `formKeys` is the armour set,
    // normally the freshly-snapshotted "locked" outfit. No-ops when the item set and
    // the actor's current outfit are both already what we last applied.
    bool Apply(RE::Actor* actor, const std::vector<std::string>& formKeys);

    // Put the actor's original outfit back and release its pool slot, so SPID can
    // resume distributing to them.
    bool Restore(RE::Actor* actor);

    // Re-assert after a load: outfit records come back empty, so refill from the
    // stored keys before checking whether anything needs dispatching.
    void ReapplyAll();

    // Drop everything we know about an actor without touching the game state.
    void Forget(RE::Actor* actor);

    // === Serialization ===
    static void OnGameSave(SKSE::SerializationInterface* a_intfc);
    static void OnPreLoad();
    static void OnLoadRecord(SKSE::SerializationInterface* a_intfc,
        std::uint32_t type, std::uint32_t version, std::uint32_t length);
    static void OnRevert(SKSE::SerializationInterface* a_intfc);

private:
    OutfitFormBackend() = default;
    ~OutfitFormBackend() = default;
    OutfitFormBackend(const OutfitFormBackend&) = delete;
    OutfitFormBackend& operator=(const OutfitFormBackend&) = delete;

    struct Assignment
    {
        std::size_t              poolIndex = 0;
        std::string              originalOutfitKey;  // empty if the NPC had no outfit
        std::vector<std::string> lastApplied;        // sorted; the change detector
    };

    // Resolve the pool out of VRDressUp.esp by local FormID. Deterministic, and
    // avoids EditorIDs, which the runtime does not keep for most record types.
    void ResolveNativePool();

    // Write the armour set into an outfit record.
    bool Fill(RE::BGSOutfit* outfit, const std::vector<std::string>& formKeys) const;

    // Take a pool slot for this actor, or return the one it already holds.
    // Caller must hold m_mutex.
    Assignment* AssignLocked(const std::string& actorKey);

    RE::BGSOutfit* OutfitForIndex(std::size_t index) const;

    // Dispatch through Papyrus with the re-entrancy flag raised.
    void DispatchSetOutfit(RE::Actor* actor, RE::BGSOutfit* outfit);

    std::vector<RE::BGSOutfit*> m_pool;
    RE::BGSOutfit*              m_blankOutfit = nullptr;

    // actor FormKey -> assignment. Keyed by FormKey rather than runtime ref ID so it
    // survives a load-order change, matching the outfit store's v5 format.
    std::unordered_map<std::string, Assignment> m_assignments;

    // How long after a dispatch the engine's queued equip events are still ours.
    static constexpr std::int64_t kApplySettleMs = 2000;

    bool                      m_initialized = false;
    std::atomic<std::int64_t> m_applyUntilMs{0};
    mutable std::mutex        m_mutex;
};

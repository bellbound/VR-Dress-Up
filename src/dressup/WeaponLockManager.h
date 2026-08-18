#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Keeps an NPC's hands empty.
//
// This is the simple half of a weapon lock. It has no opinion about *which* weapon an NPC
// carries - swapping a sword for an axe is their business - only about whether they carry
// one at all. It switches itself on when the player takes an NPC's last weapon off in the
// menu, and off again the moment the player hands one back or unlocks the NPC.
//
// Enforcement is deliberately unhurried, and that is the whole design. An NPC with a weapon
// drawn is in the middle of something - a fight, a scene, a guard's patrol idle - and
// pulling it out of their hands mid-animation looks broken and can leave the animation
// graph in a state the actor does not recover from on its own. So we wait for them to
// sheathe of their own accord and take it off then.
//
// The one thing we will not do is fight forever. Some mods and some AI packages re-equip a
// weapon the instant it comes off, and two systems flipping one flag at each other for the
// rest of a save is worse than the NPC simply holding a sword. See kStayOffMs.
class WeaponLockManager : public RE::BSTEventSink<RE::TESEquipEvent>,
                          public RE::BSTEventSink<RE::TESActorLocationChangeEvent>
{
public:
    static constexpr std::uint32_t kSerializationVersion = 1;
    static constexpr std::uint32_t kRecord = '5WPL';  // 5 + WeaPon Lock

    static WeaponLockManager* GetSingleton()
    {
        static WeaponLockManager instance;
        return &instance;
    }

    // Register for equip and location events. Safe to call repeatedly.
    void Initialize();

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESEquipEvent* a_event,
        RE::BSTEventSource<RE::TESEquipEvent>* a_eventSource) override;

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESActorLocationChangeEvent* a_event,
        RE::BSTEventSource<RE::TESActorLocationChangeEvent>* a_eventSource) override;

    // === State ===

    // Start keeping this actor's hands empty. Re-enforcing an actor we had given up on
    // starts the fight again from zero: the player asking a second time is a fresh
    // instruction, not a repeat of the one we walked away from.
    void Enforce(RE::Actor* actor);

    // Stop caring what this actor holds. Cancels any check already in flight.
    void StopEnforcing(RE::Actor* actor);

    bool IsEnforcing(RE::Actor* actor) const;

    // Called after the player changes an NPC's weapons through the menu.
    //
    // `nowHolding` is what the player's click asked for, not what the engine has applied
    // yet. Equips are queued and do not show up in the actor's worn items for another frame
    // or two (see ItemEquipHelper's "Queued equips"), so reading the actor here would find
    // empty hands and switch enforcement on over the weapon we just handed them.
    void NoteMenuWeaponChange(RE::Actor* actor, bool nowHolding);

    // Look at every enforced NPC again. Cheap, and the only way enforcement resumes for an
    // NPC whose watch was dropped because they never sheathed.
    void Sweep();

    // === Serialization ===

    static void OnGameSave(SKSE::SerializationInterface* a_intfc);
    static void OnPreLoad();
    static void OnLoadRecord(SKSE::SerializationInterface* a_intfc,
        std::uint32_t type, std::uint32_t version, std::uint32_t length);
    static void OnRevert(SKSE::SerializationInterface* a_intfc);

private:
    WeaponLockManager() = default;
    ~WeaponLockManager() = default;
    WeaponLockManager(const WeaponLockManager&) = delete;
    WeaponLockManager& operator=(const WeaponLockManager&) = delete;

    // How long to leave an NPC alone after they equip something before judging them. Long
    // enough that drawing a weapon and swinging it registers as "busy" rather than as a
    // sheathed actor we can quietly strip.
    static constexpr std::int32_t kSettleMs = 2000;

    // How often to look again while the weapon is out.
    static constexpr std::int32_t kDrawnRecheckMs = 3000;

    // Stop watching an NPC who has been holding a drawn weapon this many looks in a row
    // (~5 minutes). Nothing is given up: the next sweep picks them straight back up. This
    // only stops one permanently-in-combat actor from owning a timer for the rest of the
    // session.
    static constexpr std::uint32_t kMaxDrawnPolls = 100;

    // A weapon that reappears within this long of us removing one did not stay off.
    static constexpr std::int64_t kStayOffMs = 20000;

    // ...and more than this many of those in a row means something else owns this actor's
    // hands and is going to keep winning. Stop, and say so in the log.
    static constexpr std::uint32_t kMaxStrikes = 3;

    struct WatchState
    {
        // Load-order independent identity, so the co-save can name the actor. Empty for
        // dynamic actors, which cannot be persisted at all.
        std::string actorKey;

        // --- Runtime only ---
        //
        // None of this is worth persisting. A reload is a reasonable moment to give a
        // stubborn NPC another chance, and an in-flight timer is meaningless afterwards.

        // ID of the check currently scheduled for this actor, or 0 when none is. IDs come
        // from m_nextWatch and are never reused, so a timer that fires after its actor was
        // released - or after a save was loaded out from under it - can tell.
        std::uint64_t watch = 0;

        std::uint32_t drawnPolls = 0;    // consecutive looks that found the weapon out
        std::uint32_t strikes = 0;       // removals undone inside kStayOffMs
        std::int64_t lastRemovalMs = 0;  // when we last emptied their hands
        bool surrendered = false;        // stopped enforcing for this session
    };

    // An equip we care about landed on a watched actor.
    void NoteWeaponEquipped(RE::Actor* actor);

    // Post a check onto the game thread `delayMs` from now.
    static void ScheduleCheck(RE::FormID actorID, std::uint64_t watch, std::int32_t delayMs);

    // Runs on the game thread. Decides whether to wait longer or empty their hands.
    void RunCheck(RE::FormID actorID, std::uint64_t watch);

    // Clear the watch if it is still the one we were given.
    void EndWatch(RE::FormID actorID, std::uint64_t watch);

    // Recount the actors ProcessEvent has to care about. Caller holds m_mutex.
    void RecountLocked();

    mutable std::mutex m_mutex;
    std::unordered_map<RE::FormID, WatchState> m_watched;
    std::uint64_t m_nextWatch = 1;

    // Enforced, non-surrendered actors. Read without the lock on the game thread so the
    // usual case - nobody enforced - costs one atomic load per equip event in the cell.
    std::atomic<int> m_enforcedCount{0};

    bool m_initialized = false;
};

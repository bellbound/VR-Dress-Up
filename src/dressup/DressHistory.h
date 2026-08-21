#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "UndressManager.h"

// Undo and redo for the dress-up wheel.
//
// Every undoable press - a wheel toggle, a saved outfit, NPC Default, an undress step -
// records a snapshot of the actor it acted on, taken just before the press. Undo puts that
// snapshot back; redo puts back the snapshot taken at the moment of the undo. Nothing here
// knows how to invert an individual operation, and that is the point: the lock, the
// no-weapon lock, the undress state, the gallery marks and the player's hand-overs all move
// together, and an absolute restore stays right even after something this history never
// saw - another mod, the engine re-dressing them - has moved the actor in between.
//
// The "after" side of an entry is captured lazily, when it is undone, rather than when the
// press happened. Equips are queued, so the actor does not look right until a frame or two
// after the press; and whatever has drifted since is what a redo should honestly put back.
//
// The history lives for one menu session and is cleared when the wheel opens or closes. It
// is keyed to bookkeeping that has the same lifetime - which pieces were handed over from
// the player's pack, which gallery pieces are still being tried on - and is never written to
// the cosave. See MenuScrollMemory for the same shape of session-only store.

enum class DressActionKind : std::uint8_t
{
    WheelToggle,   // one item put on or taken off from the wheel; collapses per item
    Outfit,        // a saved outfit selected
    Default,       // NPC Default
    Undress,       // partial or full undress
    Redress
};

// Everything about one actor's dress that the menu can change. Keys are FormKeys so the
// comparison and the restore are independent of which vector they came out of; every list
// is kept sorted so equality is set equality.
struct DressSnapshot
{
    std::vector<std::string> wornKeys;        // armour on, queued equips included
    std::vector<std::string> galleryMarked;   // the subset still armed as gallery try-ons
    std::vector<RE::FormID> transferred;      // pieces handed over from the player's pack (NPC only)

    bool locked = false;                      // OutfitLockManager lock
    std::vector<std::string> lockedKeys;      // the "locked" outfit, when locked
    bool lockedNoWeapon = false;

    std::vector<RE::FormID> hands;            // weapons equipped (NPC only)
    bool weaponLock = false;                  // WeaponLockManager keeping hands empty

    UndressState undressState = UndressState::Dressed;
    std::vector<std::string> preundressKeys;  // what a redress would put back

    // The saved outfit the wheel's edits are being written into. Menu state rather than
    // actor state, filled in by the menu; restored so an undo lands back in the same
    // editing session the press was made in.
    std::optional<std::uint32_t> editingSlot;

    bool operator==(const DressSnapshot&) const = default;
};

struct DressHistoryEntry
{
    DressActionKind kind;
    RE::FormID actorID;
    RE::FormID formID;                     // the item, for WheelToggle; 0 otherwise
    DressSnapshot before;
    std::optional<DressSnapshot> after;    // filled when the entry is undone
};

class DressHistory
{
public:
    static DressHistory* GetSingleton()
    {
        static DressHistory instance;
        return &instance;
    }

    // Read the actor's dress as it stands. `editingSlot` is left empty; the menu fills it.
    static DressSnapshot Capture(RE::Actor* actor);

    // Put the actor back into `snap`. Main thread only. Returns false when the restore was
    // refused and nothing was touched - see the IsApplying guard in the implementation.
    static bool Restore(RE::Actor* actor, const DressSnapshot& snap);

    // A press is about to change `actor`; `before` is what they looked like. Drops the
    // redo stack. Consecutive toggles of the same item on the same actor share one entry
    // and keep the oldest `before`, so clicking a piece on, off and on again is one step
    // back - or none, if it ends where it started (see Undo).
    void Record(DressActionKind kind, RE::Actor* actor, RE::FormID formID, DressSnapshot before);

    bool CanUndo() const;
    bool CanRedo() const;

    // Step back. `editingSlot` is the menu's current one, needed to tell a press that
    // changed nothing from one that did. Returns the snapshot that was put back so the
    // menu can follow it, or nullopt when nothing was undone.
    std::optional<DressSnapshot> Undo(std::optional<std::uint32_t> editingSlot);
    std::optional<DressSnapshot> Redo();

    void Clear();

    static void OnPreLoad();
    static void OnRevert(SKSE::SerializationInterface*);

private:
    DressHistory() = default;
    ~DressHistory() = default;
    DressHistory(const DressHistory&) = delete;
    DressHistory& operator=(const DressHistory&) = delete;

    // Scratch name in OutfitLockManager's store that a restore goes through. Never
    // outlives the restore that wrote it.
    static constexpr const char* kScratchOutfit = "undo";

    std::vector<DressHistoryEntry> m_undo;
    std::vector<DressHistoryEntry> m_redo;
    mutable std::mutex m_mutex;
};

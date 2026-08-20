#pragma once

#include <RE/Skyrim.h>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// The saved looks of an NPC, and which of them they have on.
//
// A saved outfit is a snapshot: the armour the NPC wore and whether their hands were empty
// at the moment it was taken. It lives in OutfitLockManager's store under a "slot:<id>"
// name, next to "locked" and the other reserved entries, so it is persisted, migrated and
// enumerated the way every other stored outfit already is. This class adds nothing to the
// co-save.
//
// Putting an outfit on means making it the locked one (AdoptStoredOutfit), so from then
// on the lock defends it exactly as it defends any look the player puts together. The
// first entry in the row, "NPC Default", is not a snapshot: it is the unlocked state, the
// game dressing the NPC as it always did.
//
// Which outfit is *on* is never stored either. It is read off the lock: an unlocked NPC
// is in NPC Default; a locked NPC whose "locked" set equals a slot is wearing that slot;
// anything else is a look of the player's own that no outfit holds. That is what keeps a
// saved outfit safe from being edited by accident - the menu only writes into a slot while
// its row is open and that slot is the one on the NPC (SyncEdited), and a look the player
// made after closing the row matches nothing.
class OutfitSlotManager
{
public:
    static constexpr const char* kSlotPrefix = "slot:";

    struct Slot
    {
        std::uint32_t id;   // stable; never renumbered by a delete
        std::string name;   // "slot:<id>", the name in OutfitLockManager's store
    };

    static OutfitSlotManager* GetSingleton()
    {
        static OutfitSlotManager instance;
        return &instance;
    }

    static std::string NameOf(std::uint32_t id);

    // Every saved outfit of this actor, lowest id first. Empty for an NPC nobody has saved
    // a look for.
    std::vector<Slot> List(RE::Actor* actor) const;

    // The slot the actor is locked to right now, if the locked look equals one. Nullopt
    // for an unlocked actor (NPC Default) and for a look no slot holds.
    std::optional<std::uint32_t> Worn(RE::Actor* actor) const;

    // Unlocked: the game dresses them.
    bool IsDefault(RE::Actor* actor) const;

    // Hand the actor back to the game. Drops any undress state first - the look it would
    // re-dress into is gone.
    bool SelectDefault(RE::Actor* actor);

    // Put a saved outfit on and lock the actor to it. Drops any undress state first.
    bool Select(RE::Actor* actor, std::uint32_t id);

    // Snapshot what the actor has on - the locked set if they are locked, otherwise what
    // they are wearing - into a new slot, then Select it. Returns the new id.
    std::uint32_t SaveCurrent(RE::Actor* actor);

    // Delete a saved outfit. If it is the one on the actor, they are put into the outfit
    // before it in the row (else the one after), or back to NPC Default if it was the last.
    // The outfit's items stay in their inventory.
    bool Remove(RE::Actor* actor, std::uint32_t id);

    // Write the locked look into a slot. The menu calls this after each edit it makes
    // while the outfit row is open and `id` is the slot on the actor; nothing else does.
    void SyncEdited(RE::Actor* actor, std::uint32_t id);

    // A piece to draw on the slot's plate: the body piece if the outfit has one, else the
    // head, then the other armour slots, then whatever is left. Null for an empty outfit.
    RE::TESObjectARMO* Representative(RE::Actor* actor, std::uint32_t id) const;

    // The same, for a whole row at once, and the version the menu uses. Picking each plate
    // in isolation gave outfits that share a cuirass the same picture; this one keeps a
    // plate off a mesh an earlier plate already took whenever the outfit has an
    // alternative. Index-aligned with `slots`; a null entry is an empty outfit.
    std::vector<RE::TESObjectARMO*> Representatives(RE::Actor* actor, const std::vector<Slot>& slots) const;

private:
    OutfitSlotManager() = default;
    ~OutfitSlotManager() = default;
    OutfitSlotManager(const OutfitSlotManager&) = delete;
    OutfitSlotManager& operator=(const OutfitSlotManager&) = delete;

    // Every armour in a stored slot that still resolves.
    std::vector<RE::TESObjectARMO*> Pieces(RE::Actor* actor, std::uint32_t id) const;

    // The most telling piece of `pieces` whose mesh is not in `avoid` (lowercased model
    // paths). Null when every piece is spoken for.
    static RE::TESObjectARMO* PickPiece(const std::vector<RE::TESObjectARMO*>& pieces,
        const std::unordered_set<std::string>& avoid);

    // The weapon half of putting an outfit on: empty the actor's hands and keep them empty,
    // or stop caring what they hold.
    void ApplyHands(RE::Actor* actor, bool noWeapon) const;

    // The weapon half of a snapshot: are they meant to be empty-handed?
    static bool HandsEmpty(RE::Actor* actor);
};

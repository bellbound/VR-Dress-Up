#pragma once

#include <RE/Skyrim.h>
#include <string>

// SeverActions has its own outfit manager, and it is the more determined one: a quest
// alias per managed NPC re-applies on OnLoad / OnCellLoad / OnEnable and on a debounce
// after any unexpected unequip. Two mods re-dressing the same NPC on different triggers
// is the SPID problem again with different names, so when SeverActions is installed we
// hand the outfit to it and let it be the one that maintains it.
//
// What we hand over is a preset built from whatever the NPC is wearing at lock time -
// SeverActions snapshots the worn gear itself, so our own dressing logic is unchanged
// and it does the ownership tagging (catalog-supplied vs the player's own items) that
// makes preset switching non-destructive.
//
// We still assign our own outfit record on top, because that is the SPID handoff and
// SeverActions deliberately never calls SetOutfit on its apply path.
namespace SeverActionsCompat
{
    // The preset name we own on the SeverActions side.
    inline constexpr const char* kPresetName = "VRDressUp";

    // Detect SeverActions. Call at kDataLoaded, before OutfitFormBackend::Initialize.
    void Initialize();

    bool IsActive();

    // Snapshot the actor's current look into our SeverActions preset.
    void HandOffOutfit(RE::Actor* actor);

    // Drop the preset we created.
    void ReleaseOutfit(RE::Actor* actor);

    // Bracket anything that will fire equip events, so the SeverActions alias debounce
    // does not treat our own work as an external unequip and re-apply on top of it.
    // These are the same natives SeverActions wraps its own apply path in.
    void SuspendLock(RE::Actor* actor);
    void ResumeLock(RE::Actor* actor);
}

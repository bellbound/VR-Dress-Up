#pragma once

#include <RE/Skyrim.h>
#include <functional>
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
// The outfit record we then assign is SeverActions' own. It scaffolds 800 OTFT records
// (100 slots x 8 presets) and, since 3.9.2, never applies them - its apply path is
// deliberately SetOutfit-free. So the record for our preset is sitting there unused: we
// fill it and assign it, which is the SPID handoff, without touching anything
// SeverActions relies on. The record is obtained from SeverActions itself rather than
// looked up, so we are not guessing at its FormID layout or at EditorIDs the runtime
// does not keep.
namespace SeverActionsCompat
{
    // The preset name we own on the SeverActions side.
    inline constexpr const char* kPresetName = "VRDressUp";

    // The SeverActions main quest, which carries SeverActions_OutfitSlot.
    inline constexpr RE::FormID kQuestID = 0x000D62;
    inline constexpr const char* kPluginName = "SeverActions.esp";

    // Detect SeverActions. Call at kDataLoaded, before OutfitFormBackend::Initialize.
    void Initialize();

    bool IsActive();

    // Snapshot the actor's current look into our SeverActions preset.
    void HandOffOutfit(RE::Actor* actor);

    // Resolve the OTFT record backing our preset for this actor, so we can fill and
    // assign it: GetSlot -> FindPresetIndexByName -> GetOutfitForm, each a call into
    // SeverActions. Retries, because the preset is built by Papyrus on its own
    // schedule after HandOffOutfit. Calls back with nullptr once it gives up.
    void AcquirePresetOutfit(RE::Actor* actor, std::function<void(RE::BGSOutfit*)> done);

    // Drop the preset we created.
    void ReleaseOutfit(RE::Actor* actor);

    // Bracket anything that will fire equip events, so the SeverActions alias debounce
    // does not treat our own work as an external unequip and re-apply on top of it.
    // These are the same natives SeverActions wraps its own apply path in.
    void SuspendLock(RE::Actor* actor);
    void ResumeLock(RE::Actor* actor);
}

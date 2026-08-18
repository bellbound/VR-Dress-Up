#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <cstdint>
#include <string>

// SeverActions has its own outfit manager, and it is the more determined one: a quest
// alias per managed NPC re-applies on OnLoad / OnCellLoad / OnEnable and on a debounce
// after any unexpected unequip. Two mods re-dressing the same NPC on different triggers
// is the SPID problem again with different names.
//
// It does not go through outfits at all. SeverActions_OutfitSlot::ApplyPresetBySlot
// deliberately skips SetOutfit and calls Native_OutfitSlot_DirectEquipPreset instead,
// which strips the worn armour and equips fresh copies out of a per-preset wardrobe
// container. Its 800 OTFT records are dead scaffolding, so there is nothing on that side
// to hand an outfit record to - and its enforcement will not stand down just because we
// assigned one.
//
// What it does publish is a switch for exactly this situation:
// Native_SetOutfitExcluded takes the actor out of the outfit system entirely - "no outfit
// lock, no DefaultOutfit suppression, no situation auto-switch, no alias re-equip. Allows
// other outfit mods to manage them freely" - and its alias checks the flag at all four
// enforcement entry points. We set it while we hold the NPC's lock and clear it on
// unlock, so the NPC goes back to SeverActions exactly as they came.
namespace SeverActionsCompat
{
    inline constexpr const char* kPluginName = "SeverActions.esp";

    // Which actors we excluded. Persisted so unlock only ever hands back the ones we
    // took: an exclusion the player set themselves in the SeverActions MCM is not ours.
    inline constexpr std::uint32_t kRecord = '5SAX';  // 5 + SeverActions eXcluded
    inline constexpr std::uint32_t kSerializationVersion = 1;

    // How long the engine keeps handing Papyrus the equip events behind one of our own
    // outfit changes. Everything we do is suspended for at least this long.
    inline constexpr std::int32_t kEquipSettleMs = 2000;

    // Detect SeverActions. Call at kDataLoaded, before OutfitFormBackend::Initialize.
    void Initialize();

    // Take the actor out of the SeverActions outfit system for as long as we hold their
    // lock. No-op when the player already excluded them.
    void TakeOver(RE::Actor* actor);

    // Hand the actor back, if we were the ones who took them.
    void Release(RE::Actor* actor);

    // Bracket anything that will fire equip events, so the SeverActions alias debounce
    // does not treat our own work as an external unequip and re-dress on top of it.
    // These are the same natives SeverActions wraps its own apply path in.
    //
    // Belt and braces next to the exclusion above: it covers the actors we could not
    // exclude (no FormKey to persist against) and the window before the exclusion, set
    // through the VM, has actually landed.
    void SuspendLock(RE::Actor* actor);
    void ResumeLock(RE::Actor* actor);

    // Resume once the engine has finished with the equip events our own work queued.
    // Papyrus sees those 0.5-1.5s late, so resuming on return would hand the alias its
    // debounce trigger back just in time to catch them. The suspend self-clears after
    // 300s on the SeverActions side, so a resume that never arrives is not fatal.
    void ResumeLockAfterMs(RE::Actor* actor, std::int32_t delayMs);

    // === Serialization ===
    void OnGameSave(SKSE::SerializationInterface* a_intfc);
    void OnPreLoad();
    void OnLoadRecord(SKSE::SerializationInterface* a_intfc,
        std::uint32_t type, std::uint32_t version, std::uint32_t length);
    void OnRevert(SKSE::SerializationInterface* a_intfc);
}

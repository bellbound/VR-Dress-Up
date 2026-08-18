#pragma once

#include <RE/Skyrim.h>

// Devious Devices compatibility.
//
// A DD item is two armour records, not one, and they are not interchangeable:
//
//   * the *inventory device* - carries `zad_InventoryDevice`, has a real name, and
//     BOD2 = 0, i.e. no biped slot at all. This is the record the player sees in the
//     inventory, the record DD's zadEquipScript is attached to, and the only half
//     anything outside DD is allowed to equip or remove.
//   * the *rendered device* - carries a `zad_Devious*` type keyword, has no name, and
//     owns the biped slot the gear actually occupies. DD equips it itself, with
//     preventRemoval set, in reaction to the inventory device being equipped.
//
// Verified against the shipped records: of 1997 armours across DD Assets/Integration/
// Expansion/Contraptions, all 931 inventory devices have BOD2 = 0 and a name, and 985
// of 987 rendered devices have a slot and no name.
//
// Two consequences drive everything in here:
//
//   * `Actor::GetWornArmor(slotMask)` can never find an inventory device, because it
//     matches on slot bits and an inventory device has none. Any "is this worn" test
//     routed through the slot mask answers no forever, so we re-equip a device that is
//     already on, on every single outfit reapply - which is DD's OnEquipped running its
//     whole lock sequence again on every cell change. See ItemEquipHelper::IsArmorEquipped,
//     which now reads the inventory entry's worn flag instead.
//   * equipping or removing either half by hand desynchronises the pair, and DD notices
//     and repairs it. That repair is the equip/unequip tug-of-war in the logs.
//
// So: rendered devices are invisible to us - never snapshotted, never equipped, never
// stripped - and inventory devices are driven only through zadlibs.LockDevice /
// zadlibs.UnlockDevice, which fire the events and run the bookkeeping DD expects.
//
// Everything degrades to "no DD installed" if the keywords do not resolve, and every
// entry point is a cheap no-op in that case.
namespace DeviceCompat
{
    // Resolve DD's keywords. Safe to call when DD is absent. Called at kDataLoaded.
    void Initialize();

    // True once Initialize found DD's keywords.
    bool IsInstalled();

    // The half with the script, the name and no biped slot.
    bool IsInventoryDevice(RE::TESObjectARMO* armor);

    // The half with the slot and the mesh. Never ours to touch.
    bool IsRenderedDevice(RE::TESObjectARMO* armor);

    // Either half.
    bool IsDevice(RE::TESObjectARMO* armor);

    // DD refuses to remove quest devices, and an attempt just makes it put the device
    // straight back on. We leave them worn and leave them in the outfit instead.
    bool IsQuestDevice(RE::TESObjectARMO* armor);

    // Ask DD to lock this device onto the actor. `force` lets DD swap out a conflicting
    // device of the same type rather than silently refusing, which is what made equipping
    // a second belt or gag look like it randomly did nothing.
    //
    // Papyrus dispatch is asynchronous; this returns whether the call was dispatched.
    bool Equip(RE::Actor* actor, RE::TESObjectARMO* inventoryDevice, bool force = true);

    // Ask DD to unlock and remove this device. Keys are not required - Dress Up is an
    // out-of-band wardrobe tool, not the player picking at a lock - but quest devices
    // are still refused, by DD and by IsQuestDevice above.
    bool Unequip(RE::Actor* actor, RE::TESObjectARMO* inventoryDevice);
}

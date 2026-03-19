#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include "OutfitLockManager.h"

namespace EquipHooks
{
    // Check if an equip should be blocked on a locked actor.
    // Returns true to PREVENT the equip from happening.
    inline bool ShouldPreventEquip(RE::Actor* a_actor, RE::TESBoundObject* a_object)
    {
        if (!a_actor || a_actor->IsPlayerRef()) return false;

        auto* mgr = OutfitLockManager::GetSingleton();
        if (!mgr->IsLocked(a_actor)) return false;
        if (!a_object) return false;

        if (a_object->Is(RE::FormType::Armor)) {
            return !mgr->IsInLockedOutfit(a_actor, a_object);
        }

        if (a_object->Is(RE::FormType::Weapon)) {
            if (mgr->IsWeaponUnlocked(a_actor)) return false;
            return !mgr->IsInLockedOutfit(a_actor, a_object);
        }

        return false;  // potions, scrolls, food, etc. — always allow
    }

    // Check if an unequip should be blocked on a locked actor.
    // Returns true to PREVENT the unequip from happening.
    inline bool ShouldPreventUnequip(RE::Actor* a_actor, RE::TESBoundObject* a_object)
    {
        if (!a_actor || a_actor->IsPlayerRef()) return false;

        auto* mgr = OutfitLockManager::GetSingleton();
        if (!mgr->IsLocked(a_actor)) return false;
        if (!a_object) return false;

        if (a_object->Is(RE::FormType::Armor)) {
            return mgr->IsInLockedOutfit(a_actor, a_object);
        }

        if (a_object->Is(RE::FormType::Weapon)) {
            if (mgr->IsWeaponUnlocked(a_actor)) return false;
            return mgr->IsInLockedOutfit(a_actor, a_object);
        }

        return false;
    }

    // Hook the internal dispatch CALL inside EquipObject.
    // Uses simplified 4-param signature — the 4th arg is an opaque pointer to a
    // stack struct containing the remaining params (extraData, count, slot, bools).
    struct EquipObjectHook
    {
        static void thunk(RE::ActorEquipManager* a_this, RE::Actor* a_actor,
            RE::TESBoundObject* a_object, std::uint64_t a_unk)
        {
            if (ShouldPreventEquip(a_actor, a_object)) {
                spdlog::trace("EquipHooks: Blocked equip of '{}' on locked actor '{}'",
                    a_object ? a_object->GetName() : "null",
                    a_actor ? a_actor->GetName() : "null");
                return;
            }
            func(a_this, a_actor, a_object, a_unk);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // Hook the internal dispatch CALL inside UnequipObject.
    // Blocking returns without calling dispatch — the outer function's return value
    // (read from the param struct) stays at its default 0/false = "didn't unequip".
    struct UnequipObjectHook
    {
        static void thunk(RE::ActorEquipManager* a_this, RE::Actor* a_actor,
            RE::TESBoundObject* a_object, std::uint64_t a_unk)
        {
            if (ShouldPreventUnequip(a_actor, a_object)) {
                spdlog::trace("EquipHooks: Blocked unequip of '{}' on locked actor '{}'",
                    a_object ? a_object->GetName() : "null",
                    a_actor ? a_actor->GetName() : "null");
                return;
            }
            func(a_this, a_actor, a_object, a_unk);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    inline void Install()
    {
        // 2 write_call hooks at 14 bytes each
        SKSE::AllocTrampoline(14 * 2);
        auto& trampoline = SKSE::GetTrampoline();

        // Hook internal dispatch CALL inside EquipObject
        // SE: +0xE5, AE: +0x170, VR: +0xE5 (REL::Relocate uses SE value for VR)
        EquipObjectHook::func = trampoline.write_call<5>(
            REL::RelocationID(37938, 38894).address() + REL::Relocate(0xE5, 0x170),
            EquipObjectHook::thunk);
        spdlog::info("EquipHooks: Installed EquipObject dispatch hook");

        // Hook internal dispatch CALL inside UnequipObject
        // SE: +0x138, AE: +0x1B9, VR: +0x138 (REL::Relocate uses SE value for VR)
        UnequipObjectHook::func = trampoline.write_call<5>(
            REL::RelocationID(37945, 38901).address() + REL::Relocate(0x138, 0x1B9),
            UnequipObjectHook::thunk);
        spdlog::info("EquipHooks: Installed UnequipObject dispatch hook");
    }
}

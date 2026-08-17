#pragma once

#include <RE/Skyrim.h>
#include <vector>
#include <string>
// Spelled relative to this file, not to whoever includes it: a .cpp in this folder has no
// src/ in its include chain, so a bare "log.h" only resolves for includers up in src/.
#include "../log.h"

// Helper namespace for unified armor/weapon equipment operations
namespace ItemEquipHelper
{
    // Get inventory items of a specific type from an actor
    template<typename T>
    std::vector<T*> GetInventoryItems(RE::Actor* actor)
    {
        std::vector<T*> result;
        if (!actor) return result;

        RE::FormType formType;
        if constexpr (std::is_same_v<T, RE::TESObjectARMO>) {
            formType = RE::FormType::Armor;
        } else if constexpr (std::is_same_v<T, RE::TESObjectWEAP>) {
            formType = RE::FormType::Weapon;
        } else {
            static_assert(std::is_same_v<T, RE::TESObjectARMO> || std::is_same_v<T, RE::TESObjectWEAP>,
                "ItemEquipHelper only supports Armor and Weapon types");
        }

        auto inventory = actor->GetInventory([formType](RE::TESBoundObject& obj) {
            return obj.Is(formType);
        });

        for (const auto& [item, data] : inventory) {
            if (data.first <= 0) continue;  // Skip items with zero count

            auto* typedItem = item->As<T>();
            if (typedItem) {
                result.push_back(typedItem);
            }
        }

        return result;
    }

    // Check if actor has item in inventory
    inline bool HasItemInInventory(RE::Actor* actor, RE::TESBoundObject* item)
    {
        if (!actor || !item) return false;

        auto inventory = actor->GetInventory([item](RE::TESBoundObject& obj) {
            return obj.GetFormID() == item->GetFormID();
        });

        return !inventory.empty();
    }

    // Get model path for an item. Female-only gear - wigs, lingerie - ships with an empty
    // male model, and an empty path draws nothing at all in the wheel, so fall back.
    inline std::string GetModelPath(RE::TESObjectARMO* armor)
    {
        if (!armor) return "";

        const char* modelPath = armor->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
        if (!modelPath || !*modelPath) {
            modelPath = armor->worldModels[RE::TESBipedModelForm::Sexes::kFemale].GetModel();
        }
        return modelPath ? modelPath : "";
    }

    inline std::string GetModelPath(RE::TESObjectWEAP* weapon)
    {
        if (!weapon) return "";
        const char* modelPath = weapon->GetModel();
        return modelPath ? modelPath : "";
    }

    // Biped slot 52 is where The New Gentleman - and SOS before it - puts its genital
    // addons. Those are body parts rather than clothing: TNG equips and unequips them
    // itself in reaction to what the NPC is wearing, so anything we do with them is
    // fighting the mod that owns them. Worse, they turn up worn a frame or two after the
    // outfit that triggered them, so a snapshot can catch one by accident and then keep
    // putting it back on long after the player removed it in the TNG menu.
    //
    // Verified against the record: TNG_GenitalCover (0xAFF~TheNewGentleman.esp) has
    // BOD2 = 0x00400000, slot 52 and nothing else.
    inline bool IsBodyPart(RE::TESObjectARMO* armor)
    {
        constexpr auto kBodyPartSlots = static_cast<std::uint32_t>(1u << (52 - 30));

        if (!armor) return false;
        return (static_cast<std::uint32_t>(armor->GetSlotMask()) & kBodyPartSlots) != 0;
    }

    // Check if armor is equipped in its slot
    inline bool IsArmorEquipped(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;
        auto slotMask = armor->GetSlotMask();
        auto* wornArmor = actor->GetWornArmor(slotMask);
        return wornArmor && wornArmor->GetFormID() == armor->GetFormID();
    }

    // Check if weapon is equipped (in either hand)
    inline bool IsWeaponEquipped(RE::Actor* actor, RE::TESObjectWEAP* weapon)
    {
        if (!actor || !weapon) return false;
        RE::TESForm* rightHand = actor->GetEquippedObject(false);
        RE::TESForm* leftHand = actor->GetEquippedObject(true);

        bool inRight = rightHand && rightHand->GetFormID() == weapon->GetFormID();
        bool inLeft = leftHand && leftHand->GetFormID() == weapon->GetFormID();

        return inRight || inLeft;
    }

    // Get what's currently worn in an armor's slot
    inline RE::TESObjectARMO* GetWornInSlot(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return nullptr;
        return actor->GetWornArmor(armor->GetSlotMask());
    }

    // Get currently equipped weapon in right hand
    inline RE::TESObjectWEAP* GetEquippedWeaponRight(RE::Actor* actor)
    {
        if (!actor) return nullptr;
        RE::TESForm* rightHand = actor->GetEquippedObject(false);
        return rightHand ? rightHand->As<RE::TESObjectWEAP>() : nullptr;
    }

    // Get currently equipped weapon in left hand
    inline RE::TESObjectWEAP* GetEquippedWeaponLeft(RE::Actor* actor)
    {
        if (!actor) return nullptr;
        RE::TESForm* leftHand = actor->GetEquippedObject(true);
        return leftHand ? leftHand->As<RE::TESObjectWEAP>() : nullptr;
    }

    // Get both equipped weapons (right first, then left if different)
    inline std::vector<RE::TESObjectWEAP*> GetEquippedWeapons(RE::Actor* actor)
    {
        std::vector<RE::TESObjectWEAP*> result;
        if (!actor) return result;

        auto* rightWeapon = GetEquippedWeaponRight(actor);
        auto* leftWeapon = GetEquippedWeaponLeft(actor);

        if (rightWeapon) {
            result.push_back(rightWeapon);
        }
        if (leftWeapon && (!rightWeapon || leftWeapon->GetFormID() != rightWeapon->GetFormID())) {
            result.push_back(leftWeapon);
        }

        return result;
    }

    // Transfer item from one actor to another
    inline void TransferItem(RE::Actor* from, RE::Actor* to, RE::TESBoundObject* item, int count = 1)
    {
        if (!from || !to || !item) return;
        from->RemoveItem(item, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, to);
    }

    // Equip item on actor
    inline void EquipItem(RE::Actor* actor, RE::TESBoundObject* item)
    {
        if (!actor || !item) return;
        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (equipManager) {
            equipManager->EquipObject(actor, item, nullptr, 1, nullptr, true, false, false);
        }
    }

    // Unequip item from actor
    inline void UnequipItem(RE::Actor* actor, RE::TESBoundObject* item)
    {
        if (!actor || !item) return;
        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (equipManager) {
            equipManager->UnequipObject(actor, item, nullptr, 1, nullptr, false, true);
        }
    }

    // Toggle equip state for armor on actor
    inline bool ToggleArmorEquip(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;

        bool wasEquipped = IsArmorEquipped(actor, armor);

        if (wasEquipped) {
            UnequipItem(actor, armor);
            spdlog::info("ItemEquipHelper::ToggleArmorEquip - Unequipped '{}' from {}",
                armor->GetFullName(), actor->GetName());
        } else {
            EquipItem(actor, armor);
            spdlog::info("ItemEquipHelper::ToggleArmorEquip - Equipped '{}' on {}",
                armor->GetFullName(), actor->GetName());
        }

        return !wasEquipped;  // Return new equipped state
    }

    // Toggle equip state for weapon on actor
    inline bool ToggleWeaponEquip(RE::Actor* actor, RE::TESObjectWEAP* weapon)
    {
        if (!actor || !weapon) return false;

        bool wasEquipped = IsWeaponEquipped(actor, weapon);

        if (wasEquipped) {
            UnequipItem(actor, weapon);
            spdlog::info("ItemEquipHelper::ToggleWeaponEquip - Unequipped '{}' from {}",
                weapon->GetFullName(), actor->GetName());
        } else {
            EquipItem(actor, weapon);
            spdlog::info("ItemEquipHelper::ToggleWeaponEquip - Equipped '{}' on {}",
                weapon->GetFullName(), actor->GetName());
        }

        return !wasEquipped;  // Return new equipped state
    }
}

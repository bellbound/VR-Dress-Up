#pragma once

#include <RE/Skyrim.h>
#include "log.h"

namespace NpcUtils
{
    // Check if the grabbed object is an NPC (Actor)
    inline bool IsGrabbingNpc(RE::TESObjectREFR* grabbedObj)
    {
        if (!grabbedObj) {
            return false;
        }

        auto* actor = grabbedObj->As<RE::Actor>();
        if (!actor) {
            return false;
        }

        spdlog::info("NpcUtils: Grabbed object {} IS an Actor (NPC: {})",
            grabbedObj->GetFormID(),
            actor->GetName());
        return true;
    }

    // Get the Actor from a reference, or nullptr if not an actor
    inline RE::Actor* GetActor(RE::TESObjectREFR* ref)
    {
        return ref ? ref->As<RE::Actor>() : nullptr;
    }

    // Get the actor's name safely
    inline const char* GetActorName(RE::TESObjectREFR* ref)
    {
        auto* actor = GetActor(ref);
        return actor ? actor->GetName() : "unknown";
    }
}

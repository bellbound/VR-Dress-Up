#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <cstdint>
#include <functional>
#include <string>

// Papyrus VM entry points.
//
// Outfit assignment must be dispatched through the VM rather than done with
// RE::Actor::SetDefaultOutfit. SPID's "another mod took this NPC over, stop
// redistributing to it" hook sits on the Papyrus native - Outfits::SetOutfitActor,
// from SPID's OutfitManager+Papyrus.cpp - so an engine-side outfit change is
// invisible to it and the handoff never fires.
namespace PapyrusBridge
{
    // Papyrus dispatch is asynchronous: anything that needs a return value gets it
    // here, on a later frame. `success` is false if the call never produced an Int.
    using IntResult = std::function<void(bool success, std::int32_t value)>;

    // Actor.SetOutfit(akOutfit, abSleepOutfit)
    bool CallActorSetOutfit(RE::Actor* actor, RE::BGSOutfit* outfit, bool sleepOutfit);

    // Global Native call, fire and forget. Takes ownership of `args`.
    bool CallGlobal(const char* className, const char* fnName,
                    RE::BSScript::IFunctionArguments* args);

    // Global Native call returning an Int. Takes ownership of `args`.
    bool CallGlobalInt(const char* className, const char* fnName,
                       RE::BSScript::IFunctionArguments* args, IntResult callback);

    // Method call on a script attached to `target`, returning an Int.
    // Takes ownership of `args`.
    bool CallMethodInt(RE::TESForm* target, RE::FormType targetType,
                       const char* scriptName, const char* fnName,
                       RE::BSScript::IFunctionArguments* args, IntResult callback);

    // SendModEvent equivalent. `sender` is the form Papyrus receives as akSender.
    void SendModEvent(const std::string& eventName, const std::string& strArg,
                      float numArg, RE::TESForm* sender);
}

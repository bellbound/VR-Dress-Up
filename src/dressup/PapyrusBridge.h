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

    // Null when the call returned None or the handle no longer resolves.
    using FormResult = std::function<void(RE::TESForm* form)>;

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

    // Global Native call returning a form of `formType`. Takes ownership of `args`.
    bool CallGlobalForm(const char* className, const char* fnName, RE::FormType formType,
                        RE::BSScript::IFunctionArguments* args, FormResult callback);

    // SendModEvent equivalent. `sender` is the form Papyrus receives as akSender.
    void SendModEvent(const std::string& eventName, const std::string& strArg,
                      float numArg, RE::TESForm* sender);

    // Run `fn` on the main thread after a delay. Papyrus work started by a ModEvent
    // completes on its own schedule, so anything reading the result back has to wait
    // and retry rather than assume it landed.
    void RunAfterMs(std::int32_t delayMs, std::function<void()> fn);
}

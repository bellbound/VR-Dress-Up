#include "OutfitLockManager.h"
#include "OutfitFormBackend.h"
#include "ItemEquipHelper.h"
#include "DeviceCompat.h"
#include "PapyrusBridge.h"
#include "SeverActionsCompat.h"
#include "WeaponLockManager.h"
#include "../settings.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <spdlog/spdlog.h>

namespace
{
    std::int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void OutfitLockManager::Initialize()
{
    if (m_initialized) {
        spdlog::warn("OutfitLockManager::Initialize - Already initialized");
        return;
    }

    auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
    if (sourceHolder) {
        sourceHolder->AddEventSink<RE::TESEquipEvent>(this);
        spdlog::info("OutfitLockManager::Initialize - Registered for TESEquipEvent");
        m_initialized = true;
    } else {
        spdlog::error("OutfitLockManager::Initialize - Failed to get ScriptEventSourceHolder");
    }

    // Register for location change events (fires when player changes location)
    if (!m_cellEventRegistered && sourceHolder) {
        sourceHolder->AddEventSink<RE::TESActorLocationChangeEvent>(this);
        m_cellEventRegistered = true;
        spdlog::info("OutfitLockManager::Initialize - Registered for TESActorLocationChangeEvent");
    }
}

RE::BSEventNotifyControl OutfitLockManager::ProcessEvent(
    const RE::TESEquipEvent* a_event,
    RE::BSTEventSource<RE::TESEquipEvent>*)
{
    if (!a_event) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // This runs on the game thread for every equip in the loaded area. When another mod
    // puts an NPC into an equip loop that is hundreds of events a second, so everything
    // below is ordered cheapest-test-first and nothing allocates until we know the event
    // is one we actually act on.

    // Assigning an outfit makes the engine queue an UnequipAll, so the events that
    // arrive just after are our own doing, not the player's.
    if (OutfitFormBackend::GetSingleton()->IsApplying()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Get actor reference
    auto* actor = a_event->actor ? a_event->actor->As<RE::Actor>() : nullptr;
    if (!actor) {
        return RE::BSEventNotifyControl::kContinue;
    }

    const RE::FormID actorID = actor->GetFormID();

    // Our own ApplyOutfit equips land here too - see BeginSelfDriven.
    if (IsSelfDriven(actorID)) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Count the event even for the player, so the breaker sees the true rate.
    const bool inStorm = NoteEquipAndCheckStorm(actorID, actor->GetName());

    // Someone else changed a locked NPC's gear: put it back. Ordered so that the usual
    // case - no locked NPC in the save, or none of this is enabled - costs two atomic
    // loads. Nothing to do while a storm is already in progress either; feeding it is the
    // one thing the breaker exists to prevent.
    if (!inStorm && m_lockedCount.load(std::memory_order_relaxed) > 0 &&
        m_userEditCount.load(std::memory_order_relaxed) == 0 &&
        !actor->IsPlayerRef() &&
        Settings::GetSingleton()->IsReapplyOnExternalChangeEnabled()) {
        NoteExternalOutfitChange(actor, a_event->baseObject);
    }

    // Everything past here is gallery-item cleanup, which only ever applies to a
    // non-player actor unequipping something.
    if (a_event->equipped || actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Lock-free reject for the overwhelmingly common case of no gallery items anywhere.
    if (!m_hasGalleryItems.load(std::memory_order_relaxed)) {
        return RE::BSEventNotifyControl::kContinue;
    }

    if (!IsGallerySpawnedItem(actor, a_event->baseObject)) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Relevant event: now it is worth resolving names.
    const char* actorName = actor->GetName();

    std::string baseObjectName = "unknown";
    if (auto* form = RE::TESForm::LookupByID(a_event->baseObject)) {
        if (auto* name = form->GetName(); name && name[0]) {
            baseObjectName = name;
        } else {
            baseObjectName = fmt::format("FormID:{:08X}", a_event->baseObject);
        }
    }

    // Destroying is for the player taking a piece off in the menu. When something else
    // undresses a locked NPC - a scene, a bath mod, an outfit manager - the lock is about
    // to put that outfit straight back on, and deleting the items out from under it means
    // the reapply has to re-create them. That only works for pieces the locked outfit
    // knows about; anything else was gone for good, and the NPC came back dressed in the
    // stored set rather than in what she had on. So leave it in the inventory and leave
    // it marked: the next unequip the player actually asks for still cleans it up.
    if (m_userEditCount.load(std::memory_order_relaxed) == 0 && IsLocked(actor)) {
        spdlog::info("OutfitLockManager::ProcessEvent - Keeping gallery-spawned item '{}' "
            "(0x{:08X}) on '{}': something else undressed them and the lock is about to put "
            "it back", baseObjectName, a_event->baseObject, actorName);
        return RE::BSEventNotifyControl::kContinue;
    }

    // Capture IDs for deferred task (pointers may become invalid)
    RE::FormID itemID = a_event->baseObject;
    std::string itemName = baseObjectName;

    // Unmark immediately to prevent double-processing if re-equipped quickly
    UnmarkGalleryItem(actor, itemID);

    // Defer the actual removal to the next frame to avoid race condition
    // The unequip operation may still be modifying inventory state when this event fires
    SKSE::GetTaskInterface()->AddTask([actorID, itemID, itemName]() {
        auto* targetActor = RE::TESForm::LookupByID<RE::Actor>(actorID);
        auto* form = RE::TESForm::LookupByID(itemID);

        if (targetActor && form) {
            // The frame this waits out is long enough for the piece to have gone back on -
            // anything that clears a slot before filling it with the same item unequips
            // and re-equips inside one frame - and removing it then would delete gear the
            // actor is wearing. Put the mark back and leave it alone.
            auto* armor = form->As<RE::TESObjectARMO>();
            if (armor && ItemEquipHelper::IsArmorEquippedOrPending(targetActor, armor)) {
                GetSingleton()->MarkItemAsGallerySpawned(targetActor, itemID);
                spdlog::info("OutfitLockManager - Gallery-spawned item '{}' (0x{:08X}) went back "
                    "on '{}' before the removal ran; keeping it",
                    itemName, itemID, targetActor->GetName());
                return;
            }

            auto* boundObj = form->As<RE::TESBoundObject>();
            if (boundObj) {
                targetActor->RemoveItem(boundObj, 1,
                    RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                spdlog::info("OutfitLockManager - Destroyed gallery-spawned item '{}' (0x{:08X}) from '{}'",
                    itemName, itemID, targetActor->GetName());
            }
        }
    });

    spdlog::info("OutfitLockManager::ProcessEvent - Queued destruction of gallery-spawned item '{}' (0x{:08X}) from '{}'",
        baseObjectName, itemID, actorName);

    return RE::BSEventNotifyControl::kContinue;
}

// =============================================================================
// Re-entrancy, storm detection and debounce
// =============================================================================

void OutfitLockManager::BeginSelfDriven(RE::FormID actorID)
{
    std::lock_guard<std::mutex> lock(m_selfDrivenMutex);
    m_selfDriven.insert(actorID);
    m_selfDrivenCount.store(static_cast<int>(m_selfDriven.size()), std::memory_order_relaxed);
}

void OutfitLockManager::EndSelfDriven(RE::FormID actorID)
{
    std::lock_guard<std::mutex> lock(m_selfDrivenMutex);
    m_selfDriven.erase(actorID);
    m_selfDrivenCount.store(static_cast<int>(m_selfDriven.size()), std::memory_order_relaxed);
}

bool OutfitLockManager::IsSelfDriven(RE::FormID actorID) const
{
    // Nothing in flight is the normal state, and checking that costs one atomic load
    // rather than a lock on the game thread.
    if (m_selfDrivenCount.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_selfDrivenMutex);
    return m_selfDriven.contains(actorID);
}

bool OutfitLockManager::NoteEquipAndCheckStorm(RE::FormID actorID, const char* actorName)
{
    const std::int64_t now = NowMs();

    std::lock_guard<std::mutex> lock(m_equipRateMutex);
    auto& rate = m_equipRate[actorID];

    if (now < rate.backoffUntilMs) {
        return true;
    }

    // Backoff just expired - start clean rather than judging on a stale window.
    if (rate.backoffUntilMs != 0) {
        rate.backoffUntilMs = 0;
        rate.reported = false;
        rate.windowStartMs = now;
        rate.count = 0;
    }

    if (now - rate.windowStartMs > kStormWindowMs) {
        rate.windowStartMs = now;
        rate.count = 0;
    }

    if (++rate.count < kStormThreshold) {
        return false;
    }

    rate.backoffUntilMs = now + kStormBackoffMs;
    if (!rate.reported) {
        rate.reported = true;
        spdlog::error("OutfitLockManager - EQUIP STORM: '{}' (0x{:08X}) produced {} equip events in "
                      "under {}ms. Backing off reapplies for {}s. Something else is cycling this "
                      "actor's equipment; every one of those events also runs XPMSE/DD/OBody "
                      "Papyrus, which is what drives the VM into a stack-dump freeze.",
            actorName ? actorName : "unknown", actorID, rate.count, kStormWindowMs,
            kStormBackoffMs / 1000);
    }
    return true;
}

bool OutfitLockManager::IsInEquipBackoff(RE::FormID actorID) const
{
    std::lock_guard<std::mutex> lock(m_equipRateMutex);
    const auto it = m_equipRate.find(actorID);
    return it != m_equipRate.end() && NowMs() < it->second.backoffUntilMs;
}

bool OutfitLockManager::ClearEquipBackoff(RE::FormID actorID)
{
    const std::int64_t now = NowMs();
    bool cleared = false;

    {
        std::lock_guard<std::mutex> lock(m_equipRateMutex);
        const auto it = m_equipRate.find(actorID);
        if (it != m_equipRate.end() && now < it->second.backoffUntilMs) {
            cleared = true;
        }
        // Erased rather than zeroed: leaving the window and count behind would have the
        // breaker judge the next second against events from before the stand-down.
        m_equipRate.erase(actorID);
    }

    {
        std::lock_guard<std::mutex> lock(m_reapplyMutex);
        const auto it = m_reapply.find(actorID);
        if (it != m_reapply.end() && now < it->second.backoffUntilMs) {
            cleared = true;
            it->second.backoffUntilMs = 0;
            it->second.burst = 0.0;
            it->second.reported = false;
        }
    }

    return cleared;
}

void OutfitLockManager::BeginUserEdit()
{
    m_userEditCount.fetch_add(1, std::memory_order_relaxed);
}

void OutfitLockManager::EndUserEdit()
{
    m_userEditCount.fetch_sub(1, std::memory_order_relaxed);
}

void OutfitLockManager::RefreshLockedCount()
{
    int locked = 0;
    for (const auto& [key, outfit] : m_outfits) {
        if (key.outfitName == "locked") ++locked;
    }
    m_lockedCount.store(locked, std::memory_order_relaxed);
}

void OutfitLockManager::NoteExternalOutfitChange(RE::Actor* actor, RE::FormID baseObject)
{
    // A locked outfit is armour only. Drawing a sword or burning through arrows is not a
    // change to the look we are defending, and reacting to it would make every fight a
    // reapply storm on its own.
    auto* form = RE::TESForm::LookupByID(baseObject);
    auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
    if (!armor) {
        return;
    }

    // Devious Devices equips and unequips the rendered half of a device constantly - on
    // load, on cell change, whenever it repairs a pairing. None of that is a change to the
    // look we are defending, because the rendered half is not in the outfit; reacting to it
    // scheduled a reapply that then found nothing to do, once per device per transition.
    if (DeviceCompat::IsRenderedDevice(armor)) {
        return;
    }

    const RE::FormID actorID = actor->GetFormID();
    const std::int64_t now = NowMs();

    // Peek before the expensive test: during a burst this is every event after the first,
    // and IsLocked takes m_mutex - which ApplyOutfit holds - on the game thread.
    {
        std::lock_guard<std::mutex> lock(m_reapplyMutex);
        const auto it = m_reapply.find(actorID);
        if (it != m_reapply.end() && (it->second.pending || now < it->second.backoffUntilMs)) {
            return;
        }
    }

    if (!IsLocked(actor)) {
        return;
    }

    const auto delayMs = Settings::GetSingleton()->GetReapplyDelayMs();

    {
        std::lock_guard<std::mutex> lock(m_reapplyMutex);
        auto& state = m_reapply[actorID];
        if (state.pending || now < state.backoffUntilMs) {
            return;  // lost the race against another event on this actor
        }
        state.pending = true;
    }

    spdlog::info("OutfitLockManager - '{}' (0x{:08X}) had armour changed by something else; "
                 "reapplying the locked outfit in {}ms",
        actor->GetName(), actorID, delayMs);

    PapyrusBridge::RunAfterMs(delayMs, [actorID]() {
        GetSingleton()->RunPendingReapply(actorID);
    });
}

void OutfitLockManager::RunPendingReapply(RE::FormID actorID)
{
    // Clear the coalescing flag first, whatever happens below: leaving it set would mean
    // this actor never schedules another reapply for the rest of the session.
    {
        std::lock_guard<std::mutex> lock(m_reapplyMutex);
        const auto it = m_reapply.find(actorID);
        if (it == m_reapply.end()) {
            return;  // reverted out from under us
        }
        it->second.pending = false;
    }

    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorID);
    if (!actor || !actor->Is3DLoaded() || !IsLocked(actor)) {
        return;
    }

    // A menu edit that started during the delay owns this actor's gear for now, and will
    // re-save the lock when it ends.
    if (m_userEditCount.load(std::memory_order_relaxed) != 0) {
        spdlog::trace("OutfitLockManager - Skipping reapply for '{}': menu edit in progress",
            actor->GetName());
        return;
    }

    // The per-event breaker may have tripped while we were waiting.
    if (IsInEquipBackoff(actorID)) {
        spdlog::warn("OutfitLockManager - Not reapplying to '{}': equip-storm backoff", actor->GetName());
        return;
    }

    if (!DiffersFromLockedOutfit(actor)) {
        spdlog::trace("OutfitLockManager - '{}' is already wearing the locked outfit, nothing to reapply",
            actor->GetName());
        return;
    }

    if (!ClaimReapplySlot(actorID, actor->GetName())) {
        return;
    }

    // Same bracket the location-change path uses: the unequip half would otherwise trip
    // SeverActions' alias debounce and have it re-dress the NPC on top of us.
    SeverActionsCompat::SuspendLock(actor);
    ApplyOutfit(actor, "locked", true);
    SeverActionsCompat::ResumeLockAfterMs(actor, SeverActionsCompat::kEquipSettleMs);
}

bool OutfitLockManager::DiffersFromLockedOutfit(RE::Actor* actor) const
{
    SavedOutfit outfit;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_outfits.find(OutfitKey{actor->GetFormID(), "locked"});
        if (it == m_outfits.end()) {
            return false;
        }
        outfit = it->second;  // FormKey resolution below is not worth holding the lock for
    }

    // Both sides skip body parts and rendered devices, the same way GetEquippedArmor and
    // ApplyOutfit do: TNG owns the former and DD the latter, and both put their pieces back
    // regardless, so counting them would make every actor look permanently diverged.
    std::vector<RE::FormID> want;
    want.reserve(outfit.items.size());
    for (const auto& item : outfit.items) {
        auto* armor = item.GetArmor();
        if (!armor) continue;
        if (ItemEquipHelper::IsBodyPart(armor)) continue;
        if (DeviceCompat::IsRenderedDevice(armor)) continue;
        want.push_back(armor->GetFormID());
    }

    std::vector<RE::FormID> worn;
    for (auto* armor : GetEquippedArmor(actor)) {
        worn.push_back(armor->GetFormID());
    }

    std::sort(want.begin(), want.end());
    std::sort(worn.begin(), worn.end());
    return want != worn;
}

bool OutfitLockManager::ClaimReapplySlot(RE::FormID actorID, const char* actorName)
{
    const auto* settings = Settings::GetSingleton();
    const double allowance = static_cast<double>(settings->GetReapplyBurstAllowance());
    const double drainPerMs = 1.0 / (settings->GetReapplySustainedIntervalSec() * 1000.0);
    const std::int64_t backoffMs = static_cast<std::int64_t>(settings->GetReapplyBackoffSec()) * 1000;

    const std::int64_t now = NowMs();

    std::lock_guard<std::mutex> lock(m_reapplyMutex);
    auto& state = m_reapply[actorID];

    if (now < state.backoffUntilMs) {
        return false;
    }

    // Drain what the elapsed time has earned back, then charge for this reapply. Under the
    // allowance nothing is throttled at all, which is what keeps an ordinary burst - or an
    // NPC who gets stripped once and redressed once - free.
    if (state.lastReapplyMs != 0) {
        state.burst -= static_cast<double>(now - state.lastReapplyMs) * drainPerMs;
        if (state.burst < 0.0) state.burst = 0.0;
    }
    state.lastReapplyMs = now;
    state.burst += 1.0;

    if (state.burst <= allowance) {
        state.reported = false;
        return true;
    }

    state.backoffUntilMs = now + backoffMs;
    state.burst = 0.0;
    if (!state.reported) {
        state.reported = true;
        spdlog::error("OutfitLockManager - OUTFIT TUG-OF-WAR: '{}' (0x{:08X}) has been redressed "
                      "{} times faster than one per {}s. Something else is changing this NPC's "
                      "outfit as fast as we put it back, so we are standing down for {}s rather "
                      "than trading equips with it - each exchange also runs XPMSE/DD/OBody "
                      "Papyrus for both mods. Unlock this NPC, or turn off "
                      "bReapplyOnExternalChange, if it keeps happening.",
            actorName ? actorName : "unknown", actorID, settings->GetReapplyBurstAllowance(),
            settings->GetReapplySustainedIntervalSec(), settings->GetReapplyBackoffSec());
    }
    return false;
}

RE::BSEventNotifyControl OutfitLockManager::ProcessEvent(
    const RE::TESActorLocationChangeEvent* a_event,
    RE::BSTEventSource<RE::TESActorLocationChangeEvent>*)
{
    if (!a_event || !a_event->actor) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* actor = a_event->actor->As<RE::Actor>();
    if (!actor) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Only process when the PLAYER changes location
    // When player enters a new location, apply locked outfits to NPCs in the area
    if (!actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    std::string newLocName = "Unknown";
    if (a_event->newLoc && a_event->newLoc->fullName.c_str()) {
        newLocName = a_event->newLoc->fullName.c_str();
    }

    spdlog::info("OutfitLockManager::ProcessEvent(LocationChange) - Player entered location '{}', applying locked outfits",
        newLocName);

    // Apply locked outfits to NPCs in the new location
    ApplyLockedOutfitsInLocation(a_event->newLoc);

    return RE::BSEventNotifyControl::kContinue;
}

std::vector<RE::TESObjectARMO*> OutfitLockManager::GetEquippedArmor(RE::Actor* actor) const
{
    std::vector<RE::TESObjectARMO*> result;
    if (!actor) return result;

    // Asked per item, against each armour's own slot mask, rather than by probing a fixed
    // list of biped slots. The old list was 30 and 32-45, which misses everything worn in
    // 46-61 - where the Devious-Devices-style pieces sit. UndressManager decides what to
    // take off with exactly this test, so a snapshot built any other way could not put
    // back everything an undress had removed: it saved "no equipped armor" for an NPC
    // wearing three visible items, and the redress that followed restored nothing.
    for (auto* armor : ItemEquipHelper::GetInventoryItems<RE::TESObjectARMO>(actor)) {
        if (!armor || !ItemEquipHelper::IsArmorEquipped(actor, armor)) continue;

        // Not ours to record or to strip - see ItemEquipHelper::IsBodyPart.
        if (ItemEquipHelper::IsBodyPart(armor)) continue;

        // A Devious Devices item is two armour records, and only the inventory half is
        // ours to record. The rendered half is put on by DD in reaction to that one, so
        // storing it would mean the outfit describes the same device twice - and putting
        // it back would mean equipping it behind DD's back. See DeviceCompat.
        if (DeviceCompat::IsRenderedDevice(armor)) continue;

        // Taken off already as far as the player is concerned - the removal is a
        // zadlibs.UnlockDevice still working its way through Papyrus. Counting it would put
        // the device the player just removed back into the outfit, and the reapply behind
        // the removal would lock it on again. See ItemEquipHelper's Pending note.
        if (ItemEquipHelper::IsPendingUnequip(actor, armor)) {
            spdlog::debug("OutfitLockManager::GetEquippedArmor - skipping '{}' (0x{:08X}): "
                "removed this frame, the engine still shows it worn",
                armor->GetFullName(), armor->GetFormID());
            continue;
        }

        // An armour piece covering several slots is still one item.
        const auto duplicate = std::find_if(result.begin(), result.end(),
            [armor](RE::TESObjectARMO* existing) {
                return existing->GetFormID() == armor->GetFormID();
            });
        if (duplicate == result.end()) {
            result.push_back(armor);
        }
    }

    // Plus anything we have asked the engine to put on that it has not applied yet. Equips
    // are queued, so a snapshot taken in the same frame as one - which is exactly what the
    // re-save at the end of a user edit is - would otherwise miss the piece the player just
    // picked, and the reapply that follows would strip it as somebody else's gear. See
    // ItemEquipHelper's "Queued equips" note.
    for (auto* armor : ItemEquipHelper::GetPendingEquips(actor)) {
        if (!armor || ItemEquipHelper::IsBodyPart(armor)) continue;
        if (DeviceCompat::IsRenderedDevice(armor)) continue;

        const auto duplicate = std::find_if(result.begin(), result.end(),
            [armor](RE::TESObjectARMO* existing) {
                return existing->GetFormID() == armor->GetFormID();
            });
        if (duplicate != result.end()) continue;

        // One item per biped slot, the same rule the actor lives under. A queued equip
        // displaces whatever the engine still shows in the slots it covers, because that
        // is exactly what the engine will do the moment it applies the equip - so the
        // piece being displaced leaves the snapshot with it.
        //
        // Without this the snapshot could describe a body no actor can have. Daegon's
        // went out holding her Daegon Cuirass and an Elven Sentry Cuirass, both slot 32,
        // and Royal Boots and Sabatons, both slot 37: a queued equip that never landed
        // was being counted alongside the piece it had failed to replace. That set then
        // went into the locked outfit, and an outfit is not a list the engine can wear.
        const auto slots = static_cast<std::uint32_t>(armor->GetSlotMask());
        if (slots != 0) {
            result.erase(std::remove_if(result.begin(), result.end(),
                [armor, slots](RE::TESObjectARMO* worn) {
                    if ((static_cast<std::uint32_t>(worn->GetSlotMask()) & slots) == 0) {
                        return false;
                    }
                    spdlog::debug("OutfitLockManager::GetEquippedArmor - dropping '{}' "
                        "(0x{:08X}): '{}' is going on over it",
                        worn->GetFullName(), worn->GetFormID(), armor->GetFullName());
                    return true;
                }), result.end());
        }

        spdlog::debug("OutfitLockManager::GetEquippedArmor - counting '{}' (0x{:08X}): equipped "
            "this frame, engine has not applied it yet",
            armor->GetFullName(), armor->GetFormID());
        result.push_back(armor);
    }

    return result;
}

void OutfitLockManager::UnequipArmorExcept(RE::Actor* actor,
    const std::vector<RE::TESObjectARMO*>& keep)
{
    if (!actor) return;

    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) {
        spdlog::error("OutfitLockManager::UnequipArmorExcept - No ActorEquipManager");
        return;
    }

    for (auto* armor : GetEquippedArmor(actor)) {
        const bool wanted = std::find_if(keep.begin(), keep.end(),
            [armor](RE::TESObjectARMO* target) {
                return target && target->GetFormID() == armor->GetFormID();
            }) != keep.end();

        if (wanted) continue;

        // Devices come off through DD, which removes the rendered half and unwinds the
        // effects with it. Pulling the inventory half out from under DD instead is what
        // made it re-equip the device a moment later.
        if (DeviceCompat::IsInventoryDevice(armor)) {
            if (DeviceCompat::Unequip(actor, armor)) {
                ItemEquipHelper::NotePendingUnequip(actor, armor);
            } else {
                spdlog::debug("OutfitLockManager::UnequipArmorExcept - '{}' stays on: Devious "
                    "Devices will not release it", armor->GetFullName());
            }
            continue;
        }

        equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, true);
        spdlog::trace("OutfitLockManager::UnequipArmorExcept - Unequipped '{}'", armor->GetFullName());
    }
}

void OutfitLockManager::EquipArmorList(RE::Actor* actor, const std::vector<RE::TESObjectARMO*>& items)
{
    if (!actor) return;

    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) {
        spdlog::error("OutfitLockManager::EquipArmorList - No ActorEquipManager");
        return;
    }

    for (auto* armor : items) {
        if (!armor) continue;

        // Already worn: equipping it again is a no-op the engine still turns into a
        // TESEquipEvent, and every one of those runs a slice of Papyrus in every mod
        // listening. On a reapply where nothing changed this is the whole list.
        if (ItemEquipHelper::IsArmorEquippedOrPending(actor, armor)) {
            continue;
        }

        // Check if actor has this item in inventory
        auto inventory = actor->GetInventory([armor](RE::TESBoundObject& obj) {
            return obj.GetFormID() == armor->GetFormID();
        });

        if (!inventory.empty()) {
            // Devices go on through DD, which puts the rendered half on and starts the
            // effects. Equipping the inventory half directly fires DD's OnEquipped anyway,
            // but with none of the state it expects, so it re-runs the whole lock sequence
            // to repair itself - once per reapply, i.e. once per cell change.
            if (DeviceCompat::IsInventoryDevice(armor)) {
                DeviceCompat::Equip(actor, armor);
                continue;
            }

            equipManager->EquipObject(actor, armor, nullptr, 1, nullptr, true, false, false);
            spdlog::trace("OutfitLockManager::EquipArmorList - Equipped '{}'", armor->GetFullName());
        } else {
            spdlog::warn("OutfitLockManager::EquipArmorList - Actor doesn't have '{}' in inventory",
                armor->GetFullName());
        }
    }
}

std::string OutfitLockManager::GetModName(RE::TESForm* form) const
{
    if (!form) return "";

    std::uint8_t fullIndex = form->GetFormID() >> 24;
    const RE::TESFile* file = nullptr;

    if (fullIndex == 0xFE) {
        // Light mod
        std::uint16_t lightIndex = (form->GetFormID() & 0x00FFFFFF) >> 12;
        file = RE::TESDataHandler::GetSingleton()->LookupLoadedLightModByIndex(lightIndex);
    } else {
        file = RE::TESDataHandler::GetSingleton()->LookupLoadedModByIndex(fullIndex);
    }

    return file ? std::string(file->GetFilename()) : "";
}

bool OutfitLockManager::SaveOutfit(RE::Actor* actor, const std::string& outfitName)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::SaveOutfit - No actor provided");
        return false;
    }

    spdlog::info("OutfitLockManager::SaveOutfit - Saving outfit '{}' for actor '{}' (0x{:08X})",
        outfitName, actor->GetName(), actor->GetFormID());

    auto equipped = GetEquippedArmor(actor);
    if (equipped.empty()) {
        spdlog::warn("OutfitLockManager::SaveOutfit - Actor has no equipped armor");
    }

    SavedOutfit outfit;
    for (auto* armor : equipped) {
        std::string formKey = Persistence::FormKeyUtil::BuildFormKey(armor);
        if (formKey.empty()) {
            spdlog::warn("  - Skipping armor '{}' - no source file (dynamic item?)",
                armor->GetFullName());
            continue;
        }

        SavedArmorItem item;
        item.formKey = formKey;
        outfit.items.push_back(item);

        spdlog::debug("  - Saved armor '{}' as '{}'",
            armor->GetFullName(), formKey);
    }

    OutfitKey key{actor->GetFormID(), outfitName};

    // The stored count, not the worn count: an item with no source file is skipped above, and
    // reporting what was worn made the log claim to have saved a piece it had dropped.
    const size_t stored = outfit.items.size();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outfits[key] = std::move(outfit);
        RefreshLockedCount();
    }

    spdlog::info("OutfitLockManager::SaveOutfit - Saved {} of {} worn items for outfit '{}'",
        stored, equipped.size(), outfitName);

    return true;
}

// =============================================================================
// Menu-edit reconciliation
// =============================================================================

std::vector<RE::FormID> OutfitLockManager::SnapshotWornArmor(RE::Actor* actor) const
{
    std::vector<RE::FormID> ids;
    if (!actor) return ids;

    for (auto* armor : GetEquippedArmor(actor)) {
        if (armor) ids.push_back(armor->GetFormID());
    }

    // Sorted so the two snapshots either side of an edit can be differenced directly.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void OutfitLockManager::ReconcileBeforeUserEdit(RE::Actor* actor)
{
    if (!actor || actor->IsPlayerRef()) {
        return;
    }

    if (!Settings::GetSingleton()->IsReapplyOnExternalChangeEnabled()) {
        return;  // the player has asked us not to fight over gear at all
    }

    if (!IsLocked(actor)) {
        return;
    }

    // Already wearing the locked set: nothing to correct, and the reapply would be pure
    // equip traffic. This is the overwhelmingly common case.
    if (!DiffersFromLockedOutfit(actor)) {
        return;
    }

    const RE::FormID actorID = actor->GetFormID();

    {
        const std::int64_t now = NowMs();
        std::lock_guard<std::mutex> lock(m_lastApplyMutex);
        const auto it = m_lastApplyMs.find(actorID);
        if (it != m_lastApplyMs.end() && now - it->second < kUserEditReconcileDebounceMs) {
            spdlog::trace("OutfitLockManager::ReconcileBeforeUserEdit - '{}' was reapplied "
                "{}ms ago, letting it land", actor->GetName(), now - it->second);
            return;
        }
    }

    // The breaker exists so we do not trade equips with whatever is cycling this actor.
    // The player opening the wheel on them is the one signal that outranks it: they are
    // about to edit this outfit, and editing it from a state some other mod chose is how
    // the wrong gear ends up in the lock for good.
    const bool clearedBackoff = ClearEquipBackoff(actorID);

    spdlog::info("OutfitLockManager::ReconcileBeforeUserEdit - '{}' (0x{:08X}) is not wearing "
        "their locked outfit{}; putting it back before the edit",
        actor->GetName(), actorID,
        clearedBackoff ? ", and was in a stand-down the player opening the wheel overrides"
                       : "");

    // Same bracket every other reapply path uses - the unequip half would otherwise trip
    // SeverActions' alias debounce and have it re-dress the NPC on top of us.
    SeverActionsCompat::SuspendLock(actor);
    ApplyOutfit(actor, "locked", true);
    SeverActionsCompat::ResumeLockAfterMs(actor, SeverActionsCompat::kEquipSettleMs);
}

bool OutfitLockManager::UpdateLockedOutfitFromEdit(RE::Actor* actor,
    const std::vector<RE::FormID>& wornBefore)
{
    if (!actor) {
        return false;
    }

    const std::vector<RE::FormID> wornAfter = SnapshotWornArmor(actor);

    // Only what this edit changed. A piece worn on both sides is left exactly as the
    // stored outfit already has it, which is the whole point: gear another mod put on
    // while we were backed off is in both snapshots, so it can never be laundered into
    // the lock by the player clicking something unrelated.
    std::vector<RE::FormID> added;
    std::vector<RE::FormID> removed;
    std::set_difference(wornAfter.begin(), wornAfter.end(),
        wornBefore.begin(), wornBefore.end(), std::back_inserter(added));
    std::set_difference(wornBefore.begin(), wornBefore.end(),
        wornAfter.begin(), wornAfter.end(), std::back_inserter(removed));

    // Resolved outside the lock: BuildFormKey walks the data handler, and m_mutex is the
    // one ApplyOutfit holds on the game thread.
    struct Change
    {
        std::string formKey;
        std::string name;
    };

    const auto describe = [](RE::FormID id) -> Change {
        Change change;
        auto* form = RE::TESForm::LookupByID(id);
        if (!form) return change;
        change.formKey = Persistence::FormKeyUtil::BuildFormKey(form);
        const char* name = form->GetName();
        change.name = (name && name[0]) ? name : fmt::format("FormID:{:08X}", id);
        return change;
    };

    std::vector<Change> toAdd;
    std::vector<Change> toRemove;
    for (const RE::FormID id : added) {
        auto change = describe(id);
        if (!change.formKey.empty()) toAdd.push_back(std::move(change));
    }
    for (const RE::FormID id : removed) {
        auto change = describe(id);
        if (!change.formKey.empty()) toRemove.push_back(std::move(change));
    }

    std::size_t stored = 0;
    std::vector<std::string> leftOut;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_outfits.find(OutfitKey{actor->GetFormID(), "locked"});
        if (it == m_outfits.end()) {
            // Unlocked from inside the edit. Nothing to fold into, and re-creating the
            // outfit here would undo the unlock the player just asked for.
            spdlog::info("OutfitLockManager::UpdateLockedOutfitFromEdit - '{}' is no longer "
                "locked; leaving it that way", actor->GetName());
            return false;
        }

        auto& items = it->second.items;
        if (it->second.actorFormKey.empty()) {
            it->second.actorFormKey = Persistence::FormKeyUtil::BuildFormKey(actor);
        }

        for (const auto& change : toRemove) {
            const auto before = items.size();
            items.erase(std::remove_if(items.begin(), items.end(),
                [&change](const SavedArmorItem& item) {
                    return item.formKey == change.formKey;
                }), items.end());
            if (items.size() != before) {
                spdlog::debug("  - Took '{}' ({}) out of the locked outfit",
                    change.name, change.formKey);
            }
        }

        for (const auto& change : toAdd) {
            const bool present = std::any_of(items.begin(), items.end(),
                [&change](const SavedArmorItem& item) {
                    return item.formKey == change.formKey;
                });
            if (present) continue;

            SavedArmorItem item;
            item.formKey = change.formKey;
            items.push_back(std::move(item));
            spdlog::debug("  - Put '{}' ({}) into the locked outfit",
                change.name, change.formKey);
        }

        stored = items.size();
        RefreshLockedCount();

        // Anything worn that this edit did not touch and the outfit does not hold is
        // exactly what the old snapshot used to absorb. Named rather than silently
        // dropped: it means something outside the wheel is dressing this actor, and that
        // is the thing to go and fix.
        for (const RE::FormID id : wornAfter) {
            if (!std::binary_search(wornBefore.begin(), wornBefore.end(), id)) {
                continue;  // this edit put it on; already handled above
            }
            auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
            if (!armor) continue;

            const std::string formKey = Persistence::FormKeyUtil::BuildFormKey(armor);
            const bool inOutfit = std::any_of(items.begin(), items.end(),
                [&formKey](const SavedArmorItem& item) { return item.formKey == formKey; });
            if (inOutfit) continue;

            leftOut.push_back(fmt::format("'{}' ({})", armor->GetFullName(), formKey));
        }
    }

    for (const auto& piece : leftOut) {
        spdlog::warn("OutfitLockManager::UpdateLockedOutfitFromEdit - '{}' is wearing {}, which "
            "this edit did not put there and the locked outfit does not hold. Left out of the "
            "lock; the next reapply takes it off.", actor->GetName(), piece);
    }

    spdlog::info("OutfitLockManager::UpdateLockedOutfitFromEdit - '{}' (0x{:08X}): +{} / -{}, "
        "outfit now holds {} item(s){}",
        actor->GetName(), actor->GetFormID(), toAdd.size(), toRemove.size(), stored,
        leftOut.empty() ? std::string{}
                        : fmt::format(", {} worn item(s) left out", leftOut.size()));

    return true;
}

bool OutfitLockManager::ApplyOutfit(RE::Actor* actor, const std::string& outfitName, bool unequipOthers)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::ApplyOutfit - No actor provided");
        return false;
    }

    // Restore anything the actor is missing before the loop below gets to it. That
    // loop prunes items the actor does not have, and the pruning rewrites the stored
    // outfit - so a SPID reapply, which strips the previous outfit's items out of the
    // inventory, would otherwise make the lock delete itself one piece at a time.
    if (outfitName == "locked") {
        EnsureOutfitItemsInInventory(actor, outfitName);
    }

    OutfitKey key{actor->GetFormID(), outfitName};
    SavedOutfit outfit;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_outfits.find(key);
        if (it == m_outfits.end()) {
            spdlog::warn("OutfitLockManager::ApplyOutfit - No outfit '{}' found for actor '{}'",
                outfitName, actor->GetName());
            return false;
        }
        outfit = it->second;  // Copy so we can work outside the lock
    }

    spdlog::info("OutfitLockManager::ApplyOutfit - Applying outfit '{}' to actor '{}' (unequipOthers={})",
        outfitName, actor->GetName(), unequipOthers);

    // Build list of valid armor to equip, removing invalid items or items NPC doesn't have.
    //
    // Built *before* anything is taken off. This used to strip the actor first and then
    // decide what to put back, which meant a reapply that changed nothing still generated
    // an unequip and an equip for every piece - and each of those runs XPMSE's restyle,
    // Devious Devices' slotmask rebuild and an OBody preset pass in Papyrus. Diffing
    // against what is already worn makes the common no-change reapply cost zero events.
    std::vector<RE::TESObjectARMO*> validArmor;
    std::vector<std::string> keysToRemove;

    for (const auto& item : outfit.items) {
        if (!item.IsValid()) {
            keysToRemove.push_back(item.formKey);
            spdlog::warn("  - Item '{}' is no longer valid (mod uninstalled?), removing from outfit",
                item.formKey);
            continue;
        }

        auto* armor = item.GetArmor();

        // Drops body parts back out of outfits stored before we started skipping them,
        // so an old save stops re-equipping a TNG addon the player has since removed.
        if (ItemEquipHelper::IsBodyPart(armor)) {
            keysToRemove.push_back(item.formKey);
            spdlog::debug("  - '{}' ({}) is a body part, not clothing - dropping it from the outfit "
                "and leaving it to the mod that manages it", armor->GetFullName(), item.formKey);
            continue;
        }

        // Same for the rendered half of a Devious Devices item. Outfits snapshotted before
        // we understood the pairing hold these, and putting one back by hand is precisely
        // the desync DD then spends a cell transition repairing. The inventory half in the
        // same outfit is what actually restores the device.
        if (DeviceCompat::IsRenderedDevice(armor)) {
            keysToRemove.push_back(item.formKey);
            spdlog::debug("  - {} is the rendered half of a Devious Device - dropping it from the "
                "outfit and leaving it to DD", item.formKey);
            continue;
        }

        // Check if NPC actually has this item in their inventory
        auto inventory = actor->GetInventory([armor](RE::TESBoundObject& obj) {
            return obj.GetFormID() == armor->GetFormID();
        });

        if (inventory.empty()) {
            keysToRemove.push_back(item.formKey);
            spdlog::warn("  - NPC doesn't have '{}' ({}) in inventory, removing from outfit",
                armor->GetFullName(), item.formKey);
            continue;
        }

        validArmor.push_back(armor);
        spdlog::debug("  - Equipping '{}' ({})",
            armor->GetFullName(), item.formKey);
    }

    // If we had items to remove, update the stored outfit
    if (!keysToRemove.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_outfits.find(key);
        if (it != m_outfits.end()) {
            // Remove items that are no longer valid or NPC doesn't have
            it->second.items.erase(
                std::remove_if(it->second.items.begin(), it->second.items.end(),
                    [&keysToRemove](const SavedArmorItem& item) {
                        return std::find(keysToRemove.begin(), keysToRemove.end(), item.formKey) != keysToRemove.end();
                    }),
                it->second.items.end());
            spdlog::debug("OutfitLockManager::ApplyOutfit - Removed {} items from outfit",
                keysToRemove.size());
        }
    }

    // Our own equip traffic from here on, so the handler above ignores it rather than
    // treating it as the player undressing someone.
    {
        SelfDrivenScope selfDriven(this, actor->GetFormID());

        if (unequipOthers) {
            UnequipArmorExcept(actor, validArmor);
        }

        EquipArmorList(actor, validArmor);
    }

    {
        std::lock_guard<std::mutex> lock(m_lastApplyMutex);
        m_lastApplyMs[actor->GetFormID()] = NowMs();
    }

    spdlog::info("OutfitLockManager::ApplyOutfit - Applied {} items from outfit '{}'",
        validArmor.size(), outfitName);

    return true;
}

bool OutfitLockManager::DeleteOutfit(RE::Actor* actor, const std::string& outfitName)
{
    if (!actor) return false;

    OutfitKey key{actor->GetFormID(), outfitName};

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_outfits.find(key);
    if (it == m_outfits.end()) {
        spdlog::warn("OutfitLockManager::DeleteOutfit - No outfit '{}' found for actor '{}'",
            outfitName, actor->GetName());
        return false;
    }

    m_outfits.erase(it);
    RefreshLockedCount();
    spdlog::info("OutfitLockManager::DeleteOutfit - Deleted outfit '{}' for actor '{}'",
        outfitName, actor->GetName());

    return true;
}

bool OutfitLockManager::HasOutfit(RE::Actor* actor, const std::string& outfitName) const
{
    if (!actor) return false;

    OutfitKey key{actor->GetFormID(), outfitName};

    std::lock_guard<std::mutex> lock(m_mutex);
    return m_outfits.find(key) != m_outfits.end();
}

bool OutfitLockManager::Lock(RE::Actor* actor)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::Lock - No actor provided");
        return false;
    }

    if (actor->IsPlayerRef()) {
        spdlog::warn("OutfitLockManager::Lock - Cannot lock player");
        return false;
    }

    spdlog::info("OutfitLockManager::Lock - Locking actor '{}' (0x{:08X})",
        actor->GetName(), actor->GetFormID());

    if (!SaveOutfit(actor, "locked")) {
        return false;
    }

    // We defend the look from here on: SeverActions is asked to leave this NPC alone if
    // it is installed, and the outfit record is what makes SPID stand down.
    SeverActionsCompat::TakeOver(actor);
    PromoteLockToOutfitForm(actor);

    return true;
}

bool OutfitLockManager::AdoptStoredOutfit(RE::Actor* actor, const std::string& sourceName)
{
    if (!actor || actor->IsPlayerRef()) {
        return false;
    }

    // Copied under the lock, applied outside it: ApplyOutfit takes m_mutex itself, and it
    // is not recursive. The copy has to land *before* the suspension below is constructed,
    // so that it sees the actor as locked and folds the apply into "locked" on exit.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto source = m_outfits.find(OutfitKey{actor->GetFormID(), sourceName});
        if (source == m_outfits.end()) {
            spdlog::warn("OutfitLockManager::AdoptStoredOutfit - No outfit '{}' for '{}'",
                sourceName, actor->GetName());
            return false;
        }
        m_outfits[OutfitKey{actor->GetFormID(), "locked"}] = source->second;
        RefreshLockedCount();
    }

    spdlog::info("OutfitLockManager::AdoptStoredOutfit - Putting '{}' into '{}'",
        actor->GetName(), sourceName);

    // "locked" already is the new set, so the fold on exit is a no-op for everything this
    // apply changed, and only ever *names* a piece it could not take off - a Devious
    // Device DD refused to release, say - rather than writing it into the outfit.
    // ApplyOutfit re-adds anything a previous unlock's SetOutfit stripped out of the
    // inventory before it equips.
    {
        ScopedLockSuspension suspension(actor);
        ApplyOutfit(actor, "locked", true);
    }

    // The suspension's exit has promoted the look to the outfit record; SeverActions still
    // has to be told, which is what a fresh Lock does as well.
    SeverActionsCompat::TakeOver(actor);

    return true;
}

bool OutfitLockManager::Unlock(RE::Actor* actor)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::Unlock - No actor provided");
        return false;
    }

    spdlog::info("OutfitLockManager::Unlock - Unlocking actor '{}' (0x{:08X})",
        actor->GetName(), actor->GetFormID());

    // Give the NPC back to whoever had them: restoring the original outfit is what lets
    // SPID resume distributing to this actor. Restore first, then release - the other way
    // round the SeverActions alias would be live again for the equip events our own
    // restore is about to queue.
    OutfitFormBackend::GetSingleton()->Restore(actor);
    SeverActionsCompat::Release(actor);

    // Unlocking hands the NPC back whole. Leaving the no-weapon lock running afterwards
    // would keep one half of our grip on an actor the player has explicitly let go.
    WeaponLockManager::GetSingleton()->StopEnforcing(actor);

    return DeleteOutfit(actor, "locked");
}

bool OutfitLockManager::IsLocked(RE::Actor* actor) const
{
    return HasOutfit(actor, "locked");
}

void OutfitLockManager::PromoteLockToOutfitForm(RE::Actor* actor)
{
    if (!actor) return;

    auto* backend = OutfitFormBackend::GetSingleton();
    if (!backend->IsAvailable() || !backend->IsEligible(actor)) return;

    backend->Apply(actor, GetOutfitItemFormKeys(actor, "locked"));
}

void OutfitLockManager::MarkItemAsPlayerGiven(RE::Actor* actor, RE::FormID itemID)
{
    if (!actor || itemID == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_playerGivenItems[actor->GetFormID()].insert(itemID);

    spdlog::debug("OutfitLockManager::MarkItemAsPlayerGiven - Marked item 0x{:08X} as player-given for '{}'",
        itemID, actor->GetName());
}

bool OutfitLockManager::HasPlayerItems(RE::Actor* actor) const
{
    if (!actor) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_playerGivenItems.find(actor->GetFormID());
    if (it == m_playerGivenItems.end() || it->second.empty()) {
        return false;
    }

    // Check if any tracked items are still in NPC's inventory
    for (RE::FormID itemID : it->second) {
        auto* form = RE::TESForm::LookupByID(itemID);
        if (!form) continue;

        auto inventory = actor->GetInventory([itemID](RE::TESBoundObject& obj) {
            return obj.GetFormID() == itemID;
        });

        if (!inventory.empty()) {
            return true;  // At least one player item is still in inventory
        }
    }
    return false;
}

void OutfitLockManager::ReturnPlayerItems(RE::Actor* actor)
{
    if (!actor) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!player || !equipManager) return;

    std::unordered_set<RE::FormID> itemsToReturn;
    RE::FormID actorID = actor->GetFormID();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_playerGivenItems.find(actorID);
        if (it == m_playerGivenItems.end() || it->second.empty()) {
            spdlog::debug("OutfitLockManager::ReturnPlayerItems - No player items tracked for '{}'",
                actor->GetName());
            return;
        }
        itemsToReturn = it->second;  // Copy to work outside lock
    }

    spdlog::debug("OutfitLockManager::ReturnPlayerItems - Checking {} tracked items for '{}'",
        itemsToReturn.size(), actor->GetName());

    std::uint32_t returnedCount = 0;
    std::uint32_t missingCount = 0;
    std::vector<RE::FormID> returnedItems;

    for (RE::FormID itemID : itemsToReturn) {
        auto* form = RE::TESForm::LookupByID(itemID);
        if (!form) {
            ++missingCount;
            continue;
        }

        // Check if NPC still has this item in inventory
        auto inventory = actor->GetInventory([itemID](RE::TESBoundObject& obj) {
            return obj.GetFormID() == itemID;
        });

        if (inventory.empty()) {
            spdlog::trace("  - NPC no longer has item 0x{:08X}", itemID);
            ++missingCount;
            continue;
        }

        // Unequip if it's armor
        if (auto* armor = form->As<RE::TESObjectARMO>()) {
            if (DeviceCompat::IsInventoryDevice(armor)) {
                // A device has to be unlocked before it can change hands, and DD may say
                // no. Taking it back anyway would leave the rendered half on the NPC.
                if (!DeviceCompat::Unequip(actor, armor)) {
                    spdlog::debug("  - Leaving '{}' (0x{:08X}) on '{}': Devious Devices will not "
                        "release it", form->GetName(), itemID, actor->GetName());
                    continue;
                }
            } else {
                equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, true);
            }
        }

        // Transfer to player
        actor->RemoveItem(form->As<RE::TESBoundObject>(), 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, player);

        spdlog::debug("  - Returned '{}' (0x{:08X}) to player", form->GetName(), itemID);
        returnedItems.push_back(itemID);
        ++returnedCount;
    }

    // Remove returned items from tracking
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Clean up returned items from tracking
        if (!returnedItems.empty()) {
            auto it = m_playerGivenItems.find(actorID);
            if (it != m_playerGivenItems.end()) {
                for (RE::FormID itemID : returnedItems) {
                    it->second.erase(itemID);
                }
                // Clean up empty entries
                if (it->second.empty()) {
                    m_playerGivenItems.erase(it);
                }
            }
        }
    }

    spdlog::info("OutfitLockManager::ReturnPlayerItems - Returned {} items to player from '{}' ({} missing/invalid)",
        returnedCount, actor->GetName(), missingCount);
}

void OutfitLockManager::MarkItemAsGallerySpawned(RE::Actor* actor, RE::FormID itemID)
{
    if (!actor || itemID == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_gallerySpawnedItems[actor->GetFormID()].insert(itemID);
    m_hasGalleryItems.store(true, std::memory_order_relaxed);

    spdlog::debug("OutfitLockManager::MarkItemAsGallerySpawned - Marked item 0x{:08X} as gallery-spawned for '{}'",
        itemID, actor->GetName());
}

bool OutfitLockManager::IsGallerySpawnedItem(RE::Actor* actor, RE::FormID itemID) const
{
    if (!actor || itemID == 0) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gallerySpawnedItems.find(actor->GetFormID());
    if (it == m_gallerySpawnedItems.end()) return false;

    return it->second.contains(itemID);
}

void OutfitLockManager::UnmarkGalleryItem(RE::Actor* actor, RE::FormID itemID)
{
    if (!actor || itemID == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gallerySpawnedItems.find(actor->GetFormID());
    if (it != m_gallerySpawnedItems.end()) {
        it->second.erase(itemID);
        // Clean up empty entries
        if (it->second.empty()) {
            m_gallerySpawnedItems.erase(it);
        }
    }
    m_hasGalleryItems.store(!m_gallerySpawnedItems.empty(), std::memory_order_relaxed);
}

void OutfitLockManager::ReleaseGalleryItems(RE::Actor* actor)
{
    std::size_t released = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!actor) {
            for (const auto& [actorID, items] : m_gallerySpawnedItems) {
                released += items.size();
            }
            m_gallerySpawnedItems.clear();
        } else {
            const auto it = m_gallerySpawnedItems.find(actor->GetFormID());
            if (it != m_gallerySpawnedItems.end()) {
                released = it->second.size();
                m_gallerySpawnedItems.erase(it);
            }
        }

        m_hasGalleryItems.store(!m_gallerySpawnedItems.empty(), std::memory_order_relaxed);
    }

    if (released == 0) {
        return;
    }

    // Destructions already queued this frame are not called off: those pieces were taken
    // off inside the session, which is exactly the case the mark is for. This only stops
    // *future* unequips from being read as try-on churn.
    spdlog::info("OutfitLockManager::ReleaseGalleryItems - {} gallery item(s) on '{}' are theirs "
        "to keep now; nothing will be destroyed if they come off later",
        released, actor ? actor->GetName() : "every actor");
}

bool OutfitLockManager::HasDefaultOutfit(RE::Actor* actor) const
{
    return HasOutfit(actor, "default");
}

bool OutfitLockManager::RestoreAndClearDefault(RE::Actor* actor)
{
    if (!actor) {
        spdlog::error("OutfitLockManager::RestoreAndClearDefault - No actor provided");
        return false;
    }

    if (!HasOutfit(actor, "default")) {
        spdlog::warn("OutfitLockManager::RestoreAndClearDefault - No default outfit for '{}'",
            actor->GetName());
        return false;
    }

    spdlog::info("OutfitLockManager::RestoreAndClearDefault - Restoring '{}' to vanilla",
        actor->GetName());

    // Return player items before restoring
    ReturnPlayerItems(actor);

    // Apply the default outfit (unequip others first)
    bool applied = ApplyOutfit(actor, "default", true);

    // Delete the default outfit after restoring
    DeleteOutfit(actor, "default");

    // Also unlock the NPC since they're back to vanilla
    Unlock(actor);

    spdlog::info("OutfitLockManager::RestoreAndClearDefault - '{}' restored to vanilla (success={})",
        actor->GetName(), applied);

    return applied;
}

void OutfitLockManager::OnGameSave(SKSE::SerializationInterface* a_intfc)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    spdlog::info("OutfitLockManager::OnGameSave - Saving {} outfits, {} actors with player items",
        mgr->m_outfits.size(), mgr->m_playerGivenItems.size());

    // === Save Outfits ===
    if (!a_intfc->OpenRecord(kOutfitRecord, kSerializationVersion)) {
        spdlog::error("OutfitLockManager::OnGameSave - Failed to open outfit record");
        return;
    }

    // Write number of outfits
    std::uint32_t outfitCount = static_cast<std::uint32_t>(mgr->m_outfits.size());
    a_intfc->WriteRecordData(&outfitCount, sizeof(outfitCount));

    for (const auto& [key, outfit] : mgr->m_outfits) {
        // Write actor ref ID
        a_intfc->WriteRecordData(&key.actorRefID, sizeof(key.actorRefID));

        // Write outfit name (length + chars)
        std::uint32_t nameLen = static_cast<std::uint32_t>(key.outfitName.size());
        a_intfc->WriteRecordData(&nameLen, sizeof(nameLen));
        a_intfc->WriteRecordData(key.outfitName.data(), nameLen);

        // Write number of items
        std::uint32_t itemCount = static_cast<std::uint32_t>(outfit.items.size());
        a_intfc->WriteRecordData(&itemCount, sizeof(itemCount));

        // Write each item's formKey string (length-prefixed)
        for (const auto& item : outfit.items) {
            std::uint32_t keyLen = static_cast<std::uint32_t>(item.formKey.size());
            a_intfc->WriteRecordData(&keyLen, sizeof(keyLen));
            a_intfc->WriteRecordData(item.formKey.data(), keyLen);
        }

        // v5: the actor's own FormKey. `key.actorRefID` above is a *runtime* ref ID
        // and is only meaningful inside the load order that produced it, so on its
        // own an outfit can end up attached to a different NPC after a plugin is
        // added or removed. Derived fresh here rather than trusting a cached value,
        // so it is always consistent with the ID beside it.
        std::string actorFormKey = outfit.actorFormKey;
        if (actorFormKey.empty()) {
            if (auto* actorForm = RE::TESForm::LookupByID(key.actorRefID)) {
                actorFormKey = Persistence::FormKeyUtil::BuildFormKey(actorForm);
            }
        }
        std::uint32_t actorKeyLen = static_cast<std::uint32_t>(actorFormKey.size());
        a_intfc->WriteRecordData(&actorKeyLen, sizeof(actorKeyLen));
        if (actorKeyLen > 0) {
            a_intfc->WriteRecordData(actorFormKey.data(), actorKeyLen);
        }

        spdlog::debug("  - Saved outfit '{}' for actor 0x{:08X} ('{}') with {} items",
            key.outfitName, key.actorRefID,
            actorFormKey.empty() ? "dynamic/unknown" : actorFormKey, itemCount);
    }

    // === Save Player Given Items ===
    if (!a_intfc->OpenRecord(kPlayerItemsRecord, kSerializationVersion)) {
        spdlog::error("OutfitLockManager::OnGameSave - Failed to open player items record");
        return;
    }

    // Write number of actors with player items
    std::uint32_t actorCount = static_cast<std::uint32_t>(mgr->m_playerGivenItems.size());
    a_intfc->WriteRecordData(&actorCount, sizeof(actorCount));

    for (const auto& [actorID, items] : mgr->m_playerGivenItems) {
        // Write actor ref ID
        a_intfc->WriteRecordData(&actorID, sizeof(actorID));

        // Write number of items
        std::uint32_t itemCount = static_cast<std::uint32_t>(items.size());
        a_intfc->WriteRecordData(&itemCount, sizeof(itemCount));

        // Write each item FormID
        for (RE::FormID itemID : items) {
            a_intfc->WriteRecordData(&itemID, sizeof(itemID));
        }

        spdlog::debug("  - Saved {} player items for actor 0x{:08X}", itemCount, actorID);
    }

    spdlog::debug("OutfitLockManager::OnGameSave - Done");
}

void OutfitLockManager::OnPreLoad()
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    mgr->m_outfits.clear();
    mgr->m_playerGivenItems.clear();
    mgr->RefreshLockedCount();

    spdlog::debug("OutfitLockManager::OnPreLoad - Cleared state");
}

void OutfitLockManager::OnLoadRecord(SKSE::SerializationInterface* a_intfc,
    std::uint32_t type, std::uint32_t version, std::uint32_t length)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    if (type == kOutfitRecord) {
            // v4 and v5 are both readable: v5 only appends a field per outfit, so an
            // older co-save loads fine and simply carries no actorFormKey.
            if (version < kMinReadableVersion || version > kSerializationVersion) {
                spdlog::warn("OutfitLockManager::OnLoadRecord - Incompatible outfit version {} (readable range {}..{}), skipping",
                    version, kMinReadableVersion, kSerializationVersion);
                // Skip by consuming remaining bytes (already read type/version/length)
                if (length > 0) {
                    std::vector<char> skipBuffer(length);
                    a_intfc->ReadRecordData(skipBuffer.data(), length);
                }
                return;
            }

            // Read number of outfits
            std::uint32_t outfitCount = 0;
            a_intfc->ReadRecordData(&outfitCount, sizeof(outfitCount));

            for (std::uint32_t i = 0; i < outfitCount; ++i) {
                // Read actor ref ID (still uses SKSE resolution for NPCs)
                RE::FormID oldActorID = 0;
                a_intfc->ReadRecordData(&oldActorID, sizeof(oldActorID));

                RE::FormID newActorID = 0;
                const bool skseResolved = a_intfc->ResolveFormID(oldActorID, newActorID);

                // The record still has to be consumed in full even when we intend to
                // drop it, or every subsequent outfit reads from the wrong offset.
                // Read first, decide afterwards.
                std::uint32_t nameLen = 0;
                a_intfc->ReadRecordData(&nameLen, sizeof(nameLen));
                std::string outfitName(nameLen, '\0');
                a_intfc->ReadRecordData(outfitName.data(), nameLen);

                std::uint32_t itemCount = 0;
                a_intfc->ReadRecordData(&itemCount, sizeof(itemCount));

                SavedOutfit outfit;
                for (std::uint32_t j = 0; j < itemCount; ++j) {
                    std::uint32_t keyLen = 0;
                    a_intfc->ReadRecordData(&keyLen, sizeof(keyLen));
                    std::string formKey(keyLen, '\0');
                    a_intfc->ReadRecordData(formKey.data(), keyLen);

                    // FormKey resolution happens lazily in IsValid()/GetArmor().
                    SavedArmorItem item;
                    item.formKey = formKey;
                    outfit.items.push_back(item);
                    spdlog::trace("    - Loaded armor key: {}", formKey);
                }

                if (version >= 5) {
                    std::uint32_t actorKeyLen = 0;
                    a_intfc->ReadRecordData(&actorKeyLen, sizeof(actorKeyLen));
                    if (actorKeyLen > 0) {
                        outfit.actorFormKey.resize(actorKeyLen);
                        a_intfc->ReadRecordData(outfit.actorFormKey.data(), actorKeyLen);
                    }
                }

                // Prefer the FormKey over SKSE's ref-ID resolution. SKSE can only
                // remap an ID whose *plugin* is still at a known index; the FormKey
                // carries the plugin name, so it survives reordering that the raw ID
                // does not.
                if (!outfit.actorFormKey.empty()) {
                    const RE::FormID fromKey =
                        Persistence::FormKeyUtil::ResolveToRuntimeFormID(outfit.actorFormKey);
                    if (fromKey != 0) {
                        if (skseResolved && fromKey != newActorID) {
                            spdlog::debug(
                                "  - Actor key '{}' resolves to 0x{:08X}, SKSE said 0x{:08X}; "
                                "trusting the FormKey",
                                outfit.actorFormKey, fromKey, newActorID);
                        }
                        newActorID = fromKey;
                    }
                }

                if (newActorID == 0) {
                    spdlog::warn("  - Cannot resolve actor 0x{:08X} ('{}'), dropping outfit '{}'",
                        oldActorID, outfit.actorFormKey, outfitName);
                    continue;
                }

                // Where an unlock used to park the outfit it had been defending, for the
                // lock button to offer back. Neither exists any more; saved outfits took
                // over that job. Read in full above, dropped here.
                if (outfitName == "preunlock") {
                    spdlog::debug("  - Dropping parked pre-unlock outfit for actor 0x{:08X}", newActorID);
                    continue;
                }

                const std::uint32_t validCount = static_cast<std::uint32_t>(outfit.items.size());
                OutfitKey key{newActorID, outfitName};
                mgr->m_outfits[key] = std::move(outfit);

                spdlog::debug("  - Loaded outfit '{}' for actor 0x{:08X} with {} items",
                    outfitName, newActorID, validCount);
            }

            mgr->RefreshLockedCount();
        }
        else if (type == kPlayerItemsRecord) {
            // Read number of actors
            std::uint32_t actorCount = 0;
            a_intfc->ReadRecordData(&actorCount, sizeof(actorCount));

            for (std::uint32_t i = 0; i < actorCount; ++i) {
                // Read actor ref ID
                RE::FormID oldActorID = 0;
                a_intfc->ReadRecordData(&oldActorID, sizeof(oldActorID));

                // Read number of items
                std::uint32_t itemCount = 0;
                a_intfc->ReadRecordData(&itemCount, sizeof(itemCount));

                // Resolve actor form ID
                RE::FormID newActorID = 0;
                if (!a_intfc->ResolveFormID(oldActorID, newActorID)) {
                    spdlog::warn("  - Cannot resolve actor 0x{:08X}, skipping player items", oldActorID);
                    // Skip items
                    for (std::uint32_t j = 0; j < itemCount; ++j) {
                        RE::FormID dummyID = 0;
                        a_intfc->ReadRecordData(&dummyID, sizeof(dummyID));
                    }
                    continue;
                }

                std::unordered_set<RE::FormID> items;
                std::uint32_t validCount = 0;

                for (std::uint32_t j = 0; j < itemCount; ++j) {
                    RE::FormID oldItemID = 0;
                    a_intfc->ReadRecordData(&oldItemID, sizeof(oldItemID));

                    RE::FormID newItemID = 0;
                    if (a_intfc->ResolveFormID(oldItemID, newItemID)) {
                        items.insert(newItemID);
                        ++validCount;
                    } else {
                        spdlog::warn("    - Cannot resolve item 0x{:08X}", oldItemID);
                    }
                }

                if (!items.empty()) {
                    mgr->m_playerGivenItems[newActorID] = std::move(items);
                    spdlog::debug("  - Loaded {} player items for actor 0x{:08X}", validCount, newActorID);
                }
            }
        }
    else {
        // Unknown record type — skip its data
        if (length > 0) {
            std::vector<char> skipBuffer(length);
            a_intfc->ReadRecordData(skipBuffer.data(), length);
        }
        spdlog::warn("OutfitLockManager::OnLoadRecord - Unknown record type: {:08X}, skipped {} bytes", type, length);
    }
}

void OutfitLockManager::OnGameLoad(SKSE::SerializationInterface* a_intfc)
{
    OnPreLoad();

    spdlog::debug("OutfitLockManager::OnGameLoad - Loading data");

    std::uint32_t type, version, length;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        OnLoadRecord(a_intfc, type, version, length);
    }

    auto* mgr = GetSingleton();
    spdlog::info("OutfitLockManager::OnGameLoad - Loaded {} outfits, {} actors with player items",
        mgr->m_outfits.size(), mgr->m_playerGivenItems.size());
}

void OutfitLockManager::OnRevert(SKSE::SerializationInterface*)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    spdlog::info("OutfitLockManager::OnRevert - Clearing {} outfits, {} actors with player items, {} actors with gallery items",
        mgr->m_outfits.size(), mgr->m_playerGivenItems.size(), mgr->m_gallerySpawnedItems.size());
    mgr->m_outfits.clear();
    mgr->m_playerGivenItems.clear();
    mgr->m_gallerySpawnedItems.clear();
    mgr->m_hasGalleryItems.store(false, std::memory_order_relaxed);
    mgr->RefreshLockedCount();

    // Rate and debounce state is keyed on runtime ref IDs, which mean nothing across a
    // revert. Left in place they would suppress a reapply for whoever inherits the ID.
    {
        std::lock_guard<std::mutex> rateLock(mgr->m_equipRateMutex);
        mgr->m_equipRate.clear();
    }
    {
        std::lock_guard<std::mutex> applyLock(mgr->m_lastApplyMutex);
        mgr->m_lastApplyMs.clear();
    }
    {
        std::lock_guard<std::mutex> reapplyLock(mgr->m_reapplyMutex);
        mgr->m_reapply.clear();
    }
}

void OutfitLockManager::OnPostLoadGame()
{
    spdlog::info("OutfitLockManager::OnPostLoadGame - Scanning for locked NPCs");

    // Outfit records carry their item list in memory only, so every one of them comes
    // back from a load empty. Rebuild before anything reads them.
    OutfitFormBackend::GetSingleton()->ReapplyAll();

    // On post load game, try to get player's current location
    auto* player = RE::PlayerCharacter::GetSingleton();
    RE::BGSLocation* playerLoc = nullptr;
    if (player) {
        playerLoc = player->GetCurrentLocation();
    }

    ApplyLockedOutfitsInLocation(playerLoc);
}

void OutfitLockManager::ApplyLockedOutfitsInLocation(RE::BGSLocation* location)
{
    std::string locName = "Unknown";
    if (location && location->fullName.c_str()) {
        locName = location->fullName.c_str();
    }

    spdlog::debug("OutfitLockManager::ApplyLockedOutfitsInLocation - Scanning location '{}'", locName);

    // Collect actors to process while holding the lock
    std::vector<RE::Actor*> actorsToProcess;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_outfits.empty()) {
            spdlog::trace("OutfitLockManager::ApplyLockedOutfitsInLocation - No outfits stored");
            return;
        }

        for (const auto& [key, outfit] : m_outfits) {
            if (key.outfitName != "locked") continue;

            // Look up the actor
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(key.actorRefID);
            if (!actor) {
                spdlog::trace("  - Actor 0x{:08X} not found", key.actorRefID);
                continue;
            }

            // Check if actor is in the same location
            auto* actorLoc = actor->GetCurrentLocation();
            if (location && actorLoc != location) {
                spdlog::trace("  - Actor '{}' is in different location", actor->GetName());
                continue;
            }

            // Dressing an actor whose 3D is not loaded still costs the full equip fan-out
            // in every listening mod, and the result is invisible until they load anyway.
            // Previously only checked when no location was given.
            if (!actor->Is3DLoaded()) {
                spdlog::trace("  - Actor '{}' is not 3D loaded", actor->GetName());
                continue;
            }

            spdlog::debug("  - Found locked actor '{}' in location",
                actor->GetName());
            actorsToProcess.push_back(actor);
        }
    }  // Lock released here

    const std::int64_t now = NowMs();

    // Apply outfits outside the lock to avoid deadlock
    for (auto* actor : actorsToProcess) {
        const RE::FormID actorID = actor->GetFormID();

        // Location changes arrive in bursts - a door transition can fire several, and
        // fast travel more - and a full reapply per event is how two locked followers
        // turn into a few dozen equip events for no visible change.
        {
            std::lock_guard<std::mutex> lock(m_lastApplyMutex);
            const auto it = m_lastApplyMs.find(actorID);
            if (it != m_lastApplyMs.end() && now - it->second < kLocationDebounceMs) {
                spdlog::trace("  - '{}' reapplied {}ms ago, skipping (debounce)",
                    actor->GetName(), now - it->second);
                continue;
            }
        }

        // Someone else is already cycling this actor's equipment. Adding our own
        // unequip/equip traffic on top only feeds the Papyrus stack pile-up.
        if (IsInEquipBackoff(actorID)) {
            spdlog::warn("  - '{}' is in equip-storm backoff, not reapplying", actor->GetName());
            continue;
        }

        // Under SeverActions the unequip half of this would trip its alias debounce
        // and have it re-dress the NPC on top of us. Its own apply path brackets
        // itself the same way.
        SeverActionsCompat::SuspendLock(actor);
        ApplyOutfit(actor, "locked", true);
        SeverActionsCompat::ResumeLockAfterMs(actor, SeverActionsCompat::kEquipSettleMs);
    }
}

// =============================================================================
// Interface002 support: enumeration and FormKey injection
// =============================================================================
//
// These are the surface Save Migration drives. They deal only in FormKeys, so an
// outfit set can be read out of one savegame and written into another without
// either side touching a runtime form ID.

std::vector<std::string> OutfitLockManager::EnumerateOutfitNames(RE::Actor* actor) const
{
    std::vector<std::string> names;
    if (!actor) {
        return names;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const RE::FormID actorID = actor->GetFormID();
    for (const auto& [key, outfit] : m_outfits) {
        if (key.actorRefID == actorID) {
            names.push_back(key.outfitName);
        }
    }
    return names;
}

std::vector<std::string> OutfitLockManager::GetOutfitItemFormKeys(RE::Actor* actor,
    const std::string& outfitName) const
{
    std::vector<std::string> keys;
    if (!actor) {
        return keys;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_outfits.find(OutfitKey{actor->GetFormID(), outfitName});
    if (it == m_outfits.end()) {
        return keys;
    }
    keys.reserve(it->second.items.size());
    for (const auto& item : it->second.items) {
        keys.push_back(item.formKey);
    }
    return keys;
}

std::vector<std::string> OutfitLockManager::GetPlayerGivenFormKeys(RE::Actor* actor) const
{
    std::vector<std::string> keys;
    if (!actor) {
        return keys;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_playerGivenItems.find(actor->GetFormID());
    if (it == m_playerGivenItems.end()) {
        return keys;
    }
    // The set holds runtime FormIDs; converting to FormKeys here is what makes the
    // list meaningful outside this session.
    for (const RE::FormID itemID : it->second) {
        if (auto* form = RE::TESForm::LookupByID(itemID)) {
            auto key = Persistence::FormKeyUtil::BuildFormKey(form);
            if (!key.empty()) {
                keys.push_back(std::move(key));
            }
        }
    }
    return keys;
}

std::uint32_t OutfitLockManager::SetOutfitFromFormKeys(RE::Actor* actor,
    const std::string& outfitName, const std::vector<std::string>& formKeys)
{
    if (!actor || outfitName.empty()) {
        return 0;
    }

    SavedOutfit outfit;
    outfit.actorFormKey = Persistence::FormKeyUtil::BuildFormKey(actor);

    std::uint32_t accepted = 0;
    for (const auto& formKey : formKeys) {
        if (formKey.empty()) {
            continue;
        }
        // Verify the key resolves to an actual armour piece before storing it.
        // Storing an unresolvable key would be pruned on the first apply anyway, and
        // the pruning rewrites the map - so it is cheaper and safer to reject here.
        const RE::FormID runtimeID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey);
        if (runtimeID == 0 || !RE::TESForm::LookupByID<RE::TESObjectARMO>(runtimeID)) {
            spdlog::warn("SetOutfitFromFormKeys - '{}' does not resolve to armour here, skipping",
                formKey);
            continue;
        }
        SavedArmorItem item;
        item.formKey = formKey;
        outfit.items.push_back(std::move(item));
        ++accepted;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outfits[OutfitKey{actor->GetFormID(), outfitName}] = std::move(outfit);
        RefreshLockedCount();
    }

    spdlog::info("SetOutfitFromFormKeys - '{}' for 0x{:08X}: {}/{} key(s) accepted",
        outfitName, actor->GetFormID(), accepted, formKeys.size());
    return accepted;
}

std::uint32_t OutfitLockManager::MarkPlayerGivenFromFormKeys(RE::Actor* actor,
    const std::vector<std::string>& formKeys)
{
    if (!actor) {
        return 0;
    }
    std::uint32_t accepted = 0;
    for (const auto& formKey : formKeys) {
        const RE::FormID runtimeID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey);
        if (runtimeID == 0) {
            continue;
        }
        MarkItemAsPlayerGiven(actor, runtimeID);
        ++accepted;
    }
    return accepted;
}

std::uint32_t OutfitLockManager::EnsureOutfitItemsInInventory(RE::Actor* actor,
    const std::string& outfitName)
{
    if (!actor) {
        return 0;
    }

    // Snapshot the keys under the lock, then add outside it: AddObjectToContainer can
    // fire equip events that re-enter this manager.
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_outfits.find(OutfitKey{actor->GetFormID(), outfitName});
        if (it == m_outfits.end()) {
            return 0;
        }
        for (const auto& item : it->second.items) {
            keys.push_back(item.formKey);
        }
    }

    std::uint32_t added = 0;
    for (const auto& formKey : keys) {
        auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(
            Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey));
        if (!armor || ItemEquipHelper::IsBodyPart(armor)) {
            continue;
        }
        const auto counts = actor->GetInventoryCounts();
        const auto found = counts.find(static_cast<RE::TESBoundObject*>(armor));
        if (found != counts.end() && found->second > 0) {
            continue;  // already has it
        }
        actor->AddObjectToContainer(armor, nullptr, 1, nullptr);
        ++added;
        spdlog::debug("EnsureOutfitItemsInInventory - added '{}' ({}) to 0x{:08X}",
            armor->GetFullName(), formKey, actor->GetFormID());
    }

    if (added > 0) {
        spdlog::info("EnsureOutfitItemsInInventory - '{}': {} item(s) added. Without this, "
            "ApplyOutfit would prune them from the stored outfit permanently.",
            outfitName, added);
    }
    return added;
}

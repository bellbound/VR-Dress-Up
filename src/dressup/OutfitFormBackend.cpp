#include "OutfitFormBackend.h"
#include "OutfitLockManager.h"
#include "PapyrusBridge.h"
#include "SeverActionsCompat.h"
#include "FormKeyUtil.h"
#include "../settings.h"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

namespace
{
    std::int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void OutfitFormBackend::Initialize()
{
    if (m_initialized) {
        spdlog::warn("OutfitFormBackend::Initialize - Already initialized");
        return;
    }
    m_initialized = true;

    if (!Settings::GetSingleton()->IsOutfitBackendEnabled()) {
        spdlog::info("OutfitFormBackend::Initialize - Disabled by bUseOutfitBackend");
        return;
    }

    ResolveNativePool();
}

void OutfitFormBackend::ResolveNativePool()
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        spdlog::error("OutfitFormBackend::ResolveNativePool - No TESDataHandler");
        return;
    }

    if (!dataHandler->LookupModByName(kPluginName)) {
        spdlog::warn("OutfitFormBackend::ResolveNativePool - '{}' is not in the load order, "
            "SPID handoff disabled", kPluginName);
        return;
    }

    // Resolved through LookupForm rather than a hand-built FormID. VRDressUp.esp is
    // ESL-flagged, and synthesising 0xFE000000 | (idx << 12) does not work on VR -
    // see the note in FormKeyUtil::ResolveToRuntimeFormID.
    if (auto* blank = dataHandler->LookupForm(kBlankOutfitID, kPluginName)) {
        m_blankOutfit = blank->As<RE::BGSOutfit>();
    }

    m_pool.clear();
    m_pool.reserve(kPoolSize);
    for (std::size_t i = 0; i < kPoolSize; ++i) {
        auto* form = dataHandler->LookupForm(
            kFirstOutfitID + static_cast<RE::FormID>(i), kPluginName);
        auto* outfit = form ? form->As<RE::BGSOutfit>() : nullptr;
        if (!outfit) {
            spdlog::warn("OutfitFormBackend::ResolveNativePool - Missing outfit record #{}", i);
            break;
        }
        m_pool.push_back(outfit);
    }

    spdlog::info("OutfitFormBackend::ResolveNativePool - Indexed {}/{} outfit records from '{}'",
        m_pool.size(), kPoolSize, kPluginName);
}

bool OutfitFormBackend::IsAvailable() const
{
    // SeverActions lends us its own records, so its presence is enough even when our
    // own plugin is missing.
    return !m_pool.empty() || SeverActionsCompat::IsActive();
}

void OutfitFormBackend::SetExternalOutfit(RE::Actor* actor, RE::BGSOutfit* outfit)
{
    if (!actor) return;

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto* assignment = AssignLocked(actorKey);
    if (!assignment) return;

    assignment->externalOutfit = outfit;
    assignment->usesExternal = (outfit != nullptr);

    // Force the next Apply through: the record changed even if the item set did not.
    assignment->lastApplied.clear();
}

bool OutfitFormBackend::IsEligible(RE::Actor* actor) const
{
    if (!actor || actor->IsPlayerRef()) return false;

    auto* npc = actor->GetActorBase();
    if (!npc) return false;

    if (Settings::GetSingleton()->IsOutfitBackendUniqueOnly() && !npc->IsUnique()) {
        spdlog::info("OutfitFormBackend::IsEligible - '{}' has a shared base record, skipping "
            "(every copy of that NPC would change with it)", actor->GetName());
        return false;
    }

    return true;
}

RE::BGSOutfit* OutfitFormBackend::OutfitForIndex(std::size_t index) const
{
    return index < m_pool.size() ? m_pool[index] : nullptr;
}

OutfitFormBackend::Assignment* OutfitFormBackend::AssignLocked(const std::string& actorKey)
{
    auto it = m_assignments.find(actorKey);
    if (it != m_assignments.end()) {
        return &it->second;
    }

    std::vector<bool> taken(m_pool.size(), false);
    for (const auto& [key, assignment] : m_assignments) {
        if (assignment.poolIndex < taken.size()) {
            taken[assignment.poolIndex] = true;
        }
    }

    Assignment fresh;
    fresh.poolIndex = kNoPoolSlot;
    for (std::size_t i = 0; i < taken.size(); ++i) {
        if (!taken[i]) {
            fresh.poolIndex = i;
            break;
        }
    }

    if (fresh.poolIndex == kNoPoolSlot && !m_pool.empty()) {
        // Not fatal on its own: under SeverActions the record comes from there, and an
        // entry with no pool slot is still needed to hold it.
        spdlog::warn("OutfitFormBackend::AssignLocked - Outfit pool exhausted ({} slots)",
            m_pool.size());
    }

    return &m_assignments.emplace(actorKey, std::move(fresh)).first->second;
}

bool OutfitFormBackend::Fill(RE::BGSOutfit* outfit, const std::vector<std::string>& formKeys) const
{
    if (!outfit) return false;

    outfit->outfitItems.clear();

    for (const auto& formKey : formKeys) {
        const RE::FormID runtimeID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(formKey);
        if (runtimeID == 0) continue;

        auto* form = RE::TESForm::LookupByID(runtimeID);
        if (!form) continue;

        outfit->outfitItems.push_back(form);
    }

    spdlog::trace("OutfitFormBackend::Fill - '{}' now holds {}/{} item(s)",
        outfit->GetFormEditorID(), outfit->outfitItems.size(), formKeys.size());

    return !outfit->outfitItems.empty();
}

bool OutfitFormBackend::IsApplying() const
{
    return NowMs() < m_applyUntilMs.load(std::memory_order_relaxed);
}

void OutfitFormBackend::DispatchSetOutfit(RE::Actor* actor, RE::BGSOutfit* outfit)
{
    SeverActionsCompat::SuspendLock(actor);

    m_applyUntilMs.store(NowMs() + kApplySettleMs, std::memory_order_relaxed);
    PapyrusBridge::CallActorSetOutfit(actor, outfit, false);

    SeverActionsCompat::ResumeLock(actor);
}

bool OutfitFormBackend::Apply(RE::Actor* actor, const std::vector<std::string>& formKeys)
{
    if (!IsAvailable() || !IsEligible(actor)) return false;

    if (formKeys.empty()) {
        spdlog::info("OutfitFormBackend::Apply - '{}' has an empty outfit, not assigning one",
            actor->GetName());
        return false;
    }

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) {
        spdlog::warn("OutfitFormBackend::Apply - '{}' has no source file (dynamic actor?), skipping",
            actor->GetName());
        return false;
    }

    std::vector<std::string> sortedKeys = formKeys;
    std::sort(sortedKeys.begin(), sortedKeys.end());

    auto* npc = actor->GetActorBase();
    RE::BGSOutfit* outfit = nullptr;
    bool needsDispatch = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* assignment = AssignLocked(actorKey);
        if (!assignment) return false;

        outfit = assignment->usesExternal ? assignment->externalOutfit
                                          : OutfitForIndex(assignment->poolIndex);
        if (!outfit) {
            spdlog::warn("OutfitFormBackend::Apply - No outfit record available for '{}'",
                actor->GetName());
            return false;
        }

        if (assignment->originalOutfitKey.empty() && npc->defaultOutfit &&
            npc->defaultOutfit != outfit) {
            assignment->originalOutfitKey =
                Persistence::FormKeyUtil::BuildFormKey(npc->defaultOutfit);
        }

        // Nothing to do when the item set is unchanged AND the actor is already wearing
        // our record. Without this the UnequipAll that SetOutfit queues re-enters
        // through the equip sink and we dispatch two or three times per click.
        needsDispatch = (assignment->lastApplied != sortedKeys) || (npc->defaultOutfit != outfit);
        assignment->lastApplied = sortedKeys;
    }

    Fill(outfit, formKeys);

    if (!needsDispatch) {
        spdlog::trace("OutfitFormBackend::Apply - '{}' unchanged, skipping dispatch", actor->GetName());
        return true;
    }

    spdlog::info("OutfitFormBackend::Apply - Assigning '{}' ({} item(s)) to '{}' (0x{:08X})",
        outfit->GetFormEditorID(), outfit->outfitItems.size(),
        actor->GetName(), actor->GetFormID());

    DispatchSetOutfit(actor, outfit);
    return true;
}

bool OutfitFormBackend::Restore(RE::Actor* actor)
{
    if (!IsAvailable() || !actor) return false;

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) return false;

    std::string originalKey;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_assignments.find(actorKey);
        if (it == m_assignments.end()) return false;
        originalKey = it->second.originalOutfitKey;
        m_assignments.erase(it);
    }

    RE::BGSOutfit* original = nullptr;
    if (!originalKey.empty()) {
        const RE::FormID runtimeID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(originalKey);
        if (auto* form = RE::TESForm::LookupByID(runtimeID)) {
            original = form->As<RE::BGSOutfit>();
        }
    }

    if (!original) {
        // Nothing sensible to hand back. Leaving our record in place keeps the NPC
        // dressed; blanking it would strip them.
        spdlog::info("OutfitFormBackend::Restore - No original outfit recorded for '{}', "
            "leaving the current one in place", actor->GetName());
        return false;
    }

    spdlog::info("OutfitFormBackend::Restore - Returning '{}' to outfit '{}'",
        actor->GetName(), original->GetFormEditorID());

    DispatchSetOutfit(actor, original);
    return true;
}

void OutfitFormBackend::ReapplyAll()
{
    if (!IsAvailable()) return;

    struct Pending
    {
        RE::Actor*               actor;
        RE::BGSOutfit*           outfit;
        std::vector<std::string> keys;
    };

    std::vector<Pending>    pending;
    std::vector<RE::Actor*> needsReacquire;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [actorKey, assignment] : m_assignments) {
            const RE::FormID actorID = Persistence::FormKeyUtil::ResolveToRuntimeFormID(actorKey);
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorID);
            if (!actor) continue;

            auto* outfit = assignment.usesExternal ? assignment.externalOutfit
                                                   : OutfitForIndex(assignment.poolIndex);
            if (!outfit) {
                // A borrowed record does not survive a load; go and ask for it again.
                if (assignment.usesExternal) {
                    needsReacquire.push_back(actor);
                }
                continue;
            }

            pending.push_back({actor, outfit, assignment.lastApplied});
        }
    }

    spdlog::info("OutfitFormBackend::ReapplyAll - Rebuilding {} outfit record(s)", pending.size());

    for (auto& entry : pending) {
        // outfitItems is memory-only, so every record comes back from a load empty.
        Fill(entry.outfit, entry.keys);

        auto* npc = entry.actor->GetActorBase();
        if (npc && npc->defaultOutfit == entry.outfit) {
            continue;  // still assigned, nothing to dispatch
        }

        spdlog::info("  - Re-assigning '{}' to '{}'",
            entry.outfit->GetFormEditorID(), entry.actor->GetName());
        DispatchSetOutfit(entry.actor, entry.outfit);
    }

    for (auto* actor : needsReacquire) {
        spdlog::info("  - Re-acquiring SeverActions outfit record for '{}'", actor->GetName());
        ReacquireExternal(actor);
    }
}

void OutfitFormBackend::ReacquireExternal(RE::Actor* actor)
{
    if (!actor) return;

    const RE::FormID actorID = actor->GetFormID();
    SeverActionsCompat::AcquirePresetOutfit(actor, [actorID](RE::BGSOutfit* outfit) {
        auto* target = RE::TESForm::LookupByID<RE::Actor>(actorID);
        if (!target) return;

        auto* backend = OutfitFormBackend::GetSingleton();
        auto* lockMgr = OutfitLockManager::GetSingleton();

        if (outfit) {
            backend->SetExternalOutfit(target, outfit);
        } else {
            // SeverActions did not produce one. Fall back to our own pool rather than
            // leaving the actor unprotected.
            backend->ClearExternalOutfit(target);
        }

        backend->Apply(target, lockMgr->GetOutfitItemFormKeys(target, "locked"));
    });
}

bool OutfitFormBackend::HasOutfitRecord(RE::Actor* actor) const
{
    if (!actor) return false;

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_assignments.find(actorKey);
    if (it == m_assignments.end()) return false;

    return it->second.usesExternal ? it->second.externalOutfit != nullptr
                                   : it->second.poolIndex != kNoPoolSlot;
}

void OutfitFormBackend::ClearExternalOutfit(RE::Actor* actor)
{
    if (!actor) return;

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_assignments.find(actorKey);
    if (it == m_assignments.end()) return;

    it->second.externalOutfit = nullptr;
    it->second.usesExternal = false;
    it->second.lastApplied.clear();
}

void OutfitFormBackend::Forget(RE::Actor* actor)
{
    if (!actor) return;

    const std::string actorKey = Persistence::FormKeyUtil::BuildFormKey(actor);
    if (actorKey.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_assignments.erase(actorKey);
}

// =============================================================================
// Serialization
// =============================================================================

void OutfitFormBackend::OnGameSave(SKSE::SerializationInterface* a_intfc)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    if (!a_intfc->OpenRecord(kRecord, kSerializationVersion)) {
        spdlog::error("OutfitFormBackend::OnGameSave - Failed to open record");
        return;
    }

    std::uint32_t count = static_cast<std::uint32_t>(mgr->m_assignments.size());
    a_intfc->WriteRecordData(&count, sizeof(count));

    const auto writeString = [a_intfc](const std::string& value) {
        std::uint32_t len = static_cast<std::uint32_t>(value.size());
        a_intfc->WriteRecordData(&len, sizeof(len));
        if (len > 0) {
            a_intfc->WriteRecordData(value.data(), len);
        }
    };

    for (const auto& [actorKey, assignment] : mgr->m_assignments) {
        writeString(actorKey);

        std::uint32_t poolIndex = static_cast<std::uint32_t>(assignment.poolIndex);
        a_intfc->WriteRecordData(&poolIndex, sizeof(poolIndex));

        writeString(assignment.originalOutfitKey);

        std::uint32_t itemCount = static_cast<std::uint32_t>(assignment.lastApplied.size());
        a_intfc->WriteRecordData(&itemCount, sizeof(itemCount));
        for (const auto& itemKey : assignment.lastApplied) {
            writeString(itemKey);
        }

        // v2: the record itself is not saved - the provider hands it back on request -
        // but a load has to know to go and ask.
        std::uint8_t usesExternal = assignment.usesExternal ? 1 : 0;
        a_intfc->WriteRecordData(&usesExternal, sizeof(usesExternal));
    }

    spdlog::info("OutfitFormBackend::OnGameSave - Saved {} outfit assignment(s)", count);
}

void OutfitFormBackend::OnPreLoad()
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);
    mgr->m_assignments.clear();
}

void OutfitFormBackend::OnLoadRecord(SKSE::SerializationInterface* a_intfc,
    std::uint32_t type, std::uint32_t version, std::uint32_t length)
{
    if (type != kRecord) return;

    // v1 records load fine: v2 only appends a field per assignment, so an older co-save
    // simply carries no borrowed-record flag.
    if (version < kMinReadableVersion || version > kSerializationVersion) {
        spdlog::warn("OutfitFormBackend::OnLoadRecord - Incompatible version {} (readable range {}..{}), skipping",
            version, kMinReadableVersion, kSerializationVersion);
        if (length > 0) {
            std::vector<char> skipBuffer(length);
            a_intfc->ReadRecordData(skipBuffer.data(), length);
        }
        return;
    }

    const auto readString = [a_intfc](std::string& out) {
        std::uint32_t len = 0;
        a_intfc->ReadRecordData(&len, sizeof(len));
        out.clear();
        if (len > 0) {
            out.resize(len);
            a_intfc->ReadRecordData(out.data(), len);
        }
    };

    std::uint32_t count = 0;
    a_intfc->ReadRecordData(&count, sizeof(count));

    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);

    for (std::uint32_t i = 0; i < count; ++i) {
        std::string actorKey;
        readString(actorKey);

        Assignment assignment;

        std::uint32_t poolIndex = 0;
        a_intfc->ReadRecordData(&poolIndex, sizeof(poolIndex));
        assignment.poolIndex = poolIndex;

        readString(assignment.originalOutfitKey);

        std::uint32_t itemCount = 0;
        a_intfc->ReadRecordData(&itemCount, sizeof(itemCount));
        assignment.lastApplied.reserve(itemCount);
        for (std::uint32_t j = 0; j < itemCount; ++j) {
            std::string itemKey;
            readString(itemKey);
            assignment.lastApplied.push_back(std::move(itemKey));
        }

        if (version >= 2) {
            std::uint8_t usesExternal = 0;
            a_intfc->ReadRecordData(&usesExternal, sizeof(usesExternal));
            assignment.usesExternal = usesExternal != 0;
        }

        // Read first, decide afterwards - the record has to be consumed in full even
        // when the entry is dropped, or every following one reads at the wrong offset.
        if (actorKey.empty()) {
            spdlog::warn("  - Dropping outfit assignment with no actor key");
            continue;
        }

        mgr->m_assignments.emplace(std::move(actorKey), std::move(assignment));
    }

    spdlog::info("OutfitFormBackend::OnLoadRecord - Loaded {}/{} outfit assignment(s)",
        mgr->m_assignments.size(), count);
}

void OutfitFormBackend::OnRevert(SKSE::SerializationInterface*)
{
    auto* mgr = GetSingleton();
    std::lock_guard<std::mutex> lock(mgr->m_mutex);
    mgr->m_assignments.clear();
}

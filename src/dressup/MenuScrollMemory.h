#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <mutex>
#include <string>
#include <unordered_map>

// Where each of the menu's scrolling containers was left, so coming back to a view returns
// to the place the player scrolled to rather than to the top of the list.
//
// The wheel and the two rows are each one container reused for every view they show: pick a
// different mod category and the same wheel is emptied and refilled, and until now the
// position went with it. On a category holding several hundred pieces that meant scrolling
// the whole way back every time the player looked at something else and returned.
//
// Positions are normalized 0..1 as 3DUI reports them, so they are relative to the content
// that was in the container when they were taken. Recalling one against a list that has
// since changed length lands proportionally rather than exactly - which is why the keys
// name the actor as well as the view: the categories are cached for the whole game but
// narrowed to whoever is being dressed, so the same category is a different list per actor.
//
// The memory lasts the game session and is deliberately not written to the cosave. It is a
// convenience for the browsing the player is doing right now, not state a save should carry;
// a position restored into a load whose inventory has moved on would be meaningless anyway.
class MenuScrollMemory
{
public:
    static MenuScrollMemory* GetSingleton()
    {
        static MenuScrollMemory instance;
        return &instance;
    }

    // Store where a view was left. An empty key is ignored, so callers can hand over the
    // key of a view that was never established without checking first.
    void Remember(const std::string& key, float position)
    {
        if (key.empty()) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_positions[key] = position;

        spdlog::trace("MenuScrollMemory: Remembered '{}' at {:.3f}", key, position);
    }

    // Where a view was left, or 0.0 - the top of the list - for one not seen before.
    float Recall(const std::string& key) const
    {
        if (key.empty()) return 0.0f;

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_positions.find(key);
        return it != m_positions.end() ? it->second : 0.0f;
    }

    // Drop everything. Called when a save is loaded: the positions belong to the session
    // that took them, and the actor FormIDs the keys are built from need not mean the same
    // thing - or anything - in the game being loaded.
    static void OnPreLoad()
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);
        mgr->m_positions.clear();
        spdlog::info("MenuScrollMemory: Cleared for load");
    }

    static void OnRevert(SKSE::SerializationInterface*)
    {
        auto* mgr = GetSingleton();
        std::lock_guard<std::mutex> lock(mgr->m_mutex);
        mgr->m_positions.clear();
        spdlog::info("MenuScrollMemory: Cleared on revert");
    }

private:
    MenuScrollMemory() = default;
    ~MenuScrollMemory() = default;
    MenuScrollMemory(const MenuScrollMemory&) = delete;
    MenuScrollMemory& operator=(const MenuScrollMemory&) = delete;

    std::unordered_map<std::string, float> m_positions;

    mutable std::mutex m_mutex;
};

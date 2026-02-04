#pragma once

#include <unordered_set>
#include <string>

// Menu checking utility adapted from activeragdoll
// Thanks to Shizof for this method of checking what menus are open
namespace MenuChecker
{
    // Event sink for menu open/close events
    class MenuEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuEventHandler* GetSingleton();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

    private:
        MenuEventHandler() = default;
        ~MenuEventHandler() = default;
        MenuEventHandler(const MenuEventHandler&) = delete;
        MenuEventHandler& operator=(const MenuEventHandler&) = delete;
    };

    // Register the event handler - call during DataLoaded
    void RegisterEventSink();

    // Returns true if any game-stopping menu is open
    bool IsGameStopped();

    // Returns true if a specific menu is open
    bool IsMenuOpen(const std::string& menuName);
}

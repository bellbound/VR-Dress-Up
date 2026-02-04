#include "MenuChecker.h"
#include "log.h"

// Menu checking utility adapted from activeragdoll
// Thanks to Shizof for this method of checking what menus are open
namespace MenuChecker
{
    // Menus that should stop input processing
    static const std::unordered_set<std::string> gameStoppingMenus = {
        "BarterMenu",
        "Book Menu",
        "Console",
        "Custom Menu",
        "Native UI Menu",
        "ContainerMenu",
        "Dialogue Menu",
        "Crafting Menu",
        "Credits Menu",
        "Debug Text Menu",
        "FavoritesMenu",
        "GiftMenu",
        "InventoryMenu",
        "Journal Menu",
        "Kinect Menu",
        "Loading Menu",
        "Lockpicking Menu",
        "MagicMenu",
        "Main Menu",
        "MapMarkerText3D",
        "MapMenu",
        "MessageBoxMenu",
        "Mist Menu",
        "Quantity Menu",
        "RaceSex Menu",
        "Sleep/Wait Menu",
        "StatsMenuSkillRing",
        "StatsMenuPerks",
        "Training Menu",
        "Tutorial Menu",
        "TweenMenu"
    };

    // Currently open menus
    static std::unordered_set<std::string> openMenus;

    MenuEventHandler* MenuEventHandler::GetSingleton()
    {
        static MenuEventHandler instance;
        return &instance;
    }

    RE::BSEventNotifyControl MenuEventHandler::ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        std::string menuName = a_event->menuName.c_str();

        if (a_event->opening) {
            openMenus.insert(menuName);
            spdlog::trace("MenuChecker: Menu opened - {}", menuName);
        } else {
            openMenus.erase(menuName);
            spdlog::trace("MenuChecker: Menu closed - {}", menuName);
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    void RegisterEventSink()
    {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(MenuEventHandler::GetSingleton());
            spdlog::info("MenuChecker: Registered menu event sink");
        } else {
            spdlog::error("MenuChecker: Failed to get UI singleton");
        }
    }

    bool IsGameStopped()
    {
        for (const auto& menu : openMenus) {
            if (gameStoppingMenus.contains(menu)) {
                return true;
            }
        }
        return false;
    }

    bool IsMenuOpen(const std::string& menuName)
    {
        return openMenus.contains(menuName);
    }
}

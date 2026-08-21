#pragma once

#include <cstdint>
#include "../api/ThreeDUIInterface001.h"
#include "MenuScale.h"

// The one place the menu's colours are written down.
//
// Every plate behind every orb - the wheel, the gallery row, the outfit row - takes its
// hue from here, so a change lands everywhere at once and no row can drift into its own
// palette. Nothing else in the mod names a colour.
//
// There are two schemes. The normal one is cool: grey plates, a blue highlight, and warm
// orange reserved for the pieces the actor has on. The editing one moves the same palette
// round the wheel into purple, and is switched on while edits are being written into a
// saved outfit - see DressupMenuManager::UpdateBackdropScheme. Editing is the one mode
// where a click changes something the player still has after they walk away, and the line
// of text saying so was easy to miss, so the whole menu shifts hue with it. Shifts, not
// shouts: it has to be unmissable when you look for it and invisible when you are not.
namespace Backdrop
{
    constexpr const char* kModel = "meshes\\3DUI\\gradient-background-sphere.nif";

    // Scales are absolute since 3DUI 0.10.6: the backdrop no longer inherits the fit
    // correction the element derives from its preview model's bounds, so one value is one
    // size for a whole row instead of one size per model. A unit sphere at scale S comes
    // out S/2 units across, so each of these is set just inside its row's spacing - a
    // backdrop wider than the gap between two elements reads as one smeared plate.
    //
    // These are the scale-1 sizes; Apply multiplies them by fMenuScale, which multiplies
    // the row spacings quoted beside them by exactly the same amount.
    constexpr float kCategoryScale = 20.0f;  // gallery row, 12.0 column spacing
    constexpr float kItemScale     = 14.0f;  // item spiral, 8.0 item spacing
    constexpr float kOutfitScale   = 16.0f;  // outfit row, 10.8 column spacing

    // rgb is the hue, a the opacity, glow the emissive strength. The mesh authors a bright
    // blue (0.24, 0.78, 1.0) at glow 2.4, which is far too loud for a whole row of plates,
    // so everything here is read against that rather than against a flat sRGB swatch.
    struct Tint
    {
        float r, g, b, a, glow;
    };

    // The five roles a plate can be in. Every scheme fills all five; nothing outside this
    // header decides what a role looks like.
    struct Palette
    {
        Tint plateIdle;      // a row entry nobody has picked
        Tint plateSelected;  // the category that is open, the outfit that is on
        Tint itemAvailable;  // wheel: not worn, so a click puts it on
        Tint itemEquipped;   // wheel: already on
        Tint armed;          // one more press and something is destroyed or discarded
    };

    // The cool scheme.
    //
    // plateIdle is the near-grey the skin overlay menu rests its pack covers on, there to
    // give the artwork an edge to sit against rather than to say "pick me". plateSelected
    // is one step up in the same direction rather than a different colour: the hue commits
    // to the mesh's blue and the glow roughly doubles, which picks it out of the row at a
    // glance without the row turning into a light show.
    //
    // In the wheel it is the *equipped* piece that carries the warm orange, and everything
    // still on offer that rests on plateIdle's grey. The wheel is a wheel of what this
    // actor is wearing, and the thing worth finding at a glance is what is already on them
    // - the rest is the wardrobe it was picked out of. It ran the other way round for a
    // while, on the reasoning that a click should light up what it would change; in the
    // headset that lit most of the spiral at once and the few pieces actually worn were
    // lost in it.
    //
    // itemAvailable shares plateIdle so a row of choices reads as one surface, with the
    // worn pieces standing off it.
    constexpr Palette kNormalPalette = {
        /* plateIdle     */ {0.55f, 0.62f, 0.72f, 1.00f, 1.00f},
        /* plateSelected */ {0.45f, 0.68f, 1.00f, 1.00f, 1.90f},
        /* itemAvailable */ {0.55f, 0.62f, 0.72f, 1.00f, 1.00f},
        /* itemEquipped  */ {1.00f, 0.72f, 0.42f, 1.00f, 1.70f},
        /* armed         */ {1.00f, 0.30f, 0.25f, 1.00f, 1.90f},
    };

    // The warm scheme, worn while a saved outfit is taking the edits.
    //
    // Same lightness, same glow, same three steps as the cool one - only the hue moves, off
    // the blue and round past red into purple. A fully saturated red-orange was the first
    // attempt and it read as an alarm: most of a row is resting plates, and the player is
    // looking at the armour in front of them rather than the disc behind it, so the resting
    // colour has to stay quiet. It has to be unmissable when you look for it and invisible
    // when you are not.
    //
    // itemEquipped is deliberately identical to the cool scheme's. Which pieces the actor
    // has on is the one thing that means the same in both modes, so it is the one plate
    // that should not move when the mode does - and leaving it put gives the shifted
    // colours around it something fixed to be read against.
    //
    // armed keeps its own red, further than its cool twin so it still separates from a row
    // that no longer leans blue. Danger does not get to drift with the theme.
    constexpr Palette kEditingPalette = {
        /* plateIdle     */ {0.70f, 0.58f, 0.68f, 1.00f, 1.00f},
        /* plateSelected */ {1.00f, 0.55f, 0.70f, 1.00f, 1.90f},
        /* itemAvailable */ {0.70f, 0.58f, 0.68f, 1.00f, 1.00f},
        /* itemEquipped  */ {1.00f, 0.72f, 0.42f, 1.00f, 1.70f},
        /* armed         */ {1.00f, 0.12f, 0.10f, 1.00f, 2.40f},
    };

    enum class Scheme : std::uint8_t { Normal, Editing };

    inline Scheme g_scheme = Scheme::Normal;

    inline const Palette& Current()
    {
        return g_scheme == Scheme::Editing ? kEditingPalette : kNormalPalette;
    }

    // True when the scheme actually moved, so the caller only repaints when it must.
    inline bool SetScheme(Scheme scheme)
    {
        if (g_scheme == scheme) return false;
        g_scheme = scheme;
        return true;
    }

    // The one place that puts a backdrop on an element, so every row agrees about which
    // mesh it is - and the one place fMenuScale reaches the plates, so a plate cannot grow
    // out of step with the row spacing it was set just inside of.
    inline void Apply(P3DUI::Element* element, float scale, const Tint& tint)
    {
        if (!element) return;
        element->SetBackgroundModel(kModel);
        element->SetBackgroundScale(MenuScale::Scaled(scale));
        element->SetBackgroundColor(tint.r, tint.g, tint.b, tint.a, tint.glow);
    }

    inline void ApplyCategory(P3DUI::Element* element, bool selected)
    {
        const Palette& p = Current();
        Apply(element, kCategoryScale, selected ? p.plateSelected : p.plateIdle);
    }

    inline void ApplyItem(P3DUI::Element* element, bool equipped = false)
    {
        const Palette& p = Current();
        Apply(element, kItemScale, equipped ? p.itemEquipped : p.itemAvailable);
    }

    // The outfit row's plates share the gallery row's idle/selected pair on a smaller plate.
    enum class OutfitPlate : std::uint8_t { Idle, Worn, Armed };

    inline void ApplyOutfitPlate(P3DUI::Element* element, OutfitPlate state)
    {
        const Palette& p = Current();
        Apply(element, kOutfitScale,
            state == OutfitPlate::Armed ? p.armed
            : state == OutfitPlate::Worn ? p.plateSelected
                                         : p.plateIdle);
    }
}

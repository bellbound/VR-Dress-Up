#pragma once

#include <cstdint>
#include "../api/ThreeDUIInterface001.h"

// The one place the menu's colours are written down.
//
// Every plate behind every orb - the wheel, the gallery row, the outfit row - takes its
// hue from here, so a change lands everywhere at once and no row can drift into its own
// palette. Nothing else in the mod names a colour.
//
// There are two schemes. The normal one is cool: grey plates, a blue highlight, and warm
// orange reserved for "a click puts this on". The editing one is warm throughout, and is
// switched on while the wheel's edits are being written into a saved outfit - see
// DressupMenuManager::UpdateBackdropScheme. Turning the whole menu red-orange is the point:
// editing a saved outfit is the one mode where a click changes something the player will
// still have after they walk away, and a line of text alone was easy to miss.
namespace Backdrop
{
    constexpr const char* kModel = "meshes\\3DUI\\gradient-background-sphere.nif";

    // Scales are absolute since 3DUI 0.10.6: the backdrop no longer inherits the fit
    // correction the element derives from its preview model's bounds, so one value is one
    // size for a whole row instead of one size per model. A unit sphere at scale S comes
    // out S/2 units across, so each of these is set just inside its row's spacing - a
    // backdrop wider than the gap between two elements reads as one smeared plate.
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
    // itemAvailable is the loud one, and it is deliberately the *un*equipped state: a wheel
    // of gear is a wheel of choices, so what stands out should be what a click would change,
    // not what is already settled. Reading it the other way round meant a fully dressed NPC
    // lit the whole spiral up and the pieces still on offer were the dim ones. It is warm
    // rather than one more step along the blue, because the gallery row already owns the
    // blue and a brighter blue here would read as "selected".
    //
    // itemEquipped shares plateIdle. It was a third as opaque and barely glowing at one
    // point, on the reasoning that dozens at once would drown out the items, but in the
    // headset that read as no plate at all - the items floated against whatever the player
    // happened to be standing in front of.
    constexpr Palette kNormalPalette = {
        /* plateIdle     */ {0.55f, 0.62f, 0.72f, 1.00f, 1.00f},
        /* plateSelected */ {0.45f, 0.68f, 1.00f, 1.00f, 1.90f},
        /* itemAvailable */ {1.00f, 0.72f, 0.42f, 1.00f, 1.70f},
        /* itemEquipped  */ {0.55f, 0.62f, 0.72f, 1.00f, 1.00f},
        /* armed         */ {1.00f, 0.30f, 0.25f, 1.00f, 1.90f},
    };

    // The warm scheme, worn while a saved outfit is taking the edits.
    //
    // Same three steps as the cool one, rotated onto red-orange: a deep red-orange at rest,
    // orange for the one that is on, and a pale amber for what a click would put on. The
    // armed red has to stay legible against a red-orange row, so it goes further than its
    // cool twin - almost pure red at the mesh's full glow.
    constexpr Palette kEditingPalette = {
        /* plateIdle     */ {0.80f, 0.32f, 0.14f, 1.00f, 1.15f},
        /* plateSelected */ {1.00f, 0.60f, 0.14f, 1.00f, 2.00f},
        /* itemAvailable */ {1.00f, 0.82f, 0.45f, 1.00f, 1.80f},
        /* itemEquipped  */ {0.80f, 0.32f, 0.14f, 1.00f, 1.15f},
        /* armed         */ {1.00f, 0.10f, 0.08f, 1.00f, 2.40f},
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
    // mesh it is.
    inline void Apply(P3DUI::Element* element, float scale, const Tint& tint)
    {
        if (!element) return;
        element->SetBackgroundModel(kModel);
        element->SetBackgroundScale(scale);
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

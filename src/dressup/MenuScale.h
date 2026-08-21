#pragma once

#include <algorithm>

// The one number that sizes the whole menu.
//
// Every distance the menu is built out of - how big a plate is, how far apart two of them
// sit, how far a row hangs below the one above it, how close a hand has to come before an
// element lights up - is written in this codebase as the number it takes at scale 1, and
// passed through Scaled() on its way to 3DUI. Turning fMenuScale up multiplies all of them
// at once, so the menu grows without changing shape: the same plates, the same gaps
// between them, the same proportions, just bigger.
//
// That is the whole reason it is one multiplier over every distance rather than a size
// setting on the elements. Scaling the plates alone would have them grow into each other
// until a row read as one smeared surface; scaling the spacings alone would spread small
// plates further apart than they are wide. The layout was tuned as a set of ratios and
// this keeps every one of them.
//
// The curve radius goes through it too. What the eye reads as curvature is the menu's
// half-width divided by the radius - see Root::SetCurvature - so a menu twice as wide
// needs a radius twice as large to bend the same amount. Left alone it would wrap twice as
// hard at scale 2 and go nearly flat at scale 0.5.
//
// The hover threshold likewise: it is a world distance from the element's centre, so on
// plates twice the size an unscaled threshold would sit inside the artwork and the player
// would have to push their hand through the plate to light it up.
//
// Set once at load from the INI, before any element exists, and never moved after - the
// menu is built from these numbers rather than watching them, so a change mid-session
// would size new elements differently from the ones already up.
namespace MenuScale
{
    // What a hand-edited INI is allowed to ask for. Below a quarter the artwork is too
    // small to tell one piece of armour from another, and above four the rows reach past
    // where an arm goes even with the curve helping.
    constexpr float kMin = 0.25f;
    constexpr float kMax = 4.0f;

    inline float g_scale = 1.0f;

    inline void Set(float scale) { g_scale = std::clamp(scale, kMin, kMax); }

    inline float Get() { return g_scale; }

    // A distance in the menu's own units, in the size the player asked for.
    inline float Scaled(float value) { return value * g_scale; }
}

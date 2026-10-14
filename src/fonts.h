#pragma once

#include <QString>

/*
 * Google Sans, embedded the same way Font Awesome is (see icons.h): all
 * three weights are vendored as application fonts, so nothing about this
 * depends on the font being installed on the system it runs on.
 */
namespace Fonts {

// The whole interface's text size relative to the original sizing: 1.05 =
// 5 % larger. The base font (main.cpp) and every size derived from it by a
// fixed offset or floor (the plot's labels, cards, tabs) go through
// scaled(), so everything grows by exactly this factor — scaling only the
// base would grow the derived ones by a little more (their fixed offsets
// don't scale). The two fixed pixel sizes in style sheets are whole pixels.
constexpr double kUiScale = 1.05;
constexpr double scaled(double value) { return value * kUiScale; }

// Registers all three embedded weights. Safe to call more than once.
void initialise();

// The registered family name ("Google Sans", going by the weight files as
// supplied), or an empty string if registration failed for some reason —
// callers should leave the app's font alone rather than force a family
// name that does not actually exist.
QString family();

}   // namespace Fonts

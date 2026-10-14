#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

/*
 * Font Awesome icons rendered from the OTF rather than the SVGs: the SVG route
 * needs Qt6::Svg, which is a separate package from qt6-base, and a glyph font
 * recolours and scales without any of that.
 *
 * Names are the Font Awesome file names ("folder-open", "wave-square"). The
 * font and the codepoint table are both vendored in the project, so no external
 * Font Awesome release is needed to build or run.
 *
 * icon() still returns a null QIcon if the font fails to register, and callers
 * fall back to Qt's standard icons.
 */
namespace Icons {

bool available();

// Loads the embedded font. Safe to call more than once.
void initialise();

// The glyph itself, for placing in a label or drawing directly.
QString glyph(const QString &name);

// Rendered to a pixmap in the given colour. size is in logical pixels.
QIcon icon(const QString &name, const QColor &colour, int size = 20);
// The same as a pixmap (2x, device-pixel-ratio set), for drawing directly
// with QPainter. Null when neither an SVG nor a glyph exists for the name.
QPixmap pixmap(const QString &name, const QColor &colour, int size);
// Hugeicons SVGs (resources/icons, recoloured per theme) take precedence
// over the Font Awesome glyph of the same name when the app is built with
// Qt SVG; names are still the Font Awesome ones, mapped in icons.cpp.
bool hasSvg(const QString &name);

// The Font Awesome family name, for setting on a widget's font.
QString fontFamily();

}   // namespace Icons

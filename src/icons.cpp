#include "icons.h"

#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QPainter>
#include <QFile>
#include <QHash>
#ifdef VSX_HAVE_SVG
#include <QSvgRenderer>
#endif
#include <QPixmap>

#include "fa_codepoints.h"          // vendored alongside the font
#define VIMU_HAVE_FONTAWESOME 1

namespace {

QString g_family;
bool    g_loaded = false;

const QHash<QString, unsigned int> &codepoints()
{
    static const QHash<QString, unsigned int> table = [] {
        QHash<QString, unsigned int> map;
#if VIMU_HAVE_FONTAWESOME
        for (const auto &entry : kFontAwesomeCodepoints)
            map.insert(QString::fromLatin1(entry.name), entry.codepoint);
#endif
        return map;
    }();
    return table;
}

}   // namespace

namespace Icons {

void initialise()
{
    if (g_loaded)
        return;
    g_loaded = true;

#if VIMU_HAVE_FONTAWESOME
    const int id = QFontDatabase::addApplicationFont(
        QStringLiteral(":/fonts/fontawesome-solid.otf"));
    if (id >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty())
            g_family = families.first();
    }
#endif
}

bool available()
{
    initialise();
    return !g_family.isEmpty() && !codepoints().isEmpty();
}

QString fontFamily()
{
    initialise();
    return g_family;
}

QString glyph(const QString &name)
{
    const auto it = codepoints().constFind(name);
    if (it == codepoints().constEnd())
        return QString();
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(&(*it)), 1);
}

namespace {
// Font Awesome name (what callers ask for) -> Hugeicons file in :/icons.
const QHash<QString, QString> &svgFiles()
{
    static const QHash<QString, QString> map = {
        { QStringLiteral("forward-step"), QStringLiteral("Single") },
        { QStringLiteral("bolt"), QStringLiteral("Force") },
        { QStringLiteral("grip-lines"), QStringLiteral("Freeze") },
        { QStringLiteral("folder-open"), QStringLiteral("Open") },
        { QStringLiteral("floppy-disk"), QStringLiteral("Save") },
        { QStringLiteral("expand"), QStringLiteral("Full") },
        { QStringLiteral("calculator"), QStringLiteral("Math") },
        { QStringLiteral("ruler-combined"), QStringLiteral("Measurements") },
        { QStringLiteral("ruler-horizontal"), QStringLiteral("Rulers") },
        { QStringLiteral("square-binary"), QStringLiteral("Decode") },
        { QStringLiteral("note-sticky"), QStringLiteral("Annotations") },
        { QStringLiteral("gear"), QStringLiteral("Settings") },
        { QStringLiteral("toolbox"), QStringLiteral("Instruments") },
        { QStringLiteral("rotate-left"), QStringLiteral("Default") },
        { QStringLiteral("magnifying-glass"), QStringLiteral("Zoom") },
        { QStringLiteral("clone"), QStringLiteral("Reference") },
        { QStringLiteral("car"), QStringLiteral("Automotive") },
        { QStringLiteral("mobile-screen-button"), QStringLiteral("Remote") },
        { QStringLiteral("wave-square"), QStringLiteral("Scope") },
        { QStringLiteral("chart-simple"), QStringLiteral("FFT") },
        { QStringLiteral("chart-line"), QStringLiteral("XY") },
        { QStringLiteral("layer-group"), QStringLiteral("Persistence") },
        { QStringLiteral("eye"), QStringLiteral("Color") },
        { QStringLiteral("wrench"), QStringLiteral("Wrench") },
        { QStringLiteral("xmark"), QStringLiteral("Close") },
        { QStringLiteral("plus"), QStringLiteral("Add") },
        { QStringLiteral("pen"), QStringLiteral("Edit") },
        { QStringLiteral("trash"), QStringLiteral("Trash") },
        { QStringLiteral("arrows-up-down-left-right"), QStringLiteral("Move") },
        // No Font Awesome counterpart: the cards' step buttons and the
        // plot's zoom-history buttons.
        { QStringLiteral("card-minus"), QStringLiteral("MinusSign") },
        { QStringLiteral("card-plus"), QStringLiteral("PlusSign") },
        { QStringLiteral("zoom-back"), QStringLiteral("ZoomLevelBackward") },
        { QStringLiteral("zoom-forward"), QStringLiteral("ZoomLevelForward") },
    };
    return map;
}

#ifdef VSX_HAVE_SVG
// The file's text, once; empty when it doesn't exist.
QByteArray svgSource(const QString &name)
{
    static QHash<QString, QByteArray> cache;
    const auto it = cache.constFind(name);
    if (it != cache.constEnd())
        return *it;
    QByteArray bytes;
    const auto file = svgFiles().constFind(name);
    if (file != svgFiles().constEnd()) {
        QFile f(QStringLiteral(":/icons/%1.svg").arg(*file));
        if (f.open(QIODevice::ReadOnly))
            bytes = f.readAll();
    }
    cache.insert(name, bytes);
    return bytes;
}

// Hugeicons draw in currentColor: that becomes the requested colour.
QPixmap renderSvg(const QString &name, const QColor &colour, int size)
{
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3").arg(name, colour.name(QColor::HexArgb)).arg(size);
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return *hit;
    QByteArray source = svgSource(name);
    if (source.isEmpty())
        return QPixmap();
    source.replace("currentColor", colour.name(QColor::HexRgb).toLatin1());
    QSvgRenderer renderer(source);
    if (!renderer.isValid())
        return QPixmap();
    constexpr int kOversample = 2;
    QPixmap pixmap(size * kOversample, size * kOversample);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setOpacity(colour.alphaF());
        renderer.render(&painter);
    }
    pixmap.setDevicePixelRatio(kOversample);
    cache.insert(key, pixmap);
    return pixmap;
}
#endif
}   // namespace

bool hasSvg(const QString &name)
{
#ifdef VSX_HAVE_SVG
    return !svgSource(name).isEmpty();
#else
    Q_UNUSED(name);
    return false;
#endif
}

QPixmap pixmap(const QString &name, const QColor &colour, int size)
{
#ifdef VSX_HAVE_SVG
    const QPixmap svg = renderSvg(name, colour, size);
    if (!svg.isNull())
        return svg;
#endif
    const QIcon fallback = icon(name, colour, size);
    return fallback.isNull() ? QPixmap() : fallback.pixmap(size, size);
}

QIcon icon(const QString &name, const QColor &colour, int size)
{
#ifdef VSX_HAVE_SVG
    {
        const QPixmap svg = renderSvg(name, colour, size);
        if (!svg.isNull())
            return QIcon(svg);
    }
#endif
    if (!available())
        return QIcon();

    const QString text = glyph(name);
    if (text.isEmpty())
        return QIcon();

    // Rendered at 2x and handed to QIcon as a device-pixel-ratio pixmap, so it
    // stays sharp on a scaled display.
    constexpr int kOversample = 2;
    QPixmap pixmap(size * kOversample, size * kOversample);
    pixmap.fill(Qt::transparent);

    QFont font(g_family);
    font.setPixelSize(int(size * kOversample * 0.82));

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(colour);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, text);
    painter.end();

    pixmap.setDevicePixelRatio(kOversample);
    return QIcon(pixmap);
}

}   // namespace Icons

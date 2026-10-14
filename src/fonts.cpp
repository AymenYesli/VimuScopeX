#include "fonts.h"

#include <QFontDatabase>
#include <QStringList>

namespace {

QString g_family;
bool    g_loaded = false;

}   // namespace

namespace Fonts {

void initialise()
{
    if (g_loaded)
        return;
    g_loaded = true;

    // Regular first: if the three weights ever disagree on the family name
    // they report (a font-packaging quirk, not something to expect from a
    // well-formed release), Regular is the one every plain-text widget in
    // this app actually uses, so it is the family name worth keeping.
    const QString files[] = {
        QStringLiteral(":/fonts/GoogleSans-Regular.ttf"),
        QStringLiteral(":/fonts/GoogleSans-Medium.ttf"),
        QStringLiteral(":/fonts/GoogleSans-Bold.ttf"),
    };
    for (const QString &path : files) {
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0)
            continue;
        if (g_family.isEmpty()) {
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty())
                g_family = families.first();
        }
    }
}

QString family()
{
    initialise();
    return g_family;
}

}   // namespace Fonts

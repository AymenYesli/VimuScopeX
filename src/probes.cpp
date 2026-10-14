#include "probes.h"

#include <QSettings>

#include <cmath>

QString Probe::summary() const
{
    if (unit == QLatin1String("V") && qFuzzyIsNull(offset)) {
        if (qFuzzyCompare(gradient, 1.0))
            return QStringLiteral("x1");
        return QStringLiteral("x%1").arg(gradient, 0, 'g', 3);
    }
    QString text = QStringLiteral("%1 %2/V").arg(gradient, 0, 'g', 3).arg(unit);
    if (!qFuzzyIsNull(offset))
        text += QStringLiteral(" %1%2")
                    .arg(offset >= 0 ? QStringLiteral("+") : QStringLiteral("-"))
                    .arg(std::fabs(offset), 0, 'g', 3);
    return text;
}

ProbeLibrary::ProbeLibrary(QObject *parent)
    : QObject(parent)
{
    load();
}

void ProbeLibrary::ensureBuiltins()
{
    // Built-ins are always present and cannot be edited or removed.
    const QVector<Probe> builtins = {
        { QStringLiteral("x1"),   QStringLiteral("V"),   1.0,   0.0, true },
        { QStringLiteral("x10"),  QStringLiteral("V"),  10.0,   0.0, true },
        { QStringLiteral("x20"),  QStringLiteral("V"),  20.0,   0.0, true },
        { QStringLiteral("x100"), QStringLiteral("V"), 100.0,   0.0, true },
    };

    for (int i = builtins.size() - 1; i >= 0; --i) {
        if (indexOfName(builtins[i].name) < 0)
            m_probes.prepend(builtins[i]);
    }
}

const Probe &ProbeLibrary::at(int index) const
{
    static const Probe fallback{ QStringLiteral("x1"), QStringLiteral("V"), 1.0, 0.0, true };
    if (index < 0 || index >= m_probes.size())
        return fallback;
    return m_probes[index];
}

int ProbeLibrary::indexOfName(const QString &name) const
{
    for (int i = 0; i < m_probes.size(); ++i) {
        if (m_probes[i].name == name)
            return i;
    }
    return -1;
}

void ProbeLibrary::add(const Probe &probe)
{
    m_probes.append(probe);
    save();
    emit changed();
}

void ProbeLibrary::update(int index, const Probe &probe)
{
    if (index < 0 || index >= m_probes.size() || m_probes[index].builtin)
        return;
    m_probes[index] = probe;
    save();
    emit changed();
}

void ProbeLibrary::remove(int index)
{
    if (index < 0 || index >= m_probes.size() || m_probes[index].builtin)
        return;
    m_probes.remove(index);
    save();
    emit changed();
}

void ProbeLibrary::load()
{
    m_probes.clear();

    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    const int count = settings.beginReadArray(QStringLiteral("probes"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        Probe p;
        p.name     = settings.value(QStringLiteral("name")).toString();
        p.unit     = settings.value(QStringLiteral("unit"), QStringLiteral("V")).toString();
        p.gradient = settings.value(QStringLiteral("gradient"), 1.0).toDouble();
        p.offset   = settings.value(QStringLiteral("offset"), 0.0).toDouble();
        p.builtin  = false;
        if (!p.name.isEmpty() && !qFuzzyIsNull(p.gradient))
            m_probes.append(p);
    }
    settings.endArray();

    ensureBuiltins();
    emit changed();
}

void ProbeLibrary::save() const
{
    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    settings.beginWriteArray(QStringLiteral("probes"));

    int written = 0;
    for (const Probe &p : m_probes) {
        if (p.builtin)
            continue;                      // rebuilt on load
        settings.setArrayIndex(written++);
        settings.setValue(QStringLiteral("name"), p.name);
        settings.setValue(QStringLiteral("unit"), p.unit);
        settings.setValue(QStringLiteral("gradient"), p.gradient);
        settings.setValue(QStringLiteral("offset"), p.offset);
    }
    settings.endArray();
}

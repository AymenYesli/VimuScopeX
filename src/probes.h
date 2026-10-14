#pragma once

#include <QObject>
#include <QString>
#include <QVector>

/*
 * A probe converts what the BNC measures into what the user cares about:
 *
 *      displayed = raw_volts * gradient + offset      [unit]
 *
 * A 10:1 probe is gradient 10, unit V. A 20 A/V current clamp is gradient 20,
 * unit A. The linear form covers everything PicoScope's "Equation" scaling does.
 */
struct Probe {
    QString name;
    QString unit     = QStringLiteral("V");
    double  gradient = 1.0;
    double  offset   = 0.0;
    bool    builtin  = false;

    QString summary() const;          // e.g. "x10  (V)" or "20 A/V"
    double  apply(double rawVolts) const { return rawVolts * gradient + offset; }
};

class ProbeLibrary : public QObject {
    Q_OBJECT

public:
    explicit ProbeLibrary(QObject *parent = nullptr);

    const QVector<Probe> &probes() const { return m_probes; }
    int count() const { return m_probes.size(); }
    const Probe &at(int index) const;
    int indexOfName(const QString &name) const;

    void add(const Probe &probe);
    void update(int index, const Probe &probe);
    void remove(int index);

    void load();
    void save() const;

signals:
    void changed();

private:
    void ensureBuiltins();

    QVector<Probe> m_probes;
};

#pragma once
//
// The Math channel's operations, result units and labels — one place, used
// by the plot, the navigator strip and CSV export, so they can't disagree.
// Plain C++ (no Qt), tested standalone.
//
// Sources are what each channel *measures*: probe applied (gain and offset),
// inverted if the channel is, in the probe's unit. (Combining the raw,
// pre-probe values — the first version — made CH1 + CH2 wrong as soon as a
// channel had a x10 probe or a current clamp.)
//
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace MathOps {

// Values are saved in settings: Add/Subtract/Multiply keep 0/1/2.
enum Op {
    Add = 0, Subtract, Multiply,
    Divide, Average, Maximum, Minimum,
    Negate, Absolute, Square, SquareRoot, Reciprocal, Decibel,
    Derivative, Integral, Scale,
    Count
};

inline bool usesB(int op)
{
    return op == Add || op == Subtract || op == Multiply || op == Divide
           || op == Average || op == Maximum || op == Minimum;
}

// The menu's wording, A / B standing for the sources.
inline const char *menuText(int op)
{
    switch (op) {
    case Add:        return "A + B";
    case Subtract:   return "A \u2212 B";
    case Multiply:   return "A \u00d7 B";
    case Divide:     return "A \u00f7 B";
    case Average:    return "(A + B) / 2";
    case Maximum:    return "max(A, B)";
    case Minimum:    return "min(A, B)";
    case Negate:     return "\u2212A";
    case Absolute:   return "|A|";
    case Square:     return "A\u00b2";
    case SquareRoot: return "\u221a|A|";
    case Reciprocal: return "1 / A";
    case Decibel:    return "20 log\u2081\u2080|A|  (dB)";
    case Derivative: return "dA/dt";
    case Integral:   return "\u222bA dt";
    case Scale:      return "k \u00b7 A + c";
    default:         return "?";
    }
}

// The trace's label with the real sources, e.g. "CH1 \u00d7 CH2", "d(CH1)/dt".
inline std::string label(int op, const std::string &a, const std::string &b)
{
    switch (op) {
    case Add:        return a + " + " + b;
    case Subtract:   return a + " \u2212 " + b;
    case Multiply:   return a + " \u00d7 " + b;
    case Divide:     return a + " \u00f7 " + b;
    case Average:    return "(" + a + " + " + b + ")/2";
    case Maximum:    return "max(" + a + ", " + b + ")";
    case Minimum:    return "min(" + a + ", " + b + ")";
    case Negate:     return "\u2212" + a;
    case Absolute:   return "|" + a + "|";
    case Square:     return a + "\u00b2";
    case SquareRoot: return "\u221a|" + a + "|";
    case Reciprocal: return "1/" + a;
    case Decibel:    return "dB(" + a + ")";
    case Derivative: return "d(" + a + ")/dt";
    case Integral:   return "\u222b" + a + " dt";
    case Scale:      return "k\u00b7" + a + "+c";
    default:         return a;
    }
}

// The result's unit from the sources' units: V x A is W, V / A is ohms,
// A / V siemens, a ratio of like units is unitless, and so on.
inline std::string unit(int op, const std::string &ua, const std::string &ub)
{
    const bool va = (ua == "V" && ub == "A") || (ua == "A" && ub == "V");
    switch (op) {
    case Add: case Subtract: case Average: case Maximum: case Minimum:
    case Negate: case Absolute: case Scale:
        return ua;
    case Multiply:
        if (va) return "W";
        if (ua == ub) return ua + "\u00b2";
        return ua + "\u00b7" + ub;
    case Divide:
        if (ua == "V" && ub == "A") return "\u03a9";
        if (ua == "A" && ub == "V") return "S";
        if (ua == ub) return "";
        return ua + "/" + ub;
    case Square:     return ua + "\u00b2";
    case SquareRoot: return "\u221a" + ua;
    case Reciprocal: return "1/" + ua;
    case Decibel:    return "dB" + ua;          // dBV, dBA
    case Derivative: return ua + "/s";
    case Integral:   return ua + "\u00b7s";
    default:         return ua;
    }
}

// a, b: measured values (b ignored by one-source operations); rate in S/s
// for the derivative and integral; k, c for k.A + c. Division, 1/A and dB
// hold the previous value where the denominator / argument is ~0 rather
// than producing infinities that would wreck the autoscale.
inline std::vector<double> compute(int op, const std::vector<double> &a, const std::vector<double> &b,
                                   double rate, double k = 1.0, double c = 0.0)
{
    const size_t n = usesB(op) ? std::min(a.size(), b.size()) : a.size();
    std::vector<double> out(n, 0.0);
    constexpr double tiny = 1e-12;
    double held = 0.0;
    switch (op) {
    case Add:      for (size_t i = 0; i < n; ++i) out[i] = a[i] + b[i]; break;
    case Subtract: for (size_t i = 0; i < n; ++i) out[i] = a[i] - b[i]; break;
    case Multiply: for (size_t i = 0; i < n; ++i) out[i] = a[i] * b[i]; break;
    case Divide:
        for (size_t i = 0; i < n; ++i)
            out[i] = held = std::fabs(b[i]) > tiny ? a[i] / b[i] : held;
        break;
    case Average:  for (size_t i = 0; i < n; ++i) out[i] = 0.5 * (a[i] + b[i]); break;
    case Maximum:  for (size_t i = 0; i < n; ++i) out[i] = std::max(a[i], b[i]); break;
    case Minimum:  for (size_t i = 0; i < n; ++i) out[i] = std::min(a[i], b[i]); break;
    case Negate:   for (size_t i = 0; i < n; ++i) out[i] = -a[i]; break;
    case Absolute: for (size_t i = 0; i < n; ++i) out[i] = std::fabs(a[i]); break;
    case Square:   for (size_t i = 0; i < n; ++i) out[i] = a[i] * a[i]; break;
    case SquareRoot: for (size_t i = 0; i < n; ++i) out[i] = std::sqrt(std::fabs(a[i])); break;
    case Reciprocal:
        for (size_t i = 0; i < n; ++i)
            out[i] = held = std::fabs(a[i]) > tiny ? 1.0 / a[i] : held;
        break;
    case Decibel:
        for (size_t i = 0; i < n; ++i)
            out[i] = held = std::fabs(a[i]) > tiny ? 20.0 * std::log10(std::fabs(a[i])) : held;
        break;
    case Derivative:
        // Central differences (one-sided at the ends): units per second.
        if (n >= 2 && rate > 0.0) {
            out[0] = (a[1] - a[0]) * rate;
            out[n - 1] = (a[n - 1] - a[n - 2]) * rate;
            for (size_t i = 1; i + 1 < n; ++i)
                out[i] = (a[i + 1] - a[i - 1]) * rate * 0.5;
        }
        break;
    case Integral:
        // Running trapezoid sum from the first sample on screen.
        if (rate > 0.0)
            for (size_t i = 1; i < n; ++i)
                out[i] = out[i - 1] + 0.5 * (a[i] + a[i - 1]) / rate;
        break;
    case Scale:    for (size_t i = 0; i < n; ++i) out[i] = k * a[i] + c; break;
    default:       break;
    }
    return out;
}

} // namespace MathOps

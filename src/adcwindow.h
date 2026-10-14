#pragma once
//
// What the ADC actually digitises for one channel, from the SDK's own
// code -> volts conversion: voltage = code x zoom + bias, codes 0 .. 2^bits-1
// (the SDK documents this for ReadADCDatas / ReadStreamADCDatas). So the
// window is bias .. bias + (2^bits - 1) x zoom, at |zoom| per code — what the
// scope did with a requested range, not what was asked for.
//
#include <algorithm>
#include <cmath>

struct AdcWindow {
    bool   valid = false;
    int    bits = 0;
    double zoom = 0.0, bias = 0.0;     // as the SDK reports them
    double minV = 0.0, maxV = 0.0;     // the window, input volts (before any probe)
    double sdkResolution = 0.0;        // GetVoltageResolution(), as the SDK reports it
    int    requestedMinMv = 0, requestedMaxMv = 0;
    // What was actually sent: the stream path widens a range the scope
    // refuses until one is accepted (StreamEngine::applyStreamRange()).
    // Equal to the request when nothing was refused.
    int    sentMinMv = 0, sentMaxMv = 0;
    bool   refused() const { return sentMinMv != requestedMinMv || sentMaxMv != requestedMaxMv; }

    void complete()
    {
        const double top = std::ldexp(1.0, bits > 0 ? bits : 12) - 1.0;
        minV = std::min(bias, bias + top * zoom);
        maxV = std::max(bias, bias + top * zoom);
        valid = std::isfinite(zoom) && std::isfinite(bias) && zoom != 0.0;
    }
    int codes() const { return 1 << (bits > 0 ? bits : 12); }
    // How many of the ADC's codes fall inside the requested window (= what
    // the screen shows): all of them when the scope honoured it exactly.
    double codesOnScreen() const
    {
        if (!valid)
            return 0.0;
        return std::min(double(codes()), double(requestedMaxMv - requestedMinMv) / 1000.0 / std::fabs(zoom));
    }
};

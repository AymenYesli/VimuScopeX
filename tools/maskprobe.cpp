// maskprobe — determine which Capture() mask bit selects which analog channel.
//
// The vendor header documents the mask two incompatible ways, and a bad mask
// wedges the device so that every subsequent capture fails too. This tool
// therefore tears the SDK session down and rebuilds it between every attempt,
// so each mask is tested against a clean device and the results mean something.
//
// Build:  cmake --build build --target maskprobe
// Run:    ./build/maskprobe            (defaults: 8 KB, 1 MS/s, 1500 ms timeout)
//         ./build/maskprobe --kb 8 --rate 1000000 --masks 0x01,0x02,0x20

#include <VmMultMsoLib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__)
#  include <sys/wait.h>
#  include <unistd.h>
#  include <csignal>
#  define PROBE_CAN_FORK 1
#else
#  define PROBE_CAN_FORK 0
#endif

namespace {

int  g_kb      = 8;
unsigned g_rate = 1000000;
int  g_timeout = 1500;
unsigned g_style = 0;      // TRIGGER_STYLE_* ; 0 = free run
int  g_level   = 0;        // trigger level, mV
bool g_stream  = false;    // run the stream probe instead of the mask sweep
int  g_seconds = 3;        // how long to stream for
int  g_updates = 10;       // StreamCapture count_every_1s

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Every plausible single-channel bit, then the same bits paired with CH1.
std::vector<unsigned short> defaultMasks()
{
    return {
        0x0001,
        0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
        0x0003, 0x0005, 0x0009, 0x0011, 0x0021, 0x0041, 0x0081,
    };
}

// Filled from the SDK's thread, read from main. The callback does nothing but
// count: anything slower there trips the watchdog and resets the USB link.
struct StreamStats {
    std::atomic<unsigned long long> callbacks{0};
    std::atomic<unsigned long long> samples{0};
    std::atomic<unsigned long long> total{0};
    std::atomic<unsigned long long> peakMemory{0};
    std::atomic<unsigned int> lastLength{0};
    std::atomic<unsigned int> failures{0};
    std::atomic<unsigned int> finished{0};
    std::atomic<unsigned int> channels{0};
};

StreamStats g_stats;

void CALLBACK onStreamData(void *, unsigned int, unsigned char channel,
                           double *, unsigned int length,
                           unsigned int failed, unsigned int success,
                           unsigned long long, unsigned long long total,
                           unsigned long long memory)
{
    g_stats.callbacks.fetch_add(1);
    g_stats.samples.fetch_add(length);
    g_stats.lastLength.store(length);
    g_stats.total.store(total);
    g_stats.channels.fetch_or(1u << channel);
    if (failed)
        g_stats.failures.fetch_add(1);
    if (success)
        g_stats.finished.fetch_add(1);

    unsigned long long peak = g_stats.peakMemory.load();
    while (memory > peak && !g_stats.peakMemory.compare_exchange_weak(peak, memory)) {}
}

struct Session {
    unsigned int devId = 0xFFFFFFFFu;
    int channels = 0;
    bool ok = false;
};

Session openSession()
{
    Session s;
    if (!InitDll(0, 0)) {
        std::fprintf(stderr, "InitDll failed\n");
        return s;
    }
    for (int waited = 0; waited < 5000; waited += 100) {
        if (waited % 500 == 0)
            ScanDevice();          // the first session after a plug often needs a rescan
        if (GetAvailableDevCount() > 0)
            break;
        sleepMs(100);
    }
    if (GetAvailableDevCount() <= 0) {
        FinishDll();
        return s;
    }

    s.devId = GetOnlyId(0);
    if (!IsDevAvailable(s.devId)) {
        FinishDll();
        return s;
    }

    s.channels = std::min(GetOscChannelNum(s.devId), 4);
    SetOscCaptureMode(s.devId, 0);
    for (int ch = 0; ch < s.channels; ++ch) {
        if (IsSupportAcDc(s.devId, unsigned(ch)))
            SetAcDc(s.devId, unsigned(ch), 0);
        SetOscChannelRangemV(s.devId, ch, -1000, 1000);
    }
    SetTriggerMode(s.devId, 0);          // TRIGGER_MODE_AUTO
    SetTriggerStyle(s.devId, g_style);   // 0 = TRIGGER_STYLE_NONE, free run
    if (g_style != 0) {
        SetTriggerSource(s.devId, 0);
        SetTriggerLevelmV(s.devId, g_level, 50);
        if (IsSupportPreTriggerPercent(s.devId))
            SetPreTriggerPercent(s.devId, 50);
    }
    SetOscSampleRate(s.devId, g_rate);

    s.ok = true;
    return s;
}

void closeSession(Session &s)
{
    if (s.ok) {
        AbortCapture(s.devId);
        CloseDevice(s.devId);
    }
    FinishDll();
    s.ok = false;
}

void probeInProcess(unsigned short mask)
{
    Session s = openSession();
    if (!s.ok) {
        std::printf("mask 0x%04x : NO DEVICE\n", mask);
        return;
    }

    const int kb = Capture(s.devId, g_kb, mask, 1);
    if (kb <= 0) {
        std::printf("mask 0x%04x : Capture() refused (returned %d)\n", mask, kb);
        closeSession(s);
        return;
    }

    const unsigned capacity = unsigned(kb) * 1024u;

    bool ready = false;
    int waited = 0;
    for (; waited < g_timeout; waited += 20) {
        if (IsDataReady(s.devId)) { ready = true; break; }
        sleepMs(20);
    }

    if (!ready) {
        std::printf("mask 0x%04x : TIMEOUT after %d ms (capture %d KB)\n",
                    mask, g_timeout, kb);
        closeSession(s);
        return;
    }

    std::printf("mask 0x%04x : ready in %4d ms, %d KB ->", mask, waited, kb);

    std::vector<double> buffer(capacity);
    for (int ch = 0; ch < s.channels; ++ch) {
        const unsigned n = ReadVoltageDatas(s.devId, char(ch), buffer.data(), capacity);
        if (n == 0) {
            std::printf("  CH%d: -", ch + 1);
            continue;
        }
        const auto lo = *std::min_element(buffer.begin(), buffer.begin() + n);
        const auto hi = *std::max_element(buffer.begin(), buffer.begin() + n);
        std::printf("  CH%d: %u pts Vpp %.4f", ch + 1, n, hi - lo);
    }
    std::printf("\n");

    closeSession(s);
}

// The vendor library raises SIGFPE on a mask whose bits do not correspond to
// channels this device has, so each attempt runs in a forked child. A mask that
// kills the library kills only the child, and the sweep continues.
void probe(unsigned short mask)
{
#if PROBE_CAN_FORK
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) {
        probeInProcess(mask);
        return;
    }
    if (pid == 0) {
        probeInProcess(mask);
        std::fflush(stdout);
        _exit(0);
    }

    int state = 0;
    waitpid(pid, &state, 0);
    if (WIFSIGNALED(state)) {
        const int sig = WTERMSIG(state);
        std::printf("mask 0x%04x : LIBRARY CRASHED (%s) — this mask is not valid on this device\n",
                    mask, strsignal(sig));
        std::fflush(stdout);
    }
#else
    probeInProcess(mask);
#endif
}

// Stream probe, now a faithful copy of DLLMultDev-Stream:
//
//   InitDll(1, 1)                      log and watchdog on, as the demo has them
//   SetDevNoticeCallBack               before ScanDevice
//   SetStreamDataReadyCallBack         before ScanDevice
//   ScanDevice()                       exactly once
//   ...add callback: SetOscCaptureMode(1), ranges, StreamCapture()
//
// StreamCapture RETURNS 0 ON SUCCESS, despite the header documenting 1. The
// vendor demo ignores the return value entirely, which is how it never noticed.
// Success is therefore judged by whether data callbacks arrive, not by rc.
//
// Repeated ScanDevice calls leave the interface claimed (LIBUSB_ERROR_BUSY), and
// retrying StreamCapture in a loop eventually crashes the library with SIGFPE,
// so this makes exactly one attempt.

std::atomic<unsigned int> g_streamDev{0xFFFFFFFFu};
std::atomic<int>          g_startRc{99};
std::atomic<int>          g_channels{0};

unsigned short g_streamMask = 0x0003;

void CALLBACK onStreamAdd(void *, unsigned int devId)
{
    // Configured and started here, on the SDK's thread, exactly as the demo
    // does it. Kept short: the watchdog resets USB if a callback dawdles.
    SetOscCaptureMode(devId, 1);

    const int channels = std::min(GetOscChannelNum(devId), 4);
    for (int ch = 0; ch < channels; ++ch)
        SetStreamChannelRangemV(devId, ch, -10000, 10000);
    g_channels.store(channels);

    const unsigned long long lengthKb =
        (unsigned long long)(g_rate) * (unsigned long long)(g_seconds + 2) / 1024ull + 16ull;

    const int rc = StreamCapture(devId, unsigned(g_updates), lengthKb,
                                 g_streamMask, g_rate);
    g_startRc.store(rc);
    g_streamDev.store(devId);
}

void CALLBACK onStreamRemove(void *, unsigned int)
{
    g_streamDev.store(0xFFFFFFFFu);
}

void streamProbe(unsigned short requestedMask)
{
    g_streamMask = requestedMask;

    // Log and watchdog on, matching the demo.
    if (!InitDll(1, 1)) {
        std::printf("InitDll failed\n");
        return;
    }

    SetDevNoticeCallBack(nullptr, &onStreamAdd, &onStreamRemove);
    SetStreamDataReadyCallBack(nullptr, &onStreamData);

    std::printf("request     : %u S/s, %d updates/s, mask 0x%04x, %d s\n\n",
                g_rate, g_updates, requestedMask, g_seconds);

    ScanDevice();          // once only: rescanning re-claims and returns BUSY

    unsigned int dev = 0xFFFFFFFFu;
    for (int waited = 0; waited < 6000; waited += 100) {
        dev = g_streamDev.load();
        if (dev != 0xFFFFFFFFu)
            break;
        sleepMs(100);
    }
    if (dev == 0xFFFFFFFFu) {
        std::printf("No device. Close VimuScopeX if it is running.\n");
        FinishDll();
        return;
    }

    std::printf("roll mode   : IsSupportRollMode = %d\n", IsSupportRollMode(dev));
    std::printf("stream range: %d .. %d mV\n",
                GetStreamRangeMinmV(dev), GetStreamRangeMaxmV(dev));
    std::printf("channels    : %d\n", g_channels.load());
    std::printf("StreamCapture returned %d (0 is success here, not 1)\n",
                g_startRc.load());

    // Wait for the first callback rather than trusting the return code.
    bool running = false;
    for (int waited = 0; waited < 2000 && !running; waited += 100) {
        running = g_stats.callbacks.load() > 0;
        sleepMs(100);
    }
    if (!running) {
        std::printf("\nNo data callbacks arrived: the stream really did not start.\n");
        FinishDll();
        return;
    }
    std::printf("\nStreaming.\n");
    const auto began = std::chrono::steady_clock::now();

    for (int tick = 0; tick < g_seconds * 1000; tick += 500) {
        sleepMs(500);
        std::printf("  t=%4.1fs  callbacks %-6llu  last %-8u  samples %-12llu "
                    "peakmem %-6llu  failed %u\n",
                    (tick + 500) / 1000.0,
                    g_stats.callbacks.load(), g_stats.lastLength.load(),
                    g_stats.samples.load(), g_stats.peakMemory.load(),
                    g_stats.failures.load());
        if (g_stats.finished.load())
            break;
    }

    // Measured, not nominal: dividing by the requested duration inflates every
    // rate by the few percent the print loop overruns by.
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - began).count();

    StreamStopCapture(dev);
    sleepMs(300);

    const unsigned long long callbacks = g_stats.callbacks.load();
    const unsigned long long samples   = g_stats.samples.load();
    std::printf("\nchannels seen : mask 0x%02x\n", g_stats.channels.load());
    // One callback per channel per block, so the block rate is the callback
    // count divided by the number of channels streaming.
    const int active = std::max(1, __builtin_popcount(g_stats.channels.load()));
    const double blocksPerSec = double(callbacks) / active / elapsed;
    const double perChannel   = double(samples) / active / elapsed;
    const unsigned int block  = g_stats.lastLength.load();

    std::printf("block rate    : %.1f/s per channel (asked for %d)\n",
                blocksPerSec, g_updates);
    std::printf("block size    : %u samples (%.0f KB, ideal %.0f)\n",
                block, block / 1024.0, double(g_rate) / std::max(1, g_updates));
    std::printf("per channel   : %.3f MS/s (asked for %.3f)\n",
                perChannel / 1e6, g_rate / 1e6);
    std::printf("throughput    : %.2f MS/s aggregate, %.1f MB/s at 2 bytes/sample\n",
                double(samples) / elapsed / 1e6,
                double(samples) * 2.0 / elapsed / 1e6);
    std::printf("library queue : %llu samples = %.1f blocks -> %.0f ms of latency\n",
                g_stats.peakMemory.load(),
                block ? double(g_stats.peakMemory.load()) / block : 0.0,
                blocksPerSec > 0 && block
                    ? double(g_stats.peakMemory.load()) / block / blocksPerSec * 1000.0
                    : 0.0);
    std::printf("dropped       : %u failed callbacks over %.2f s\n",
                g_stats.failures.load(), elapsed);

    FinishDll();
}

// Rates do not fit in the unsigned short parseMasks() returns.
std::vector<unsigned int> parseRates(const char *csv)
{
    std::vector<unsigned int> out;
    std::string s(csv);
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        const std::string token = s.substr(start, comma == std::string::npos
                                                      ? std::string::npos
                                                      : comma - start);
        if (!token.empty())
            out.push_back(static_cast<unsigned int>(std::strtoul(token.c_str(), nullptr, 0)));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

std::vector<unsigned short> parseMasks(const char *csv)
{
    std::vector<unsigned short> out;
    std::string s(csv);
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        const std::string token = s.substr(start, comma == std::string::npos
                                                      ? std::string::npos
                                                      : comma - start);
        if (!token.empty())
            out.push_back(static_cast<unsigned short>(std::strtoul(token.c_str(), nullptr, 0)));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

}   // namespace

int main(int argc, char **argv)
{
    std::vector<unsigned short> masks;
    std::vector<unsigned short> depths;
    std::vector<unsigned int>   ladder;

    for (int i = 1; i < argc; ++i) {
        const bool hasNext = (i + 1) < argc;
        if (!std::strcmp(argv[i], "--kb") && hasNext)
            g_kb = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--rate") && hasNext)
            g_rate = unsigned(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--timeout") && hasNext)
            g_timeout = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--masks") && hasNext)
            masks = parseMasks(argv[++i]);
        else if (!std::strcmp(argv[i], "--style") && hasNext)
            g_style = unsigned(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--level") && hasNext)
            g_level = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--depths") && hasNext)
            depths = parseMasks(argv[++i]);
        else if (!std::strcmp(argv[i], "--stream"))
            g_stream = true;
        else if (!std::strcmp(argv[i], "--seconds") && hasNext)
            g_seconds = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--updates") && hasNext)
            g_updates = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--ladder") && hasNext)
            ladder = parseRates(argv[++i]);
        else {
            std::printf(
                "usage: %s [--kb N] [--rate HZ] [--timeout MS] [--masks 0x01,0x03]\n"
                "          [--style 0x01] [--level MV] [--depths 8,64,256,1024]\n"
                "\n"
                "          [--stream] [--seconds N] [--updates N]\n"
                "\n"
                "  --style 0x01 arms a rising-edge trigger, so --depths measures how\n"
                "  deep a TRIGGERED capture can go.\n"
                "  --stream reports roll-mode support, the streaming rate table and\n"
                "  the real callback behaviour of StreamCapture.\n",
                argv[0]);
            return 1;
        }
    }

    if (g_stream) {
        const unsigned short mask = masks.empty() ? 0x0003 : masks.front();

        // A ladder walks the rates looking for where drops start. Each step runs
        // in its own process: if one wedges the library, the rest still run.
        if (!ladder.empty()) {
#if PROBE_CAN_FORK
            for (unsigned int rate : ladder) {
                std::printf("\n===== %u S/s =====\n", rate);
                std::fflush(stdout);
                const pid_t pid = fork();
                if (pid == 0) {
                    g_rate = rate;
                    streamProbe(mask);
                    std::fflush(stdout);
                    _exit(0);
                }
                int state = 0;
                waitpid(pid, &state, 0);
                if (WIFSIGNALED(state))
                    std::printf("  library crashed (%s)\n", strsignal(WTERMSIG(state)));
                sleepMs(800);
            }
            return 0;
#endif
        }

        streamProbe(mask);
        return 0;
    }

    if (masks.empty() && depths.empty())
        masks = defaultMasks();

    // One session up front purely to report what the device claims to be.
    Session info = openSession();
    if (info.ok) {
        char description[256] = {0};
        GetDeviceDesString(info.devId, description, int(sizeof(description) - 1));

        const int rateCount = GetOscSupportSampleRateNum(info.devId);
        std::vector<unsigned int> rates(std::max(rateCount, 0));
        const int got = rateCount > 0
            ? GetOscSupportSampleRates(info.devId, rates.data(), rateCount)
            : 0;

        std::printf("device      : %s (id %u)\n", description, info.devId);
        std::printf("channels    : %d, %d-bit ADC, %u KB memory\n",
                    info.channels, GetOscAdcBit(info.devId), GetMemoryLength(info.devId));
        std::printf("hard trigger: %d,  pre-trigger: %d,  roll: %d\n",
                    IsSupportHardTrigger(info.devId),
                    IsSupportPreTriggerPercent(info.devId) ? 1 : 0,
                    IsSupportRollMode(info.devId));
        std::printf("rates (%d)   :", got);
        for (int i = 0; i < got; ++i)
            std::printf(" %u", rates[size_t(i)]);
        std::printf("\n");
        std::printf("probing at  : %d KB, %u S/s, %d ms timeout\n\n",
                    g_kb, g_rate, g_timeout);
        closeSession(info);
    } else {
        std::printf("No device found for the initial info pass.\n\n");
    }

    sleepMs(300);

    for (unsigned short mask : masks) {
        probe(mask);
        sleepMs(400);   // let the device settle between sessions
    }

    // Depth sweep: same probe, one capture length at a time, fresh session each.
    const unsigned short depthMask = masks.empty() ? 0x0001 : masks.front();
    for (unsigned short kb : depths) {
        g_kb = kb;
        std::printf("depth %5d KB : ", int(kb));
        std::fflush(stdout);
        probe(depthMask);
        sleepMs(400);
    }

    std::printf("\nA channel shown as '-' returned zero samples: it was not part of\n"
                "that capture. A mask is correct when exactly the channels it should\n"
                "select come back with samples.\n");
    return 0;
}

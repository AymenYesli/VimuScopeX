#pragma once
//
// Finding the interface freezes: timings instead of guesses.
//
//   VSX_TRACE("name");   at the top of a function (or block): times it.
//
// A traced call taking >= 20 ms prints, when its outermost traced caller
// finishes, as a tree on stderr — what ran inside, with the quick calls
// summed into one line:
//
//   [slow] setAcquisitionMode 840 ms
//   [slow]   applyChannel 410 ms
//   [slow]     acq.setRangeMv 200 ms
//   [slow]   (+27 quick calls, 95 ms)
//
// startStallWatch() adds a heartbeat on the GUI thread and a watcher thread:
// whenever the interface stops responding for >= 150 ms it prints
// "[stall] interface blocked N ms (in <traced call>)" — which also catches
// freezes in code that isn't traced.
//
#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <chrono>
#ifdef __linux__
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace PerfTrace {

// Off unless VSX_TRACE=1: the timers then cost one check each and print
// nothing, and no stall watcher runs.
inline bool enabled()
{
    static const bool on = qEnvironmentVariableIntValue("VSX_TRACE") != 0;
    return on;
}

using Clock = std::chrono::steady_clock;
constexpr double kSlowMs = 20.0;
constexpr long long kStallMs = 150;

struct Frame {
    const char *name = "";
    Clock::time_point start;
    std::vector<std::string> lines;   // slow callees, already indented
    int quickCalls = 0;
    double quickMs = 0.0;
    Frame *parent = nullptr;          // the traced call this one runs inside
    size_t depth = 0;
};

// The innermost traced call on this thread: a plain pointer, constant-
// initialised, so the compiler needs no hidden "TLS init function". (A
// thread_local std::vector here made MinGW emit that function in every
// translation unit as an ordinary symbol, and the Windows link failed with
// "multiple definition of TLS init function for PerfTrace::t_stack".)
inline thread_local Frame *t_top = nullptr;
inline std::atomic<const char *> g_guiRoot{ nullptr };     // outermost traced call on the GUI thread
inline std::atomic<std::thread::id> g_guiThread{};

inline std::string indent(size_t depth) { return std::string(depth * 2, ' '); }

class Scope
{
public:
    explicit Scope(const char *name)
    {
        m_on = enabled();
        if (!m_on)
            return;
        m_frame.name = name;
        m_frame.start = Clock::now();
        m_frame.parent = t_top;
        m_frame.depth = t_top ? t_top->depth + 1 : 0;
        if (!t_top && std::this_thread::get_id() == g_guiThread.load())
            g_guiRoot.store(name);
        t_top = &m_frame;
    }
    ~Scope()
    {
        if (!m_on)
            return;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - m_frame.start).count();
        Frame *parent = m_frame.parent;
        const size_t depth = m_frame.depth;
        t_top = parent;
        if (ms >= kSlowMs) {
            char head[160];
            std::snprintf(head, sizeof head, "%s%s %.0f ms", indent(depth).c_str(), m_frame.name, ms);
            std::vector<std::string> block{ head };
            block.insert(block.end(), m_frame.lines.begin(), m_frame.lines.end());
            if (m_frame.quickCalls > 0) {
                char quick[96];
                std::snprintf(quick, sizeof quick, "%s(+%d quick call%s, %.0f ms)", indent(depth + 1).c_str(),
                              m_frame.quickCalls, m_frame.quickCalls == 1 ? "" : "s", m_frame.quickMs);
                block.emplace_back(quick);
            }
            if (parent) {
                parent->lines.insert(parent->lines.end(), block.begin(), block.end());
            } else {
                for (const std::string &line : block)
                    std::fprintf(stderr, "[slow] %s\n", line.c_str());
                std::fflush(stderr);
            }
        } else if (parent) {
            ++parent->quickCalls;
            parent->quickMs += ms;
        }
        if (!parent && std::this_thread::get_id() == g_guiThread.load())
            g_guiRoot.store(nullptr);
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    bool  m_on = false;
    Frame m_frame;
};

#ifdef __linux__
// Mid-stall, the watcher signals the GUI thread, which prints where it is:
// "[stall-trace]" frames on stderr (names readable thanks to -rdynamic; run
// them through c++filt). backtrace_symbols_fd() doesn't allocate, and
// backtrace() is primed at start so its first call doesn't load libgcc here.
inline pthread_t g_guiPthread{};
inline void stallTraceHandler(int)
{
    void *frames[48];
    const int n = backtrace(frames, 48);
    static const char head[] = "[stall-trace] interface thread is here:\n";
    (void)!write(2, head, sizeof head - 1);
    backtrace_symbols_fd(frames, n, 2);
    static const char tail[] = "[stall-trace] end\n";
    (void)!write(2, tail, sizeof tail - 1);
}
#endif

// Call once on the GUI thread after QApplication exists.
inline void startStallWatch(QCoreApplication *app)
{
    if (!enabled())
        return;
#ifdef __linux__
    g_guiPthread = pthread_self();
    {
        void *prime[2];
        backtrace(prime, 2);
    }
    struct sigaction sa {};
    sa.sa_handler = stallTraceHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);
#endif
    static std::atomic<long long> heartbeat{ 0 };
    const auto nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    };
    g_guiThread.store(std::this_thread::get_id());
    heartbeat.store(nowMs());
    auto *beat = new QTimer(app);
    beat->setInterval(20);
    QObject::connect(beat, &QTimer::timeout, app, [nowMs] { heartbeat.store(nowMs()); });
    beat->start();
    std::thread([nowMs] {
        bool stalled = false;
        long long since = 0;
        const char *during = nullptr;
        int tracesThisStall = 0, tracesTotal = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            const long long last = heartbeat.load();
            const long long gap = nowMs() - last;
            if (!stalled && gap >= kStallMs) {
                stalled = true;
                since = last;
                during = g_guiRoot.load();
                tracesThisStall = 0;
            }
#ifdef __linux__
            // Where it's stuck: a trace at ~200 ms and ~450 ms into a stall
            // (two, in case it moves on), at most 20 per session.
            if (stalled && tracesTotal < 20
                && ((tracesThisStall == 0 && gap >= 200) || (tracesThisStall == 1 && gap >= 450))) {
                ++tracesThisStall;
                ++tracesTotal;
                pthread_kill(g_guiPthread, SIGUSR2);
            }
#endif
            if (stalled && gap < kStallMs) {
                stalled = false;
                std::fprintf(stderr, "[stall] interface blocked %lld ms%s%s\n", last - since,
                             during ? " in " : " (untraced work)", during ? during : "");
                std::fflush(stderr);
            }
        }
    }).detach();
}

} // namespace PerfTrace

#define VSX_TRACE_CAT2(a, b) a##b
#define VSX_TRACE_CAT(a, b) VSX_TRACE_CAT2(a, b)
#define VSX_TRACE(name) PerfTrace::Scope VSX_TRACE_CAT(vsxTrace_, __LINE__)(name)

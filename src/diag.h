#pragma once
//
// Diagnostics on CAN, Qt-free (tested on its own): ISO-TP (ISO 15765-2)
// reassembly, then what the messages mean — OBD-II (SAE J1979 modes, PIDs,
// fault codes, vehicle info) and UDS (ISO 14229 services, data identifiers,
// fault codes with status, negative responses).
//
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Diag {

inline std::string fmt(const char *f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ---- fault codes ------------------------------------------------------------

// Two bytes to "P0301": the top two bits pick P / C / B / U, the next two
// the first digit, then three hex digits.
inline std::string dtcCode(int hi, int lo)
{
    static const char letters[] = { 'P', 'C', 'B', 'U' };
    return fmt("%c%d%X%X%X", letters[(hi >> 6) & 3], (hi >> 4) & 3, hi & 15, (lo >> 4) & 15, lo & 15);
}

// Common generic (SAE J2012) codes; others get their category.
inline std::string dtcDescription(const std::string &code)
{
    static const std::map<std::string, const char *> known = {
        { "P0010", "Intake camshaft position actuator circuit (bank 1)" },
        { "P0011", "Intake camshaft timing over-advanced or system performance (bank 1)" },
        { "P0016", "Crankshaft / camshaft position correlation (bank 1 sensor A)" },
        { "P0087", "Fuel rail / system pressure too low" },
        { "P0088", "Fuel rail / system pressure too high" },
        { "P0100", "Mass or volume air flow circuit" },
        { "P0101", "Mass or volume air flow circuit range / performance" },
        { "P0102", "Mass or volume air flow circuit low input" },
        { "P0103", "Mass or volume air flow circuit high input" },
        { "P0106", "Manifold absolute / barometric pressure circuit range / performance" },
        { "P0107", "Manifold absolute / barometric pressure circuit low input" },
        { "P0108", "Manifold absolute / barometric pressure circuit high input" },
        { "P0112", "Intake air temperature circuit low input" },
        { "P0113", "Intake air temperature circuit high input" },
        { "P0115", "Engine coolant temperature circuit" },
        { "P0116", "Engine coolant temperature circuit range / performance" },
        { "P0117", "Engine coolant temperature circuit low input" },
        { "P0118", "Engine coolant temperature circuit high input" },
        { "P0121", "Throttle / pedal position sensor A circuit range / performance" },
        { "P0122", "Throttle / pedal position sensor A circuit low input" },
        { "P0123", "Throttle / pedal position sensor A circuit high input" },
        { "P0125", "Insufficient coolant temperature for closed-loop fuel control" },
        { "P0128", "Coolant thermostat (coolant below thermostat regulating temperature)" },
        { "P0130", "O2 sensor circuit (bank 1 sensor 1)" },
        { "P0131", "O2 sensor circuit low voltage (bank 1 sensor 1)" },
        { "P0132", "O2 sensor circuit high voltage (bank 1 sensor 1)" },
        { "P0133", "O2 sensor circuit slow response (bank 1 sensor 1)" },
        { "P0134", "O2 sensor circuit no activity detected (bank 1 sensor 1)" },
        { "P0135", "O2 sensor heater circuit (bank 1 sensor 1)" },
        { "P0141", "O2 sensor heater circuit (bank 1 sensor 2)" },
        { "P0171", "System too lean (bank 1)" },
        { "P0172", "System too rich (bank 1)" },
        { "P0174", "System too lean (bank 2)" },
        { "P0175", "System too rich (bank 2)" },
        { "P0190", "Fuel rail pressure sensor circuit" },
        { "P0234", "Turbocharger / supercharger overboost condition" },
        { "P0299", "Turbocharger / supercharger underboost" },
        { "P0300", "Random / multiple cylinder misfire detected" },
        { "P0325", "Knock sensor 1 circuit (bank 1 or single sensor)" },
        { "P0335", "Crankshaft position sensor A circuit" },
        { "P0336", "Crankshaft position sensor A circuit range / performance" },
        { "P0340", "Camshaft position sensor A circuit (bank 1 or single sensor)" },
        { "P0380", "Glow plug / heater circuit A" },
        { "P0400", "Exhaust gas recirculation flow" },
        { "P0401", "Exhaust gas recirculation flow insufficient" },
        { "P0402", "Exhaust gas recirculation flow excessive" },
        { "P0420", "Catalyst system efficiency below threshold (bank 1)" },
        { "P0430", "Catalyst system efficiency below threshold (bank 2)" },
        { "P0440", "Evaporative emission system" },
        { "P0441", "Evaporative emission system incorrect purge flow" },
        { "P0442", "Evaporative emission system leak detected (small leak)" },
        { "P0446", "Evaporative emission system vent control circuit" },
        { "P0455", "Evaporative emission system leak detected (large leak)" },
        { "P0456", "Evaporative emission system leak detected (very small leak)" },
        { "P0500", "Vehicle speed sensor A" },
        { "P0505", "Idle air control system" },
        { "P0506", "Idle air control system RPM lower than expected" },
        { "P0507", "Idle air control system RPM higher than expected" },
        { "P0562", "System voltage low" },
        { "P0563", "System voltage high" },
        { "P0571", "Brake switch A circuit" },
        { "P0601", "Internal control module memory checksum error" },
        { "P0606", "Control module processor" },
        { "P0700", "Transmission control system (MIL request)" },
        { "P0705", "Transmission range sensor A circuit (PRNDL input)" },
        { "P0715", "Input / turbine speed sensor A circuit" },
        { "P0720", "Output speed sensor circuit" },
        { "P0741", "Torque converter clutch circuit performance / stuck off" },
        { "U0001", "High-speed CAN communication bus" },
        { "U0100", "Lost communication with ECM / PCM A" },
        { "U0101", "Lost communication with TCM" },
        { "U0121", "Lost communication with ABS control module" },
        { "U0140", "Lost communication with body control module" },
        { "U0155", "Lost communication with instrument cluster" },
    };
    const auto it = known.find(code);
    if (it != known.end())
        return it->second;
    if (code.size() == 5) {
        if (code[0] == 'P' && code[1] == '0' && code[2] == '3' && code[3] == '0' && code[4] >= '1' && code[4] <= '9')
            return fmt("Cylinder %c misfire detected", code[4]);
        if (code.compare(0, 4, "P031") == 0 && code[4] >= '0' && code[4] <= '2')
            return fmt("Cylinder 1%c misfire detected", code[4]);
        if (code.compare(0, 3, "P02") == 0 && code[3] <= '1' && !(code[3] == '0' && code[4] == '0')) {
            const int cyl = (code[3] - '0') * 10 + (code[4] - '0');
            if (cyl >= 1 && cyl <= 12)
                return fmt("Injector circuit / open, cylinder %d", cyl);
        }
        if (code.compare(0, 4, "P035") == 0 && code[4] >= '1' && code[4] <= '8')
            return fmt("Ignition coil %c primary / secondary circuit", 'A' + (code[4] - '1'));
    }
    static const char *areas[] = { "powertrain", "chassis", "body", "network" };
    const int area = code.empty() ? 0 : code[0] == 'C' ? 1 : code[0] == 'B' ? 2 : code[0] == 'U' ? 3 : 0;
    const bool generic = code.size() > 1 && (code[1] == '0' || (code[0] == 'P' && code[1] == '2'));
    return fmt("%s %s code", generic ? "Generic" : "Manufacturer-specific", areas[area]);
}

// ---- OBD-II mode 01 PIDs ----------------------------------------------------

struct Pid {
    int pid;
    const char *name, *unit;
    int bytes;
    double (*value)(int a, int b);
};

inline const Pid *findPid(int pid)
{
    static const Pid table[] = {
        { 0x04, "Engine load", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x05, "Coolant temp", "\xc2\xb0" "C", 1, [](int a, int) { return a - 40.0; } },
        { 0x06, "Short fuel trim B1", "%", 1, [](int a, int) { return (a - 128) * 100.0 / 128.0; } },
        { 0x07, "Long fuel trim B1", "%", 1, [](int a, int) { return (a - 128) * 100.0 / 128.0; } },
        { 0x08, "Short fuel trim B2", "%", 1, [](int a, int) { return (a - 128) * 100.0 / 128.0; } },
        { 0x09, "Long fuel trim B2", "%", 1, [](int a, int) { return (a - 128) * 100.0 / 128.0; } },
        { 0x0A, "Fuel pressure", "kPa", 1, [](int a, int) { return 3.0 * a; } },
        { 0x0B, "Intake manifold pressure", "kPa", 1, [](int a, int) { return double(a); } },
        { 0x0C, "Engine speed", "rpm", 2, [](int a, int b) { return (256.0 * a + b) / 4.0; } },
        { 0x0D, "Vehicle speed", "km/h", 1, [](int a, int) { return double(a); } },
        { 0x0E, "Timing advance", "\xc2\xb0", 1, [](int a, int) { return a / 2.0 - 64.0; } },
        { 0x0F, "Intake air temp", "\xc2\xb0" "C", 1, [](int a, int) { return a - 40.0; } },
        { 0x10, "Air flow (MAF)", "g/s", 2, [](int a, int b) { return (256.0 * a + b) / 100.0; } },
        { 0x11, "Throttle position", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x1F, "Run time since start", "s", 2, [](int a, int b) { return 256.0 * a + b; } },
        { 0x21, "Distance with MIL on", "km", 2, [](int a, int b) { return 256.0 * a + b; } },
        { 0x22, "Fuel rail pressure (vs manifold)", "kPa", 2, [](int a, int b) { return 0.079 * (256.0 * a + b); } },
        { 0x23, "Fuel rail gauge pressure", "kPa", 2, [](int a, int b) { return 10.0 * (256.0 * a + b); } },
        { 0x2C, "Commanded EGR", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x2F, "Fuel tank level", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x30, "Warm-ups since codes cleared", "", 1, [](int a, int) { return double(a); } },
        { 0x31, "Distance since codes cleared", "km", 2, [](int a, int b) { return 256.0 * a + b; } },
        { 0x33, "Barometric pressure", "kPa", 1, [](int a, int) { return double(a); } },
        { 0x3C, "Catalyst temp B1S1", "\xc2\xb0" "C", 2, [](int a, int b) { return (256.0 * a + b) / 10.0 - 40.0; } },
        { 0x42, "Control module voltage", "V", 2, [](int a, int b) { return (256.0 * a + b) / 1000.0; } },
        { 0x43, "Absolute load", "%", 2, [](int a, int b) { return (256.0 * a + b) * 100.0 / 255.0; } },
        { 0x44, "Commanded lambda", "", 2, [](int a, int b) { return (256.0 * a + b) / 32768.0; } },
        { 0x45, "Relative throttle position", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x46, "Ambient air temp", "\xc2\xb0" "C", 1, [](int a, int) { return a - 40.0; } },
        { 0x47, "Absolute throttle position B", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x49, "Accelerator pedal position D", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x4A, "Accelerator pedal position E", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x4C, "Commanded throttle actuator", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x4D, "Time run with MIL on", "min", 2, [](int a, int b) { return 256.0 * a + b; } },
        { 0x4E, "Time since codes cleared", "min", 2, [](int a, int b) { return 256.0 * a + b; } },
        { 0x52, "Ethanol fuel", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x5A, "Relative accelerator pedal position", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x5B, "Hybrid battery remaining life", "%", 1, [](int a, int) { return a * 100.0 / 255.0; } },
        { 0x5C, "Engine oil temp", "\xc2\xb0" "C", 1, [](int a, int) { return a - 40.0; } },
        { 0x5E, "Engine fuel rate", "L/h", 2, [](int a, int b) { return (256.0 * a + b) / 20.0; } },
        { 0x61, "Driver demand torque", "%", 1, [](int a, int) { return a - 125.0; } },
        { 0x62, "Actual engine torque", "%", 1, [](int a, int) { return a - 125.0; } },
        { 0x63, "Engine reference torque", "Nm", 2, [](int a, int b) { return 256.0 * a + b; } },
    };
    for (const Pid &p : table) {
        if (p.pid == pid)
            return &p;
    }
    return nullptr;
}

// Data bytes of PIDs the table above doesn't scale (so a multi-PID response
// can still be walked past them).
inline int pidBytes(int pid)
{
    if (const Pid *p = findPid(pid))
        return p->bytes;
    if (pid % 0x20 == 0)
        return 4;                               // supported-PID bitmaps
    switch (pid) {
    case 0x01: case 0x41: return 4;             // monitor status
    case 0x03: return 2;                        // fuel system status
    case 0x12: case 0x13: case 0x1C: case 0x1D: case 0x1E: case 0x51: return 1;
    default: break;
    }
    if (pid >= 0x14 && pid <= 0x1B)
        return 2;                               // O2 sensor voltage + trim
    return -1;
}

inline std::string trimNumber(double v)
{
    return fmt(v == double(static_cast<long long>(v)) ? "%.0f" : (std::fabs(v) < 10 ? "%.2f" : "%.1f"), v);
}

// ---- UDS --------------------------------------------------------------------

inline const char *serviceName(int sid)
{
    switch (sid) {
    case 0x10: return "Diagnostic session control";
    case 0x11: return "ECU reset";
    case 0x14: return "Clear diagnostic information";
    case 0x19: return "Read DTC information";
    case 0x22: return "Read data by identifier";
    case 0x23: return "Read memory by address";
    case 0x24: return "Read scaling data by identifier";
    case 0x27: return "Security access";
    case 0x28: return "Communication control";
    case 0x29: return "Authentication";
    case 0x2A: return "Read data by periodic identifier";
    case 0x2C: return "Dynamically define data identifier";
    case 0x2E: return "Write data by identifier";
    case 0x2F: return "Input / output control by identifier";
    case 0x31: return "Routine control";
    case 0x34: return "Request download";
    case 0x35: return "Request upload";
    case 0x36: return "Transfer data";
    case 0x37: return "Request transfer exit";
    case 0x38: return "Request file transfer";
    case 0x3D: return "Write memory by address";
    case 0x3E: return "Tester present";
    case 0x83: return "Access timing parameter";
    case 0x84: return "Secured data transmission";
    case 0x85: return "Control DTC setting";
    case 0x86: return "Response on event";
    case 0x87: return "Link control";
    case 0x01: return "OBD current data";
    case 0x02: return "OBD freeze frame data";
    case 0x03: return "OBD stored DTCs";
    case 0x04: return "OBD clear DTCs";
    case 0x05: return "OBD O2 sensor monitoring";
    case 0x06: return "OBD on-board monitoring tests";
    case 0x07: return "OBD pending DTCs";
    case 0x08: return "OBD control of on-board system";
    case 0x09: return "OBD vehicle information";
    case 0x0A: return "OBD permanent DTCs";
    default: return nullptr;
    }
}

inline const char *nrcName(int nrc)
{
    switch (nrc) {
    case 0x10: return "General reject";
    case 0x11: return "Service not supported";
    case 0x12: return "Sub-function not supported";
    case 0x13: return "Incorrect message length or format";
    case 0x14: return "Response too long";
    case 0x21: return "Busy, repeat request";
    case 0x22: return "Conditions not correct";
    case 0x24: return "Request sequence error";
    case 0x25: return "No response from sub-net component";
    case 0x26: return "Failure prevents execution";
    case 0x31: return "Request out of range";
    case 0x33: return "Security access denied";
    case 0x35: return "Invalid key";
    case 0x36: return "Exceeded number of attempts";
    case 0x37: return "Required time delay not expired";
    case 0x70: return "Upload / download not accepted";
    case 0x71: return "Transfer data suspended";
    case 0x72: return "General programming failure";
    case 0x73: return "Wrong block sequence counter";
    case 0x78: return "Response pending";
    case 0x7E: return "Sub-function not supported in active session";
    case 0x7F: return "Service not supported in active session";
    case 0x92: return "Voltage too high";
    case 0x93: return "Voltage too low";
    default: return "Unknown reason";
    }
}

inline const char *didName(int did)
{
    switch (did) {
    case 0xF180: return "Boot software ID";
    case 0xF181: return "Application software ID";
    case 0xF182: return "Application data ID";
    case 0xF186: return "Active diagnostic session";
    case 0xF187: return "Spare part number";
    case 0xF188: return "ECU software number";
    case 0xF189: return "ECU software version";
    case 0xF18A: return "System supplier ID";
    case 0xF18B: return "ECU manufacturing date";
    case 0xF18C: return "ECU serial number";
    case 0xF190: return "VIN";
    case 0xF191: return "ECU hardware number";
    case 0xF192: return "Supplier hardware number";
    case 0xF193: return "Supplier hardware version";
    case 0xF194: return "Supplier software number";
    case 0xF195: return "Supplier software version";
    case 0xF197: return "System name";
    case 0xF198: return "Repair shop code";
    case 0xF199: return "Programming date";
    case 0xF19E: return "ODX file";
    default: return nullptr;
    }
}

inline std::string hex(const std::vector<std::uint8_t> &b, size_t from, size_t max = 12)
{
    std::string s;
    for (size_t i = from; i < b.size() && i < from + max; ++i)
        s += fmt(i == from ? "%02X" : " %02X", b[i]);
    if (b.size() > from + max)
        s += " \xe2\x80\xa6";
    return s;
}

// Printable ASCII, or empty if it isn't text.
inline std::string ascii(const std::vector<std::uint8_t> &b, size_t from)
{
    std::string s;
    for (size_t i = from; i < b.size(); ++i) {
        const int c = b[i];
        if (c == 0 || c == 0xFF)
            continue;
        if (c < 0x20 || c > 0x7E)
            return std::string();
        s += char(c);
    }
    return s;
}

// ---- what a message says ----------------------------------------------------

struct Meaning {
    std::string text;                 // full sentence
    std::string brief;                // a few words for narrow boxes
    bool negative = false, pending = false, error = false;
    int  sid = -1, nrc = -1;
    std::vector<std::pair<std::string, std::string>> values;   // live data: name, value with unit
    std::vector<std::string> dtcs;    // "P0301 Cylinder 1 misfire detected"
    std::string vin;
};

inline bool isResponseSid(int sid) { return sid == 0x7F || (sid >= 0x40 && !(sid >= 0x83 && sid <= 0x88)); }

inline Meaning describe(const std::vector<std::uint8_t> &p)
{
    Meaning m;
    if (p.empty()) {
        m.text = m.brief = "(empty)";
        return m;
    }
    const int sid = p[0];
    m.sid = sid;
    const auto name = [](int s) { const char *n = serviceName(s); return std::string(n ? n : fmt("Service 0x%02X", s)); };
    // Negative response.
    if (sid == 0x7F) {
        const int req = p.size() > 1 ? p[1] : -1, nrc = p.size() > 2 ? p[2] : -1;
        m.nrc = nrc;
        m.pending = nrc == 0x78;
        m.negative = !m.pending;
        m.text = fmt("Negative response to %s: %s (0x%02X)", name(req).c_str(), nrcName(nrc), nrc);
        m.brief = m.pending ? "Pending" : fmt("NRC %02X", nrc);
        return m;
    }
    const bool response = isResponseSid(sid);
    const int base = response ? sid - 0x40 : sid;
    // ---- OBD-II ----
    if (base >= 0x01 && base <= 0x0A) {
        if (!response) {
            std::string pids;
            if (base == 0x01 || base == 0x02 || base == 0x09) {
                for (size_t i = 1; i < p.size(); ++i) {
                    const Pid *pd = base == 0x01 ? findPid(p[i]) : nullptr;
                    const char *info = base != 0x09 ? nullptr
                                     : p[i] == 0x02 ? "VIN" : p[i] == 0x04 ? "calibration ID"
                                     : p[i] == 0x0A ? "ECU name" : p[i] == 0x00 ? "supported info types" : nullptr;
                    pids += pids.empty() ? ": " : ", ";
                    pids += pd ? pd->name : info ? info : fmt("PID %02X", p[i]);
                }
            }
            m.text = "Request: " + name(base) + pids;
            m.brief = fmt("Req %02X", base);
            return m;
        }
        if (base == 0x01 || base == 0x02) {
            // PID, (frame number for mode 02), data — possibly several PIDs.
            size_t i = 1;
            std::string parts;
            while (i < p.size()) {
                const int pid = p[i++];
                if (base == 0x02 && i < p.size())
                    ++i;                                          // freeze frame number
                const int n = pidBytes(pid);
                if (n < 0 || i + size_t(n) > p.size()) {
                    parts += (parts.empty() ? "" : " \xc2\xb7 ") + fmt("PID %02X: %s", pid, hex(p, i - 1).c_str());
                    break;
                }
                std::string item;
                if (const Pid *pd = findPid(pid)) {
                    const std::string v = trimNumber(pd->value(p[i], n > 1 ? p[i + 1] : 0)) + (pd->unit[0] ? std::string(" ") + pd->unit : "");
                    item = std::string(pd->name) + " " + v;
                    m.values.emplace_back(pd->name, v);
                } else if (pid % 0x20 == 0) {
                    std::string list;
                    for (int bit = 0; bit < 32; ++bit) {
                        if (p[i + size_t(bit / 8)] & (0x80 >> (bit % 8)))
                            list += fmt(list.empty() ? "%02X" : " %02X", pid + bit + 1);
                    }
                    item = fmt("Supported PIDs %02X-%02X: %s", pid + 1, pid + 32, list.c_str());
                } else if (pid == 0x01) {
                    item = fmt("MIL %s, %d DTC%s", (p[i] & 0x80) ? "on" : "off", p[i] & 0x7F, (p[i] & 0x7F) == 1 ? "" : "s");
                    m.values.emplace_back("MIL", (p[i] & 0x80) ? "on" : "off");
                    m.values.emplace_back("DTC count", std::to_string(p[i] & 0x7F));
                } else if (pid >= 0x14 && pid <= 0x1B) {
                    const std::string v = trimNumber(p[i] / 200.0) + " V";
                    item = fmt("O2 sensor %d ", pid - 0x13) + v;
                    m.values.emplace_back(fmt("O2 sensor %d", pid - 0x13), v);
                } else {
                    item = fmt("PID %02X: %s", pid, hex(p, i, size_t(n)).c_str());
                }
                parts += (parts.empty() ? "" : " \xc2\xb7 ") + item;
                i += size_t(n);
            }
            m.text = parts.empty() ? name(base) : parts;
            m.brief = m.values.empty() ? fmt("Resp %02X", sid) : m.values.front().first + " " + m.values.front().second;
            return m;
        }
        if (base == 0x03 || base == 0x07 || base == 0x0A) {
            const char *kind = base == 0x03 ? "Stored" : base == 0x07 ? "Pending" : "Permanent";
            // ISO 15765-4: a count byte, then two bytes per code.
            size_t i = 2;
            for (; i + 1 < p.size(); i += 2) {
                if (p[i] == 0 && p[i + 1] == 0)
                    continue;
                const std::string code = dtcCode(p[i], p[i + 1]);
                m.dtcs.push_back(code + " " + dtcDescription(code));
            }
            m.text = fmt("%s DTCs (%zu)", kind, m.dtcs.size());
            for (const std::string &d : m.dtcs)
                m.text += (&d == &m.dtcs.front() ? ": " : ", ") + d;
            m.brief = fmt("%zu DTC%s", m.dtcs.size(), m.dtcs.size() == 1 ? "" : "s");
            return m;
        }
        if (base == 0x04) {
            m.text = "DTCs cleared";
            m.brief = "Cleared";
            return m;
        }
        if (base == 0x09 && p.size() >= 2) {
            const int info = p[1];
            const std::string s = ascii(p, 3);
            if (info == 0x02 && !s.empty()) {
                m.vin = s;
                m.text = "VIN " + s;
            } else if (info == 0x04 && !s.empty()) {
                m.text = "Calibration ID " + s;
            } else if (info == 0x0A && !s.empty()) {
                m.text = "ECU name " + s;
            } else {
                m.text = fmt("Vehicle info %02X: %s", info, hex(p, 2).c_str());
            }
            m.brief = info == 0x02 ? "VIN" : fmt("Info %02X", info);
            return m;
        }
        m.text = name(base) + ": " + hex(p, 1);
        m.brief = fmt("Resp %02X", sid);
        return m;
    }
    // ---- UDS ----
    std::string detail;
    if (base == 0x10 && p.size() > 1) {
        static const char *sessions[] = { "?", "default", "programming", "extended" };
        const int s = p[1] & 0x7F;
        detail = fmt(": %s session", s <= 3 ? sessions[s] : fmt("0x%02X", s).c_str());
    } else if (base == 0x11 && p.size() > 1) {
        static const char *resets[] = { "?", "hard", "key off / on", "soft" };
        const int r = p[1] & 0x7F;
        detail = fmt(": %s reset", r <= 3 ? resets[r] : fmt("0x%02X", r).c_str());
    } else if ((base == 0x22 || base == 0x2E || base == 0x2F) && p.size() >= 3) {
        const int did = p[1] << 8 | p[2];
        const char *dn = didName(did);
        detail = fmt(": %04X%s%s", did, dn ? " " : "", dn ? dn : "");
        if (response && base == 0x22 && p.size() > 3) {
            const std::string s = ascii(p, 3);
            detail += " = " + (s.size() >= 2 ? s : hex(p, 3));
            if (did == 0xF190 && s.size() >= 11)
                m.vin = s;
        } else if (!response && base == 0x22) {
            for (size_t i = 3; i + 1 < p.size(); i += 2) {
                const int more = p[i] << 8 | p[i + 1];
                const char *mn = didName(more);
                detail += fmt(", %04X%s%s", more, mn ? " " : "", mn ? mn : "");
            }
        }
    } else if (base == 0x27 && p.size() > 1) {
        const int level = p[1] & 0x7F;
        detail = fmt(": %s, level %d", (level & 1) ? "seed" : "key", (level + 1) / 2);
        if (p.size() > 2)
            detail += " " + hex(p, 2);
    } else if (base == 0x31 && p.size() >= 4) {
        static const char *types[] = { "?", "start", "stop", "results" };
        const int t = p[1] & 0x7F;
        detail = fmt(": %s routine %04X", t <= 3 ? types[t] : "?", p[2] << 8 | p[3]);
    } else if (base == 0x3E) {
        detail = "";
    } else if (base == 0x85 && p.size() > 1) {
        detail = (p[1] & 0x7F) == 1 ? ": on" : (p[1] & 0x7F) == 2 ? ": off" : "";
    } else if (base == 0x14 && p.size() >= 4) {
        const int group = p[1] << 16 | p[2] << 8 | p[3];
        detail = group == 0xFFFFFF ? ": all groups" : fmt(": group %06X", group);
    } else if (base == 0x19 && p.size() > 1) {
        const int sub = p[1];
        static const std::map<int, const char *> subs = {
            { 0x01, "number of DTCs by status mask" }, { 0x02, "DTCs by status mask" },
            { 0x04, "snapshot record" }, { 0x06, "extended data record" }, { 0x0A, "supported DTCs" } };
        const auto it = subs.find(sub);
        detail = fmt(": %s", it != subs.end() ? it->second : fmt("sub-function %02X", sub).c_str());
        if (response && (sub == 0x02 || sub == 0x0A) && p.size() >= 3) {
            // Availability mask, then three-byte codes with a status byte each.
            for (size_t i = 3; i + 3 < p.size(); i += 4) {             // code (3 bytes) + status
                const std::string code = dtcCode(p[i], p[i + 1]);
                const int status = p[i + 3];
                std::string flags;
                if (status & 0x08) flags += "confirmed";
                if (status & 0x04) flags += flags.empty() ? "pending" : ", pending";
                if (status & 0x80) flags += flags.empty() ? "MIL" : ", MIL";
                m.dtcs.push_back(fmt("%s-%02X %s%s%s%s", code.c_str(), p[i + 2], dtcDescription(code).c_str(),
                                     flags.empty() ? "" : " (", flags.c_str(), flags.empty() ? "" : ")"));
            }
            detail += fmt(" \xe2\x80\x94 %zu DTC%s", m.dtcs.size(), m.dtcs.size() == 1 ? "" : "s");
            for (const std::string &d : m.dtcs)
                detail += (&d == &m.dtcs.front() ? ": " : ", ") + d;
        } else if (response && sub == 0x01 && p.size() >= 6) {
            detail += fmt(" \xe2\x80\x94 %d", p[4] << 8 | p[5]);
        }
    } else if (p.size() > 1) {
        detail = ": " + hex(p, 1);
    }
    m.text = (response ? "OK: " : "") + name(base) + detail;
    m.brief = (response ? "OK " : "") + fmt("%02X", base);
    if (!serviceName(base))
        m.text = (response ? "Response " : "Request ") + hex(p, 0);
    return m;
}

// ---- ISO-TP over CAN --------------------------------------------------------

struct Frame {
    long long start = 0, end = 0;
    std::uint32_t id = 0;
    bool extended = false, canError = false;
    std::vector<std::uint8_t> data;
};

struct Note {                         // one per CAN frame shown
    size_t frame = 0;
    std::string text, brief, tiny;
    bool error = false;
};

struct Message {
    long long start = 0, end = 0;
    std::uint32_t id = 0;
    bool extended = false, request = false, complete = true;
    std::vector<std::uint8_t> payload;
    Meaning meaning;
};

struct Result {
    std::vector<Note> notes;
    std::vector<Message> messages;
};

// 0: the standard OBD-II / UDS IDs; 1: also 0x600-0x7FF; 2: every ID that
// looks like ISO-TP.
inline bool diagnosticId(std::uint32_t id, bool extended, int mode)
{
    if (extended) {
        const std::uint32_t top = id >> 16;
        return top == 0x18DA || top == 0x18DB || mode == 2;
    }
    if (id == 0x7DF || (id >= 0x7E0 && id <= 0x7EF))
        return true;
    if (mode >= 1 && id >= 0x600 && id <= 0x7FF)
        return true;
    return mode == 2;
}

inline std::string idText(std::uint32_t id, bool extended)
{
    return extended ? fmt("%08X", id) : fmt("%03X", id);
}

inline Result analyse(const std::vector<Frame> &frames, int idMode)
{
    Result r;
    struct Assembly {
        bool active = false;
        size_t firstFrame = 0, note = 0;
        size_t expected = 0;
        int nextSn = 1;
        Message msg;
    };
    std::map<std::uint64_t, Assembly> open;
    const auto finish = [&](Assembly &a) {
        Message &m = a.msg;
        m.meaning = describe(m.payload);
        if (!m.complete)
            m.meaning.text += " (incomplete)";
        m.request = !isResponseSid(m.meaning.sid);
        Note &n = r.notes[a.note];
        const std::string dir = (m.request ? "\xe2\x86\x92 " : "\xe2\x86\x90 ") + idText(m.id, m.extended);
        n.text = dir + " \xc2\xb7 " + m.meaning.text;
        n.brief = dir + " " + m.meaning.brief;
        n.tiny = m.meaning.brief;
        n.error = n.error || m.meaning.negative || !m.complete;
        r.messages.push_back(m);
        a.active = false;
    };
    for (size_t k = 0; k < frames.size(); ++k) {
        const Frame &f = frames[k];
        if (!diagnosticId(f.id, f.extended, idMode) || f.data.empty())
            continue;
        const std::vector<std::uint8_t> &d = f.data;
        const int type = d[0] >> 4;
        const std::uint64_t key = (std::uint64_t(f.extended) << 32) | f.id;
        Note note;
        note.frame = k;
        note.error = f.canError;
        if (type == 0) {                                          // single frame
            size_t len = d[0] & 0x0F, off = 1;
            if (len == 0 && d.size() >= 2) {                     // CAN FD: the length in the next byte
                len = d[1];
                off = 2;
            }
            if (len == 0 || off + len > d.size()) {
                if (idMode == 2)
                    continue;
                note.text = note.brief = "Not ISO-TP (bad single-frame length)";
                note.tiny = "?";
                note.error = true;
                r.notes.push_back(note);
                continue;
            }
            Assembly a;
            a.note = r.notes.size();
            r.notes.push_back(note);
            a.msg.start = f.start;
            a.msg.end = f.end;
            a.msg.id = f.id;
            a.msg.extended = f.extended;
            a.msg.payload.assign(d.begin() + std::ptrdiff_t(off), d.begin() + std::ptrdiff_t(off + len));
            finish(a);
        } else if (type == 1 && d.size() >= 2) {                  // first frame
            size_t len = size_t(d[0] & 0x0F) << 8 | d[1], off = 2;
            if (len == 0 && d.size() >= 6) {
                len = size_t(d[2]) << 24 | size_t(d[3]) << 16 | size_t(d[4]) << 8 | d[5];
                off = 6;
            }
            Assembly &a = open[key];
            if (a.active) {                                       // the previous one never finished
                a.msg.complete = false;
                r.notes[a.note].error = true;
                finish(a);
            }
            a = Assembly();
            a.active = true;
            a.note = r.notes.size();
            note.text = note.brief = fmt("First frame of %zu bytes (rest not in view)", len);
            note.tiny = "FF";
            r.notes.push_back(note);
            a.expected = len;
            a.msg.start = f.start;
            a.msg.end = f.end;
            a.msg.id = f.id;
            a.msg.extended = f.extended;
            a.msg.payload.assign(d.begin() + std::ptrdiff_t(off), d.end());
        } else if (type == 2) {                                   // consecutive frame
            const int sn = d[0] & 0x0F;
            auto it = open.find(key);
            if (it == open.end() || !it->second.active) {
                note.text = fmt("Consecutive frame %d (its first frame is before the view)", sn);
                note.brief = fmt("CF %d", sn);
                note.tiny = "CF";
                r.notes.push_back(note);
                continue;
            }
            Assembly &a = it->second;
            if (sn != a.nextSn) {
                note.text = fmt("Consecutive frame %d out of sequence (expected %d)", sn, a.nextSn);
                note.brief = "CF sequence!";
                note.tiny = "CF!";
                note.error = true;
                r.notes.push_back(note);
                a.msg.complete = false;
                finish(a);
                continue;
            }
            a.nextSn = (sn + 1) & 0x0F;
            for (size_t i = 1; i < d.size() && a.msg.payload.size() < a.expected; ++i)
                a.msg.payload.push_back(d[i]);
            a.msg.end = f.end;
            const size_t got = a.msg.payload.size();
            note.text = fmt("Consecutive frame %d (%zu / %zu bytes)", sn, std::min(got, a.expected), a.expected);
            note.brief = fmt("CF %d", sn);
            note.tiny = "CF";
            r.notes.push_back(note);
            if (got >= a.expected) {
                a.msg.payload.resize(a.expected);
                finish(a);
            }
        } else if (type == 3 && d.size() >= 3) {                  // flow control
            static const char *status[] = { "continue", "wait", "overflow" };
            const int fs = d[0] & 0x0F, st = d[2];
            const std::string stmin = st <= 0x7F ? fmt("%d ms", st)
                                    : (st >= 0xF1 && st <= 0xF9) ? fmt("%d00 \xc2\xb5s", st - 0xF0) : "?";
            note.text = fmt("Flow control: %s, block size %d, STmin %s", fs <= 2 ? status[fs] : "?", d[1], stmin.c_str());
            note.brief = "Flow control";
            note.tiny = "FC";
            note.error = note.error || fs == 2;
            r.notes.push_back(note);
        } else if (idMode != 2) {
            note.text = note.brief = "Not ISO-TP";
            note.tiny = "?";
            note.error = true;
            r.notes.push_back(note);
        }
    }
    for (auto &[key, a] : open) {                                 // cut off by the view's end
        if (a.active) {
            a.msg.complete = false;
            finish(a);
            r.notes[a.note].text += " (rest not in view)";
            r.notes[a.note].error = false;
        }
    }
    return r;
}

// The ECU's answer time: from a request's end to the next response from an
// ECU it addressed (7DF: any of 7E8-7EF; 7E0+n: 7E8+n; 18DA tt ss: 18DA ss tt).
inline bool answers(const Message &request, const Message &response)
{
    if (!request.request || response.request || request.extended != response.extended)
        return false;
    if (!request.extended) {
        if (request.id == 0x7DF)
            return response.id >= 0x7E8 && response.id <= 0x7EF;
        if (request.id >= 0x7E0 && request.id <= 0x7E7)
            return response.id == request.id + 8;
        return true;
    }
    const std::uint32_t target = (request.id >> 8) & 0xFF, source = request.id & 0xFF;
    if ((request.id >> 16) == 0x18DB)
        return (response.id >> 16) == 0x18DA && (response.id & 0xFF) != source && ((response.id >> 8) & 0xFF) == source;
    return (response.id >> 16) == 0x18DA && ((response.id >> 8) & 0xFF) == source && (response.id & 0xFF) == target;
}

} // namespace Diag

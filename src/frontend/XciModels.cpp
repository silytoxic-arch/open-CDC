#include "frontend/XciModels.h"

#include "core/PathUtil.h"
#include "core/TextEncoding.h"
#include "core/Zip.h"
#include "frontend/vhdl/VhdlParser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <map>
#include <set>
#include <optional>
#include <regex>
#include <sstream>

namespace opencdc::frontend {

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Diagnostic unsupported(std::string message, std::vector<std::string> notes = {}) {
    return Diagnostic{"FE-XCI-UNSUPPORTED", Severity::Warning, std::move(message), {}, std::move(notes)};
}

// ---------------------------------------------------------------------------
// Reading the configuration
// ---------------------------------------------------------------------------

using BoundaryPort = XciPort;

/// Ports of the IP-XACT component file Vivado writes next to an XML .xci
/// (<instance>.xml): resolved widths, disabled ports (isEnabled false) left out.
std::optional<std::vector<BoundaryPort>> parseComponentPorts(const std::string& text) {
    static const std::regex portRe(R"(<spirit:port>([\s\S]*?)</spirit:port>)");
    static const std::regex nameRe(R"(<spirit:name>\s*([^<\s]+)\s*</spirit:name>)");
    static const std::regex dirRe(R"(<spirit:direction>\s*([a-z]+)\s*</spirit:direction>)");
    static const std::regex leftRe(R"(<spirit:left\b(?:[^>"]|"[^"]*")*>\s*(-?[0-9]+)\s*</spirit:left>)");
    static const std::regex rightRe(R"(<spirit:right\b(?:[^>"]|"[^"]*")*>\s*(-?[0-9]+)\s*</spirit:right>)");
    // (the attributes may contain '>' in their dependency expressions)
    static const std::regex enabledRe(R"(>\s*(true|false)\s*</xilinx:isEnabled>)");
    std::vector<BoundaryPort> ports;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), portRe); it != std::sregex_iterator(); ++it) {
        const std::string body = (*it)[1].str();
        std::smatch m;
        if (!std::regex_search(body, m, nameRe))
            continue;
        BoundaryPort p;
        p.name = m[1].str();
        if (!std::regex_search(body, m, dirRe))
            continue;
        p.output = m[1].str() == "out";
        p.inout = m[1].str() == "inout";
        if (std::regex_search(body, m, enabledRe) && m[1].str() == "false")
            continue;
        const bool hasVector = body.find("<spirit:vector>") != std::string::npos;
        if (hasVector) {
            std::smatch l, r;
            if (!std::regex_search(body, l, leftRe) || !std::regex_search(body, r, rightRe))
                return std::nullopt; // an unresolved width: do not guess
            p.range = std::pair{std::stoll(l[1].str()), std::stoll(r[1].str())};
        }
        ports.push_back(std::move(p));
    }
    if (ports.empty())
        return std::nullopt;
    return ports;
}

struct XciInfo {
    std::string instance;
    std::string vendor, library, name, version;
    std::map<std::string, std::string> model; ///< C_* model parameters
    std::map<std::string, std::string> user;  ///< component (user) parameters
    std::vector<BoundaryPort> ports;          ///< JSON format only
    bool json = false;
    /// lower-case bus interface -> its clock port (ASSOCIATED_BUSIF of the clock interfaces)
    std::map<std::string, std::string> busClock;
    /// lower-case port -> its clock port: the ports of each bus interface associated with a clock
    std::map<std::string, std::string> portClock;
};

std::string unescapeXml(std::string s) {
    static const std::pair<const char*, const char*> kEntities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : kEntities)
        for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + 1))
            s.replace(p, std::string_view(from).size(), to);
    return s;
}

Result<XciInfo> parseXml(const std::string& text) {
    XciInfo info;
    std::smatch m;
    static const std::regex instRe(R"(<spirit:instanceName>\s*([^<\s]+)\s*</spirit:instanceName>)");
    if (std::regex_search(text, m, instRe))
        info.instance = m[1].str();
    static const std::regex refRe(R"(<spirit:componentRef\b([^>]*)>)");
    if (std::regex_search(text, m, refRe)) {
        const std::string attrs = m[1].str();
        auto attr = [&](const char* key) {
            std::smatch a;
            const std::regex re(std::string("spirit:") + key + R"re(\s*=\s*"([^"]*)")re");
            return std::regex_search(attrs, a, re) ? a[1].str() : std::string();
        };
        info.vendor = attr("vendor");
        info.library = attr("library");
        info.name = attr("name");
        info.version = attr("version");
    }
    static const std::regex valRe(
        R"re(<spirit:configurableElementValue\s+spirit:referenceId="(MODELPARAM_VALUE|PARAM_VALUE)\.([A-Za-z0-9_]+)"\s*>([^<]*)</spirit:configurableElementValue>)re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), valRe); it != std::sregex_iterator(); ++it) {
        auto& target = (*it)[1].str() == "MODELPARAM_VALUE" ? info.model : info.user;
        target[(*it)[2].str()] = unescapeXml((*it)[3].str());
    }
    if (info.name.empty() || info.instance.empty())
        return Diagnostic{"FE-XCI-PARSE", Severity::Warning,
                          "not a Vivado IP configuration (no component reference or instance name)", {}, {}};
    return info;
}

Result<XciInfo> parseJson(const std::string& text) {
    XciInfo info;
    info.json = true;
    try {
        const auto root = nlohmann::json::parse(text);
        const auto& ip = root.at("ip_inst");
        info.instance = ip.value("xci_name", std::string());
        // "xilinx.com:ip:fifo_generator:13.2"
        std::stringstream ref(ip.value("component_reference", std::string()));
        std::getline(ref, info.vendor, ':');
        std::getline(ref, info.library, ':');
        std::getline(ref, info.name, ':');
        std::getline(ref, info.version, ':');
        auto readParams = [](const nlohmann::json& obj, std::map<std::string, std::string>& out) {
            for (const auto& [key, v] : obj.items()) {
                const nlohmann::json* e = &v;
                if (v.is_array() && !v.empty())
                    e = &v[0];
                if (e->is_object() && e->contains("value")) {
                    const auto& val = (*e)["value"];
                    out[key] = val.is_string() ? val.get<std::string>() : val.dump();
                }
            }
        };
        if (ip.contains("parameters")) {
            const auto& params = ip["parameters"];
            if (params.contains("component_parameters"))
                readParams(params["component_parameters"], info.user);
            if (params.contains("model_parameters"))
                readParams(params["model_parameters"], info.model);
        }
        if (ip.contains("boundary") && ip["boundary"].contains("interfaces"))
            for (const auto& [ifName, itf] : ip["boundary"]["interfaces"].items()) {
                if (!itf.is_object() || itf.value("vlnv", std::string()).find(":signal:clock:") == std::string::npos ||
                    !itf.contains("port_maps") || !itf.contains("parameters"))
                    continue;
                std::string clk;
                for (const auto& [logical, maps] : itf["port_maps"].items()) {
                    const auto& e = maps.is_array() && !maps.empty() ? maps[0] : maps;
                    if (e.is_object() && e.contains("physical_name"))
                        clk = e["physical_name"].get<std::string>();
                }
                const auto& params = itf["parameters"];
                if (clk.empty() || !params.contains("ASSOCIATED_BUSIF"))
                    continue;
                const auto& a = params["ASSOCIATED_BUSIF"];
                const auto& av = a.is_array() && !a.empty() ? a[0] : a;
                if (!av.is_object() || !av.contains("value") || !av["value"].is_string())
                    continue;
                std::stringstream list(av["value"].get<std::string>());
                for (std::string bus; std::getline(list, bus, ':');)
                    if (!bus.empty())
                        info.busClock[lower(bus)] = clk;
            }
        if (ip.contains("boundary") && ip["boundary"].contains("interfaces"))
            for (const auto& [ifName, itf] : ip["boundary"]["interfaces"].items()) {
                const auto clk = info.busClock.find(lower(ifName));
                if (clk == info.busClock.end() || !itf.is_object() || !itf.contains("port_maps"))
                    continue;
                for (const auto& [logical, maps] : itf["port_maps"].items()) {
                    const auto& e = maps.is_array() && !maps.empty() ? maps[0] : maps;
                    if (e.is_object() && e.contains("physical_name") && e["physical_name"].is_string())
                        info.portClock[lower(e["physical_name"].get<std::string>())] = clk->second;
                }
            }
        if (ip.contains("boundary") && ip["boundary"].contains("ports"))
            for (const auto& [name, v] : ip["boundary"]["ports"].items()) {
                const auto& e = v.is_array() && !v.empty() ? v[0] : v;
                BoundaryPort p;
                p.name = name;
                const std::string dirText = lower(e.value("direction", std::string("in")));
                p.output = dirText == "out";
                p.inout = dirText == "inout";
                if (e.contains("size_left") && e.contains("size_right")) {
                    auto num = [](const nlohmann::json& n) {
                        return n.is_string() ? std::stoll(n.get<std::string>()) : n.get<long long>();
                    };
                    p.range = std::pair{num(e["size_left"]), num(e["size_right"])};
                }
                info.ports.push_back(std::move(p));
            }
    }
    catch (const std::exception& e) {
        return Diagnostic{"FE-XCI-PARSE", Severity::Warning, std::string("cannot read the IP configuration: ") + e.what(),
                          {}, {}};
    }
    if (info.name.empty() || info.instance.empty())
        return Diagnostic{"FE-XCI-PARSE", Severity::Warning,
                          "not a Vivado IP configuration (no component reference or xci_name)", {}, {}};
    return info;
}

/// Integer parameter: decimal, 0x.., 0b.., true/false; `def` when absent.
long long num(const std::map<std::string, std::string>& m, const std::string& key, long long def) {
    auto it = m.find(key);
    if (it == m.end())
        return def;
    std::string v = lower(it->second);
    v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char c) { return std::isspace(c) || c == '_'; }), v.end());
    if (v == "true")
        return 1;
    if (v == "false")
        return 0;
    try {
        if (v.starts_with("0b"))
            return std::stoll(v.substr(2), nullptr, 2);
        if (v.starts_with("0x"))
            return std::stoll(v.substr(2), nullptr, 16);
        return std::stoll(v);
    }
    catch (...) {
        return def;
    }
}

unsigned clog2(long long v) {
    unsigned n = 0;
    while ((1LL << n) < v)
        ++n;
    return n;
}

// ---------------------------------------------------------------------------
// FIFO model
// ---------------------------------------------------------------------------

enum class Role {
    Ignore,
    WrClk, RdClk, Clk,                       // Clk: both sides (common clock)
    WrRstAsyncHigh, WrRstAsyncLow, RdRstAsyncHigh, RdRstAsyncLow,
    RstAsyncHigh, RstAsyncLow, RstSync,      // both sides
    WrRstSync, RdRstSync,
    Din, WrEn, RdEn, SValid, SField, MReady,
    Dout, Full, Empty, AlmostFull, AlmostEmpty, ProgFull, ProgEmpty, Valid, WrAck, Overflow, Underflow,
    WrCount, RdCount, WrRstBusy, RdRstBusy, SReady, MValid, MField,
    Zero, // output without a model: constant 0
};

struct Port {
    std::string name;
    bool output = false;
    unsigned width = 1;
    bool vector = false;
    Role role = Role::Ignore;
    unsigned offset = 0; ///< AXI-Stream field: offset in the stored word
    bool countMsbs = false; ///< data count: the most significant bits (FIFO Generator), else the low bits
};

struct FifoSpec {
    std::string module;
    bool independent = true;
    unsigned depthBits = 4;     ///< address bits (depth = 2^depthBits)
    unsigned memWidth = 1;
    unsigned stages = 2;
    bool fwft = false;
    long long progFull = 0, progEmpty = 0;
    std::vector<Port> ports;
};

/// AXI-Stream payload fields in the order they are stored.
constexpr const char* kAxisFields[] = {"tdata", "tstrb", "tkeep", "tlast", "tid", "tdest", "tuser"};

std::string vec(unsigned w) { return "[" + std::to_string(w - 1) + ":0]"; }

std::string generate(const FifoSpec& s, std::vector<std::string>& notes) {
    auto find = [&](Role r) -> const Port* {
        for (const auto& p : s.ports)
            if (p.role == r)
                return &p;
        return nullptr;
    };
    const unsigned AW = s.depthBits;
    const unsigned MW = s.memWidth;
    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n";
    v << "module " << s.module << " (\n";
    for (std::size_t i = 0; i < s.ports.size(); ++i) {
        const auto& p = s.ports[i];
        v << "  " << (p.output ? "output wire " : "input  wire ") << (p.vector ? vec(p.width) + " " : std::string())
          << p.name << (i + 1 < s.ports.size() ? ",\n" : "\n");
    }
    v << ");\n";

    // clocks
    const Port* clk = find(Role::Clk);
    const Port* wclk = find(Role::WrClk);
    const Port* rclk = find(Role::RdClk);
    const std::string W = wclk ? wclk->name : clk ? clk->name : "1'b0";
    const std::string R = !s.independent ? W : rclk ? rclk->name : clk ? clk->name : "1'b0";

    // resets: per side, asynchronous (synchronized into the side) or synchronous
    auto resetFor = [&](bool write, std::string& out) {
        struct Src {
            Role role;
            bool async, high;
        };
        const Src srcs[] = {
            {write ? Role::WrRstAsyncHigh : Role::RdRstAsyncHigh, true, true},
            {write ? Role::WrRstAsyncLow : Role::RdRstAsyncLow, true, false},
            {Role::RstAsyncHigh, true, true},
            {Role::RstAsyncLow, true, false},
            {write ? Role::WrRstSync : Role::RdRstSync, false, true},
            {Role::RstSync, false, true},
        };
        const std::string side = write ? "w" : "r";
        const std::string clkName = write ? W : R;
        for (const auto& src : srcs)
            if (const Port* p = find(src.role)) {
                if (!src.async) {
                    out = p->name;
                    return;
                }
                // assertion asynchronous, release synchronized (two stages)
                const std::string edge = src.high ? "posedge " + p->name : "negedge " + p->name;
                const std::string cond = src.high ? p->name : "!" + p->name;
                v << "  reg oc_" << side << "rst_s1 = 1'b1, oc_" << side << "rst_s2 = 1'b1;\n"
                  << "  always @(posedge " << clkName << " or " << edge << ")\n"
                  << "    if (" << cond << ") begin oc_" << side << "rst_s1 <= 1'b1; oc_" << side
                  << "rst_s2 <= 1'b1; end\n"
                  << "    else begin oc_" << side << "rst_s1 <= 1'b0; oc_" << side << "rst_s2 <= oc_" << side
                  << "rst_s1; end\n";
                out = "oc_" + side + "rst_s2";
                return;
            }
        out = "1'b0";
    };
    std::string wrst, rrst;
    resetFor(true, wrst);
    if (s.independent)
        resetFor(false, rrst);
    else
        rrst = wrst;

    // write side
    std::string we, wd;
    if (const Port* p = find(Role::WrEn))
        we = p->name;
    else if (const Port* q = find(Role::SValid))
        we = q->name;
    else
        we = "1'b0";
    if (const Port* p = find(Role::Din)) {
        wd = p->width < MW ? "{" + std::to_string(MW - p->width) + "'b0, " + p->name + "}" : p->name;
    }
    else {
        std::vector<const Port*> fields;
        for (const auto& fp : s.ports)
            if (fp.role == Role::SField)
                fields.push_back(&fp);
        std::ranges::sort(fields, {}, &Port::offset);
        unsigned used = 0;
        std::string cat;
        for (const auto* f : fields) {
            cat = f->name + (cat.empty() ? "" : ", ") + cat;
            used = std::max(used, f->offset + f->width);
        }
        if (cat.empty())
            wd = std::to_string(MW) + "'b0";
        else
            wd = used < MW ? "{" + std::to_string(MW - used) + "'b0, " + cat + "}" : "{" + cat + "}";
    }
    v << "  localparam integer AW = " << AW << ";\n"
      << "  reg [" << MW - 1 << ":0] oc_mem [0:" << (1ULL << AW) - 1 << "];\n"
      << "  reg [AW:0] oc_wbin = 0, oc_wgray = 0, oc_rbin = 0, oc_rgray = 0;\n";
    // pointer synchronizers (Gray code), or direct in a common-clock FIFO
    auto syncChain = [&](const std::string& prefix, const std::string& from, const std::string& clkName) {
        if (!s.independent)
            return from;
        v << "  reg [AW:0]";
        for (unsigned k = 1; k <= s.stages; ++k)
            v << (k > 1 ? "," : "") << " " << prefix << k << " = 0";
        v << ";\n  always @(posedge " << clkName << ") begin";
        for (unsigned k = 1; k <= s.stages; ++k)
            v << " " << prefix << k << " <= " << (k == 1 ? from : prefix + std::to_string(k - 1)) << ";";
        v << " end\n";
        return prefix + std::to_string(s.stages);
    };
    const std::string rq = syncChain("oc_rq", "oc_rgray", W); // read pointer in the write domain
    const std::string wq = syncChain("oc_wq", "oc_wgray", R); // write pointer in the read domain
    // Gray to binary of the synchronized pointers (bit i = XOR of bits AW..i)
    v << "  wire [AW:0] oc_rqb, oc_wqb;\n";
    for (unsigned i = 0; i <= AW; ++i)
        v << "  assign oc_rqb[" << i << "] = ^" << rq << "[AW:" << i << "]; assign oc_wqb[" << i << "] = ^" << wq
          << "[AW:" << i << "];\n";
    v << "  wire oc_full = oc_wgray == {~" << rq << "[AW:AW-1], " << rq << "[AW-2:0]};\n"
      << "  wire oc_empty = oc_rgray == " << wq << ";\n"
      << "  wire oc_we = " << we << " & ~oc_full & ~" << wrst << ";\n"
      << "  wire [AW:0] oc_wbin_n = oc_wbin + {{AW{1'b0}}, oc_we};\n"
      << "  always @(posedge " << W << ")\n"
      << "    if (" << wrst << ") begin oc_wbin <= 0; oc_wgray <= 0; end\n"
      << "    else begin oc_wbin <= oc_wbin_n; oc_wgray <= (oc_wbin_n >> 1) ^ oc_wbin_n; end\n"
      << "  always @(posedge " << W << ") if (oc_we) oc_mem[oc_wbin[AW-1:0]] <= " << wd << ";\n"
      << "  wire [AW:0] oc_wcount = oc_wbin - oc_rqb;\n";

    // read side
    std::string ready;
    if (const Port* p = find(Role::RdEn))
        ready = p->name;
    else if (const Port* q = find(Role::MReady))
        ready = q->name;
    else
        ready = find(Role::MValid) ? "1'b1" : "1'b0";
    v << "  reg [" << MW - 1 << ":0] oc_rdata = 0;\n"
      << "  reg oc_ov = 1'b0;\n";
    if (s.fwft) {
        // first-word fall-through / AXI-Stream: an output register loaded when empty or taken
        v << "  wire oc_re = ~oc_empty & (~oc_ov | " << ready << ") & ~" << rrst << ";\n"
          << "  always @(posedge " << R << ")\n"
          << "    if (" << rrst << ") oc_ov <= 1'b0;\n"
          << "    else if (oc_re) oc_ov <= 1'b1;\n"
          << "    else if (" << ready << ") oc_ov <= 1'b0;\n";
    }
    else {
        v << "  wire oc_re = " << ready << " & ~oc_empty & ~" << rrst << ";\n"
          << "  always @(posedge " << R << ") if (" << rrst << ") oc_ov <= 1'b0; else oc_ov <= oc_re;\n";
    }
    v << "  wire [AW:0] oc_rbin_n = oc_rbin + {{AW{1'b0}}, oc_re};\n"
      << "  always @(posedge " << R << ")\n"
      << "    if (" << rrst << ") begin oc_rbin <= 0; oc_rgray <= 0; end\n"
      << "    else begin oc_rbin <= oc_rbin_n; oc_rgray <= (oc_rbin_n >> 1) ^ oc_rbin_n; end\n"
      << "  always @(posedge " << R << ") if (oc_re) oc_rdata <= oc_mem[oc_rbin[AW-1:0]];\n"
      << "  wire [AW:0] oc_rcount = oc_wqb - oc_rbin;\n";
    const std::string emptyOut = s.fwft ? "~oc_ov" : "oc_empty";

    // registered status outputs
    v << "  reg oc_wack = 1'b0, oc_ovf = 1'b0, oc_unf = 1'b0;\n"
      << "  always @(posedge " << W << ") begin oc_wack <= oc_we; oc_ovf <= " << we << " & oc_full; end\n"
      << "  always @(posedge " << R << ") oc_unf <= " << ready << " & " << emptyOut << ";\n";

    const unsigned depth = 1U << AW;
    std::vector<std::string> zero;
    for (const auto& p : s.ports) {
        if (!p.output)
            continue;
        std::string rhs;
        auto count = [&](const std::string& c) {
            const unsigned cw = AW + 1;
            if (p.width >= cw)
                return p.width > cw ? "{" + std::to_string(p.width - cw) + "'b0, " + c + "}" : c;
            if (p.countMsbs)
                return c + "[" + std::to_string(AW) + ":" + std::to_string(cw - p.width) + "]";
            return c + "[" + std::to_string(p.width - 1) + ":0]";
        };
        switch (p.role) {
            case Role::Dout: rhs = "oc_rdata[" + std::to_string(p.width - 1) + ":0]"; break;
            case Role::MField:
                rhs = "oc_rdata[" + std::to_string(p.offset + p.width - 1) + ":" + std::to_string(p.offset) + "]";
                break;
            case Role::Full: rhs = "oc_full | " + wrst; break;
            case Role::SReady: rhs = "~oc_full & ~" + wrst; break;
            case Role::Empty: rhs = emptyOut; break;
            case Role::Valid:
            case Role::MValid: rhs = "oc_ov"; break;
            case Role::AlmostFull: rhs = "oc_wcount >= " + std::to_string(depth - 1); break;
            case Role::AlmostEmpty: rhs = "oc_rcount <= 1"; break;
            case Role::ProgFull: rhs = "oc_wcount >= " + std::to_string(std::max<long long>(s.progFull, 1)); break;
            case Role::ProgEmpty: rhs = "oc_rcount <= " + std::to_string(std::max<long long>(s.progEmpty, 0)); break;
            case Role::WrAck: rhs = "oc_wack"; break;
            case Role::Overflow: rhs = "oc_ovf"; break;
            case Role::Underflow: rhs = "oc_unf"; break;
            case Role::WrCount: rhs = count("oc_wcount"); break;
            case Role::RdCount: rhs = count("oc_rcount"); break;
            case Role::WrRstBusy: rhs = wrst; break;
            case Role::RdRstBusy: rhs = rrst; break;
            default:
                rhs = p.vector ? std::to_string(p.width) + "'b0" : "1'b0";
                zero.push_back(p.name);
                break;
        }
        v << "  assign " << p.name << " = " << rhs << ";\n";
    }
    v << "endmodule\n";
    if (!zero.empty()) {
        std::string list;
        for (const auto& z : zero)
            list += (list.empty() ? "" : ", ") + z;
        notes.push_back("outputs without a model, driven with 0: " + list);
    }
    return v.str();
}

// ---- port lists -------------------------------------------------------------

Port scalar(std::string name, bool out, Role role) {
    Port p;
    p.name = std::move(name);
    p.output = out;
    p.role = role;
    return p;
}

Port vectorPort(std::string name, bool out, unsigned width, Role role) {
    Port p = scalar(std::move(name), out, role);
    p.width = std::max(1U, width);
    p.vector = true;
    return p;
}

/// Roles of the ports of a FIFO Generator (native or AXI-Stream) or an
/// AXI4-Stream Data FIFO by name.
Role roleOf(const std::string& n, bool output, bool independent, bool axisDataFifo) {
    if (n == "wr_clk" || n == "s_aclk" || n == "s_axis_aclk")
        return independent ? Role::WrClk : Role::Clk;
    if (n == "rd_clk" || n == "m_aclk" || n == "m_axis_aclk")
        return independent ? Role::RdClk : Role::Clk;
    if (n == "clk")
        return Role::Clk;
    if (n == "rst")
        return Role::RstAsyncHigh;
    if (n == "s_aresetn")
        return Role::RstAsyncLow;
    if (n == "s_axis_aresetn")
        return axisDataFifo ? Role::WrRstAsyncLow : Role::RstAsyncLow;
    if (n == "m_axis_aresetn")
        return Role::RdRstAsyncLow;
    if (n == "srst")
        return Role::RstSync;
    if (n == "wr_rst")
        return Role::WrRstSync;
    if (n == "rd_rst")
        return Role::RdRstSync;
    if (n == "din")
        return Role::Din;
    if (n == "wr_en")
        return Role::WrEn;
    if (n == "rd_en")
        return Role::RdEn;
    if (n == "dout")
        return Role::Dout;
    if (n == "full")
        return Role::Full;
    if (n == "empty")
        return Role::Empty;
    if (n == "almost_full")
        return Role::AlmostFull;
    if (n == "almost_empty")
        return Role::AlmostEmpty;
    if (n == "prog_full" || n == "axis_prog_full")
        return Role::ProgFull;
    if (n == "prog_empty" || n == "axis_prog_empty")
        return Role::ProgEmpty;
    if (n == "valid")
        return Role::Valid;
    if (n == "wr_ack")
        return Role::WrAck;
    if (n == "overflow" || n == "axis_overflow")
        return Role::Overflow;
    if (n == "underflow" || n == "axis_underflow")
        return Role::Underflow;
    if (n == "wr_data_count" || n == "data_count" || n == "axis_wr_data_count" || n == "axis_data_count")
        return Role::WrCount;
    if (n == "rd_data_count" || n == "axis_rd_data_count")
        return Role::RdCount;
    if (n == "wr_rst_busy")
        return Role::WrRstBusy;
    if (n == "rd_rst_busy")
        return Role::RdRstBusy;
    if (n == "s_axis_tvalid")
        return Role::SValid;
    if (n == "s_axis_tready")
        return Role::SReady;
    if (n == "m_axis_tvalid")
        return Role::MValid;
    if (n == "m_axis_tready")
        return Role::MReady;
    for (const char* f : kAxisFields) {
        if (n == std::string("s_axis_") + f)
            return Role::SField;
        if (n == std::string("m_axis_") + f)
            return Role::MField;
    }
    return output ? Role::Zero : Role::Ignore;
}

/// Offsets of the AXI-Stream fields in the stored word, from the slave ports.
unsigned layoutAxis(std::vector<Port>& ports) {
    std::map<std::string, std::pair<unsigned, unsigned>> fields; // field -> (offset, width)
    unsigned off = 0;
    for (const char* f : kAxisFields)
        for (const auto& p : ports)
            if (p.name == std::string("s_axis_") + f) {
                fields[f] = {off, p.width};
                off += p.width;
            }
    for (auto& p : ports)
        for (const char* f : kAxisFields)
            if ((p.role == Role::SField && p.name == std::string("s_axis_") + f) ||
                (p.role == Role::MField && p.name == std::string("m_axis_") + f)) {
                if (auto it = fields.find(f); it != fields.end())
                    p.offset = it->second.first;
                else
                    p.role = p.output ? Role::Zero : Role::Ignore; // a master field without a slave field
            }
    return std::max(off, 1U);
}

Result<XciModel> fifoGenerator(const XciInfo& info) {
    const auto& m = info.model;
    const long long itf = num(m, "C_INTERFACE_TYPE", 0);
    if (itf == 2)
        return unsupported("IP '" + info.instance + "': FIFO Generator with an AXI4 memory-mapped interface is not modelled",
                           {"use the IP's generated HDL, or set the project option unknownModules to blackbox"});
    const bool axis = itf == 1;
    FifoSpec s;
    s.module = info.instance;
    s.independent = num(m, "C_COMMON_CLOCK", 0) == 0;
    s.stages = static_cast<unsigned>(std::clamp<long long>(num(m, "C_SYNCHRONIZER_STAGE", 2), 2, 8));
    const long long depth = axis ? num(m, "C_WR_DEPTH_AXIS", 1024)
                                 : std::max(num(m, "C_WR_DEPTH", 16), num(m, "C_RD_DEPTH", 16));
    s.depthBits = std::max(2U, clog2(std::max<long long>(depth, 4)));
    s.fwft = axis || (num(m, "C_PRELOAD_REGS", 0) == 1 && num(m, "C_PRELOAD_LATENCY", 1) == 0);
    s.progFull = num(m, axis ? "C_PROG_FULL_THRESH_ASSERT_VAL_AXIS" : "C_PROG_FULL_THRESH_ASSERT_VAL", depth - 2);
    s.progEmpty = num(m, axis ? "C_PROG_EMPTY_THRESH_ASSERT_VAL_AXIS" : "C_PROG_EMPTY_THRESH_ASSERT_VAL", 2);
    std::vector<Port>& ports = s.ports;
    const unsigned cw = s.depthBits + 1;
    if (!info.ports.empty()) {
        for (const auto& b : info.ports) {
            Port p;
            p.name = b.name;
            p.output = b.output;
            if (b.range) {
                p.vector = true;
                p.width = static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1);
            }
            p.role = roleOf(b.name, b.output, s.independent, false);
            ports.push_back(std::move(p));
        }
    }
    else if (axis) {
        auto has = [&](const char* k) { return num(m, k, 0) != 0; };
        const auto add = [&](const std::string& n, bool out, unsigned w, bool isVec) {
            Port p = isVec ? vectorPort(n, out, w, Role::Ignore) : scalar(n, out, Role::Ignore);
            p.role = roleOf(n, out, s.independent, false);
            ports.push_back(std::move(p));
        };
        if (s.independent)
            add("m_aclk", false, 1, false);
        add("s_aclk", false, 1, false);
        add("s_aresetn", false, 1, false);
        if (has("C_HAS_SLAVE_CE"))
            add("s_aclk_en", false, 1, false);
        if (has("C_HAS_MASTER_CE"))
            add("m_aclk_en", false, 1, false);
        struct F {
            const char* name;
            const char* hasKey;
            const char* widthKey;
        };
        const F fields[] = {{"tdata", "C_HAS_AXIS_TDATA", "C_AXIS_TDATA_WIDTH"},
                            {"tstrb", "C_HAS_AXIS_TSTRB", "C_AXIS_TSTRB_WIDTH"},
                            {"tkeep", "C_HAS_AXIS_TKEEP", "C_AXIS_TKEEP_WIDTH"},
                            {"tlast", "C_HAS_AXIS_TLAST", nullptr},
                            {"tid", "C_HAS_AXIS_TID", "C_AXIS_TID_WIDTH"},
                            {"tdest", "C_HAS_AXIS_TDEST", "C_AXIS_TDEST_WIDTH"},
                            {"tuser", "C_HAS_AXIS_TUSER", "C_AXIS_TUSER_WIDTH"}};
        for (const char* side : {"s", "m"}) {
            const bool out = std::string(side) == "m";
            add(std::string(side) + "_axis_tvalid", out, 1, false);
            if (num(m, "C_HAS_AXIS_TREADY", 1) != 0)
                add(std::string(side) + "_axis_tready", !out, 1, false);
            for (const auto& f : fields)
                if (has(f.hasKey))
                    add(std::string(side) + "_axis_" + f.name, out,
                        f.widthKey ? static_cast<unsigned>(num(m, f.widthKey, 1)) : 1, f.widthKey != nullptr);
        }
        if (has("C_HAS_DATA_COUNTS_AXIS")) {
            if (s.independent) {
                add("axis_wr_data_count", true, cw, true);
                add("axis_rd_data_count", true, cw, true);
            }
            else {
                add("axis_data_count", true, cw, true);
            }
        }
        if (has("C_HAS_PROG_FLAGS_AXIS")) {
            add("axis_prog_full", true, 1, false);
            add("axis_prog_empty", true, 1, false);
        }
    }
    else {
        auto has = [&](const char* k) { return num(m, k, 0) != 0; };
        const auto add = [&](const std::string& n, bool out, unsigned w, bool isVec) {
            Port p = isVec ? vectorPort(n, out, w, Role::Ignore) : scalar(n, out, Role::Ignore);
            p.role = roleOf(n, out, s.independent, false);
            p.countMsbs = true;
            ports.push_back(std::move(p));
        };
        if (has("C_HAS_WR_RST") || has("C_HAS_RD_RST")) {
            if (has("C_HAS_WR_RST"))
                add("wr_rst", false, 1, false);
            if (has("C_HAS_RD_RST"))
                add("rd_rst", false, 1, false);
        }
        else if (has("C_HAS_RST")) {
            add("rst", false, 1, false);
        }
        if (s.independent) {
            add("wr_clk", false, 1, false);
            add("rd_clk", false, 1, false);
        }
        else {
            add("clk", false, 1, false);
            if (has("C_HAS_SRST"))
                add("srst", false, 1, false);
        }
        add("din", false, static_cast<unsigned>(num(m, "C_DIN_WIDTH", 18)), true);
        add("wr_en", false, 1, false);
        add("rd_en", false, 1, false);
        const long long pft = num(m, "C_PROG_FULL_TYPE", 0);
        const long long pet = num(m, "C_PROG_EMPTY_TYPE", 0);
        const unsigned tw = clog2(std::max<long long>(depth, 2));
        if (pft == 3)
            add("prog_full_thresh", false, tw, true);
        if (pft == 4) {
            add("prog_full_thresh_assert", false, tw, true);
            add("prog_full_thresh_negate", false, tw, true);
        }
        if (pet == 3)
            add("prog_empty_thresh", false, tw, true);
        if (pet == 4) {
            add("prog_empty_thresh_assert", false, tw, true);
            add("prog_empty_thresh_negate", false, tw, true);
        }
        if (num(m, "C_USE_ECC", 0) != 0 && num(m, "C_ERROR_INJECTION_TYPE", 0) != 0) {
            add("injectdbiterr", false, 1, false);
            add("injectsbiterr", false, 1, false);
        }
        add("dout", true, static_cast<unsigned>(num(m, "C_DOUT_WIDTH", 18)), true);
        add("full", true, 1, false);
        if (has("C_HAS_ALMOST_FULL"))
            add("almost_full", true, 1, false);
        if (has("C_HAS_WR_ACK"))
            add("wr_ack", true, 1, false);
        if (has("C_HAS_OVERFLOW"))
            add("overflow", true, 1, false);
        add("empty", true, 1, false);
        if (has("C_HAS_ALMOST_EMPTY"))
            add("almost_empty", true, 1, false);
        if (has("C_HAS_VALID"))
            add("valid", true, 1, false);
        if (has("C_HAS_UNDERFLOW"))
            add("underflow", true, 1, false);
        if (s.independent) {
            if (has("C_HAS_RD_DATA_COUNT"))
                add("rd_data_count", true, static_cast<unsigned>(num(m, "C_RD_DATA_COUNT_WIDTH", cw)), true);
            if (has("C_HAS_WR_DATA_COUNT"))
                add("wr_data_count", true, static_cast<unsigned>(num(m, "C_WR_DATA_COUNT_WIDTH", cw)), true);
        }
        else if (has("C_HAS_DATA_COUNT")) {
            add("data_count", true, static_cast<unsigned>(num(m, "C_DATA_COUNT_WIDTH", cw)), true);
        }
        if (pft != 0)
            add("prog_full", true, 1, false);
        if (pet != 0)
            add("prog_empty", true, 1, false);
        if (num(m, "C_USE_ECC", 0) != 0) {
            add("sbiterr", true, 1, false);
            add("dbiterr", true, 1, false);
        }
        if (has("C_EN_SAFETY_CKT")) {
            add("wr_rst_busy", true, 1, false);
            add("rd_rst_busy", true, 1, false);
        }
    }
    for (auto& p : ports)
        if (p.role == Role::WrCount || p.role == Role::RdCount)
            p.countMsbs = !axis && !p.name.starts_with("axis_");
    unsigned mw = 1;
    if (axis) {
        mw = layoutAxis(ports);
    }
    else {
        for (const auto& p : ports)
            if (p.role == Role::Din || p.role == Role::Dout)
                mw = std::max(mw, p.width);
    }
    s.memWidth = mw;

    XciModel out;
    out.module = s.module;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = generate(s, out.notes);
    out.summary = std::string(s.independent ? "independent clocks (" + std::to_string(s.stages) + "-stage synchronizers)"
                                            : "common clock") +
                  ", " + std::to_string(mw) + " x " + std::to_string(1ULL << s.depthBits) + ", " +
                  (axis ? "AXI-Stream" : s.fwft ? "native, first-word fall-through" : "native");
    return out;
}

Result<XciModel> axisDataFifo(const XciInfo& info) {
    const auto& m = info.model;
    const auto& u = info.user;
    FifoSpec s;
    s.module = info.instance;
    s.independent = num(m, "C_IS_ACLK_ASYNC", num(u, "IS_ACLK_ASYNC", 0)) != 0;
    s.stages = static_cast<unsigned>(
        std::clamp<long long>(num(m, "C_SYNCHRONIZER_STAGE", num(u, "SYNCHRONIZATION_STAGES", 2)), 2, 8));
    const long long depth = num(m, "C_FIFO_DEPTH", num(u, "FIFO_DEPTH", 1024));
    s.depthBits = std::max(2U, clog2(std::max<long long>(depth, 4)));
    s.fwft = true;
    s.progFull = num(m, "C_PROG_FULL_THRESH", num(u, "PROG_FULL_THRESH", depth - 2));
    s.progEmpty = num(m, "C_PROG_EMPTY_THRESH", num(u, "PROG_EMPTY_THRESH", 2));
    auto& ports = s.ports;
    if (!info.ports.empty()) {
        for (const auto& b : info.ports) {
            Port p;
            p.name = b.name;
            p.output = b.output;
            if (b.range) {
                p.vector = true;
                p.width = static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1);
            }
            p.role = roleOf(b.name, b.output, s.independent, true);
            ports.push_back(std::move(p));
        }
    }
    else {
        const auto add = [&](const std::string& n, bool out, unsigned w, bool isVec) {
            Port p = isVec ? vectorPort(n, out, w, Role::Ignore) : scalar(n, out, Role::Ignore);
            p.role = roleOf(n, out, s.independent, true);
            ports.push_back(std::move(p));
        };
        const unsigned tdata = static_cast<unsigned>(num(m, "C_AXIS_TDATA_WIDTH", 8 * num(u, "TDATA_NUM_BYTES", 1)));
        long long set = num(m, "C_AXIS_SIGNAL_SET", -1);
        if (set < 0) {
            set = 0x3 | (num(u, "HAS_TSTRB", 0) ? 0x4 : 0) | (num(u, "HAS_TKEEP", 0) ? 0x8 : 0) |
                  (num(u, "HAS_TLAST", 0) ? 0x10 : 0) | (num(u, "TID_WIDTH", 0) ? 0x20 : 0) |
                  (num(u, "TDEST_WIDTH", 0) ? 0x40 : 0) | (num(u, "TUSER_WIDTH", 0) ? 0x80 : 0);
            if (num(u, "HAS_TREADY", 1) == 0)
                set &= ~0x1LL;
        }
        add("s_axis_aresetn", false, 1, false);
        add("s_axis_aclk", false, 1, false);
        if (s.independent)
            add("m_axis_aclk", false, 1, false);
        const long long aclken = num(m, "C_ACLKEN_CONV_MODE", num(u, "ACLKEN_CONV_MODE", 0));
        if (aclken == 1 || aclken == 3)
            add("s_axis_aclken", false, 1, false);
        if (aclken == 2 || aclken == 3)
            add("m_axis_aclken", false, 1, false);
        struct F {
            const char* name;
            long long bit;
            unsigned width;
            bool isVec;
        };
        const F fields[] = {{"tdata", 0x2, tdata, true},
                            {"tstrb", 0x4, std::max(1U, tdata / 8), true},
                            {"tkeep", 0x8, std::max(1U, tdata / 8), true},
                            {"tlast", 0x10, 1, false},
                            {"tid", 0x20, static_cast<unsigned>(num(m, "C_AXIS_TID_WIDTH", 1)), true},
                            {"tdest", 0x40, static_cast<unsigned>(num(m, "C_AXIS_TDEST_WIDTH", 1)), true},
                            {"tuser", 0x80, static_cast<unsigned>(num(m, "C_AXIS_TUSER_WIDTH", 1)), true}};
        for (const char* side : {"s", "m"}) {
            const bool out = std::string(side) == "m";
            add(std::string(side) + "_axis_tvalid", out, 1, false);
            if (set & 0x1)
                add(std::string(side) + "_axis_tready", !out, 1, false);
            for (const auto& f : fields)
                if (set & f.bit)
                    add(std::string(side) + "_axis_" + f.name, out, f.width, f.isVec);
        }
        if (num(u, "HAS_WR_DATA_COUNT", 0))
            add("axis_wr_data_count", true, 32, true);
        if (num(u, "HAS_RD_DATA_COUNT", 0))
            add("axis_rd_data_count", true, 32, true);
        if (num(u, "HAS_PROG_FULL", 0))
            add("prog_full", true, 1, false);
        if (num(u, "HAS_PROG_EMPTY", 0))
            add("prog_empty", true, 1, false);
        if (num(u, "HAS_AFULL", 0))
            add("almost_full", true, 1, false);
        if (num(u, "HAS_AEMPTY", 0))
            add("almost_empty", true, 1, false);
        if (num(u, "ENABLE_ECC", 0)) {
            if (num(u, "HAS_ECC_ERR_INJECT", 0)) {
                add("injectsbiterr", false, 1, false);
                add("injectdbiterr", false, 1, false);
            }
            add("sbiterr", true, 1, false);
            add("dbiterr", true, 1, false);
        }
    }
    s.memWidth = layoutAxis(ports);
    XciModel out;
    out.module = s.module;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = generate(s, out.notes);
    out.summary = std::string(s.independent ? "independent clocks (" + std::to_string(s.stages) + "-stage synchronizers)"
                                            : "common clock") +
                  ", " + std::to_string(s.memWidth) + " x " + std::to_string(1ULL << s.depthBits) + ", AXI-Stream" +
                  (num(m, "C_FIFO_MODE", num(u, "FIFO_MODE", 1)) == 2 ? " (packet mode modelled as a plain FIFO)" : "");
    return out;
}


// ---------------------------------------------------------------------------
// Debug cores: ILA (inputs only) and VIO (outputs set over JTAG, synchronized
// to clk inside the core)
// ---------------------------------------------------------------------------

Result<XciModel> debugCore(const XciInfo& info, bool vio) {
    const auto& m = info.model;
    const auto& u = info.user;
    auto param = [&](const std::string& k, long long def) { return num(m, k, num(u, k, def)); };
    struct P {
        std::string name;
        bool output;
        unsigned width;
        bool isVec;
        long long init = 0;
    };
    std::vector<P> ports;
    if (!info.ports.empty()) {
        for (const auto& b : info.ports)
            ports.push_back({b.name, b.output,
                             b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U,
                             b.range.has_value()});
    }
    else if (!vio) {
        const auto monitor = lower(u.contains("C_MONITOR_TYPE") ? u.at("C_MONITOR_TYPE") : std::string("native"));
        if (monitor != "native")
            return unsupported("IP '" + info.instance + "': ILA in AXI monitor mode is not modelled from an XML .xci",
                               {"use the IP's generated HDL, or set the project option unknownModules to blackbox"});
        ports.push_back({"clk", false, 1, false});
        if (param("C_TRIGIN_EN", 0)) {
            ports.push_back({"trig_in", false, 1, false});
            ports.push_back({"trig_in_ack", true, 1, false});
        }
        if (param("C_TRIGOUT_EN", 0)) {
            ports.push_back({"trig_out", true, 1, false});
            ports.push_back({"trig_out_ack", false, 1, false});
        }
        const long long n = param("C_NUM_OF_PROBES", 1);
        for (long long i = 0; i < n; ++i)
            ports.push_back({"probe" + std::to_string(i), false,
                             static_cast<unsigned>(param("C_PROBE" + std::to_string(i) + "_WIDTH", 1)), true});
    }
    else {
        ports.push_back({"clk", false, 1, false});
        const long long nin = param("C_NUM_PROBE_IN", 0);
        const long long nout = param("C_NUM_PROBE_OUT", 0);
        for (long long i = 0; i < nin; ++i)
            ports.push_back({"probe_in" + std::to_string(i), false,
                             static_cast<unsigned>(param("C_PROBE_IN" + std::to_string(i) + "_WIDTH", 1)), true});
        for (long long i = 0; i < nout; ++i)
            ports.push_back({"probe_out" + std::to_string(i), true,
                             static_cast<unsigned>(param("C_PROBE_OUT" + std::to_string(i) + "_WIDTH", 1)), true,
                             param("C_PROBE_OUT" + std::to_string(i) + "_INIT_VAL", 0)});
    }
    for (auto& p : ports)
        p.width = std::max(1U, p.width);
    const bool hasClk = std::ranges::any_of(ports, [](const P& p) { return p.name == "clk" && !p.output; });
    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << info.instance << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].output ? "output wire " : "input  wire ")
          << (ports[i].isVec ? vec(ports[i].width) + " " : std::string()) << ports[i].name
          << (i + 1 < ports.size() ? ",\n" : "\n");
    v << ");\n";
    for (const auto& p : ports) {
        if (!p.output)
            continue;
        const std::string w = std::to_string(p.width);
        const unsigned long long init =
            p.width >= 64 ? static_cast<unsigned long long>(p.init)
                          : static_cast<unsigned long long>(p.init) & ((1ULL << p.width) - 1);
        if (hasClk) {
            // a value in the clk domain (VIO: set over JTAG and synchronized inside the core)
            v << "  reg [" << p.width - 1 << ":0] oc_" << p.name << " = " << w << "'d" << init << ";\n"
              << "  always @(posedge clk) oc_" << p.name << " <= " << w << "'d" << init << ";\n"
              << "  assign " << p.name << " = oc_" << p.name << ";\n";
        }
        else {
            v << "  assign " << p.name << " = " << w << "'d" << init << ";\n";
        }
    }
    v << "endmodule\n";
    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    std::size_t probes = 0;
    for (const auto& p : ports)
        if (p.name.starts_with("probe"))
            ++probes;
    out.summary = vio ? std::to_string(probes) + " probe(s); outputs in the clk domain"
                      : std::to_string(probes) + " probe(s), inputs only";
    return out;
}


// ---------------------------------------------------------------------------
// Clocking Wizard: an MMCME2_ADV or PLLE2_ADV with the multiply/divide values
// Vivado computed (the clock analysis derives the output clocks from them)
// ---------------------------------------------------------------------------

std::string text(const std::map<std::string, std::string>& m, const std::string& key, std::string def) {
    auto it = m.find(key);
    return it == m.end() || it->second.empty() ? def : it->second;
}

/// A real parameter as a Verilog real literal ("11.875", "6" -> "6.0").
std::string realLit(const std::map<std::string, std::string>& m, const std::string& key, double def) {
    double v = def;
    if (auto it = m.find(key); it != m.end())
        try {
            v = std::stod(it->second);
        }
        catch (...) {
        }
    std::ostringstream s;
    s.precision(10);
    s << v;
    std::string r = s.str();
    if (r.find_first_of(".eE") == std::string::npos)
        r += ".0";
    return r;
}

Result<XciModel> clockingWizard(const XciInfo& info) {
    const auto& m = info.model;
    const auto& u = info.user;
    const bool pll = lower(text(m, "C_PRIMITIVE", text(u, "PRIMITIVE", "MMCM"))) == "pll";
    const std::string prim = pll ? "PLLE2_ADV" : "MMCME2_ADV";
    const std::string src = lower(text(m, "C_PRIM_SOURCE", text(u, "PRIM_SOURCE", "Single_ended_clock_capable_pin")));
    const bool diff = src.find("differential") != std::string::npos;
    const std::string in1 = text(m, "C_PRIMARY_PORT", text(u, "PRIMARY_PORT", "clk_in1"));
    const bool switchover = num(m, "C_USE_INCLK_SWITCHOVER", num(u, "USE_INCLK_SWITCHOVER", 0)) != 0;
    const std::string in2 = text(m, "C_SECONDARY_PORT", text(u, "SECONDARY_PORT", "clk_in2"));
    const bool useReset = num(m, "C_USE_RESET", num(u, "USE_RESET", 1)) != 0;
    const std::string resetPort = text(m, "C_RESET_PORT", text(u, "RESET_PORT", "reset"));
    const bool resetLow = lower(text(u, "RESET_TYPE", text(m, "C_RESET_TYPE", "ACTIVE_HIGH"))) == "active_low";
    const bool useLocked = num(m, "C_USE_LOCKED", num(u, "USE_LOCKED", 1)) != 0;
    const std::string lockedPort = text(m, "C_LOCKED_PORT", text(u, "LOCKED_PORT", "locked"));
    const bool powerDown = num(m, "C_USE_POWER_DOWN", num(u, "USE_POWER_DOWN", 0)) != 0;
    const std::string fbSource = lower(text(m, "C_FEEDBACK_SOURCE", text(u, "FEEDBACK_SOURCE", "FDBK_AUTO")));
    const bool extFeedback = fbSource != "fdbk_auto";
    const std::string fbIn = text(m, "C_CLKFB_IN_PORT", "clkfb_in");
    const std::string fbOut = text(m, "C_CLKFB_OUT_PORT", "clkfb_out");
    const unsigned maxOut = pll ? 6 : 7;

    struct Out {
        unsigned n;        ///< 1-based clk_outN
        std::string port;
        bool buffered;
    };
    std::vector<Out> outs;
    for (unsigned n = 1; n <= maxOut; ++n) {
        const std::string k = std::to_string(n);
        const bool used = n == 1 || num(m, "C_CLKOUT" + k + "_USED", num(u, "CLKOUT" + k + "_USED", 0)) != 0;
        if (!used)
            continue;
        const std::string drives = lower(text(m, "C_CLKOUT" + k + "_DRIVES", text(u, "CLKOUT" + k + "_DRIVES", "BUFG")));
        outs.push_back({n, text(m, "C_CLK_OUT" + k + "_PORT", text(u, "CLK_OUT" + k + "_PORT", "clk_out" + k)),
                        drives.starts_with("bufg")});
    }

    // ports: the JSON boundary as it is, otherwise from the configuration
    struct P {
        std::string name;
        bool output;
        unsigned width;
        bool isVec;
    };
    std::vector<P> ports;
    if (!info.ports.empty()) {
        for (const auto& b : info.ports)
            ports.push_back({b.name, b.output,
                             b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U,
                             b.range.has_value()});
    }
    else {
        for (const auto& o : outs)
            ports.push_back({o.port, true, 1, false});
        if (useReset)
            ports.push_back({resetPort, false, 1, false});
        if (powerDown)
            ports.push_back({"power_down", false, 1, false});
        if (switchover)
            ports.push_back({"clk_in_sel", false, 1, false});
        if (useLocked)
            ports.push_back({lockedPort, true, 1, false});
        if (extFeedback) {
            ports.push_back({fbIn, false, 1, false});
            ports.push_back({fbOut, true, 1, false});
        }
        if (diff) {
            ports.push_back({in1 + "_p", false, 1, false});
            ports.push_back({in1 + "_n", false, 1, false});
        }
        else {
            ports.push_back({in1, false, 1, false});
        }
        if (switchover) {
            if (diff) {
                ports.push_back({in2 + "_p", false, 1, false});
                ports.push_back({in2 + "_n", false, 1, false});
            }
            else {
                ports.push_back({in2, false, 1, false});
            }
        }
    }
    auto hasPort = [&](const std::string& n) {
        return std::ranges::any_of(ports, [&](const P& p) { return p.name == n; });
    };

    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << info.instance << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].output ? "output wire " : "input  wire ")
          << (ports[i].isVec ? vec(ports[i].width) + " " : std::string()) << ports[i].name
          << (i + 1 < ports.size() ? ",\n" : "\n");
    v << ");\n";
    // input clocks
    auto inputClock = [&](const std::string& base, const std::string& wire) {
        if (hasPort(base + "_p") && hasPort(base + "_n"))
            v << "  IBUFDS oc_ibuf_" << wire << " (.I(" << base << "_p), .IB(" << base << "_n), .O(" << wire << "));\n";
        else if (hasPort(base))
            v << "  assign " << wire << " = " << base << ";\n";
        else
            v << "  assign " << wire << " = 1'b0;\n";
    };
    v << "  wire oc_clkin1, oc_clkin2, oc_clkfb_o, oc_clkfb_i;\n";
    inputClock(in1, "oc_clkin1");
    if (switchover)
        inputClock(in2, "oc_clkin2");
    else
        v << "  assign oc_clkin2 = 1'b0;\n";
    // feedback: internal, or through the user's ports
    if (extFeedback && hasPort(fbIn) && hasPort(fbOut))
        v << "  assign " << fbOut << " = oc_clkfb_o;\n  assign oc_clkfb_i = " << fbIn << ";\n";
    else
        v << "  assign oc_clkfb_i = oc_clkfb_o;\n";
    const std::string rst = useReset && hasPort(resetPort) ? (resetLow ? "~" + resetPort : resetPort) : "1'b0";

    // the multiply / divide values Vivado chose for the requested frequencies
    const double inFreq = [&] {
        try {
            return std::stod(text(m, "C_PRIM_IN_FREQ", text(u, "PRIM_IN_FREQ", "100")));
        }
        catch (...) {
            return 100.0;
        }
    }();
    // the multiply/divide values must give the frequencies Vivado reports for the
    // outputs (C_CLKOUTn_OUT_FREQ): older .xci of a PLL keep stale C_PLL_* values
    // (all 1) next to the MMCM values Vivado computed for the same frequencies
    auto real = [&](const std::string& k, double def) {
        try {
            return std::stod(text(m, k, ""));
        }
        catch (...) {
            return def;
        }
    };
    auto consistent = [&](bool usePll) {
        const double mult = usePll ? real("C_PLL_CLKFBOUT_MULT", 1) : real("C_MMCM_CLKFBOUT_MULT_F", 1);
        const double dv = usePll ? real("C_PLL_DIVCLK_DIVIDE", 1) : real("C_MMCM_DIVCLK_DIVIDE", 1);
        for (const auto& o : outs) {
            const std::string k = std::to_string(o.n - 1);
            const double od = usePll ? real("C_PLL_CLKOUT" + k + "_DIVIDE", 1)
                                     : real(o.n == 1 ? "C_MMCM_CLKOUT0_DIVIDE_F" : "C_MMCM_CLKOUT" + k + "_DIVIDE", 1);
            const double want = real("C_CLKOUT" + std::to_string(o.n) + "_OUT_FREQ",
                                     real("C_CLKOUT" + std::to_string(o.n - 1) + "_ACTUAL_FREQ", 0));
            if (want <= 0 || dv <= 0 || od <= 0)
                continue;
            const double got = inFreq * mult / dv / od;
            if (std::abs(got - want) > 0.01 * want)
                return false;
        }
        return true;
    };
    bool usePllParams = pll;
    std::string staleNote;
    if (pll && !consistent(true) && consistent(false)) {
        usePllParams = false;
        staleNote = "the PLL's multiply/divide values in the .xci (C_PLL_*) do not give the configured output "
                    "frequencies; the clock ratios are taken from the MMCM values Vivado computed for the same "
                    "frequencies (C_MMCM_*)";
    }
    else if (!consistent(pll)) {
        staleNote = "the multiply/divide values in the .xci do not give the configured output frequencies "
                    "(C_CLKOUTn_OUT_FREQ); the clock periods follow the multiply/divide values";
    }
    std::ostringstream params;
    if (usePllParams) {
        params << ".CLKIN1_PERIOD(" << realLit(m, "C_PLL_CLKIN1_PERIOD", 1000.0 / inFreq) << "), .DIVCLK_DIVIDE("
               << num(m, "C_PLL_DIVCLK_DIVIDE", 1) << "), .CLKFBOUT_MULT(" << num(m, "C_PLL_CLKFBOUT_MULT", 1) << ")";
        for (const auto& o : outs)
            params << ", .CLKOUT" << o.n - 1 << "_DIVIDE("
                   << num(m, "C_PLL_CLKOUT" + std::to_string(o.n - 1) + "_DIVIDE", 1) << ")";
    }
    else {
        params << ".CLKIN1_PERIOD(" << realLit(m, "C_MMCM_CLKIN1_PERIOD", 1000.0 / inFreq) << "), .DIVCLK_DIVIDE("
               << num(m, "C_MMCM_DIVCLK_DIVIDE", 1) << "), .CLKFBOUT_MULT_F("
               << realLit(m, "C_MMCM_CLKFBOUT_MULT_F", 1.0) << ")";
        for (const auto& o : outs) {
            if (o.n == 1)
                params << ", .CLKOUT0_DIVIDE_F(" << realLit(m, "C_MMCM_CLKOUT0_DIVIDE_F", 1.0) << ")";
            else
                params << ", .CLKOUT" << o.n - 1 << "_DIVIDE("
                       << num(m, "C_MMCM_CLKOUT" + std::to_string(o.n - 1) + "_DIVIDE", 1) << ")";
        }
    }
    // the nets on the output pins carry Vivado's clock names (clk_out1_clk_wiz_0)
    auto outNet = [&](const Out& o) { return o.port + "_" + info.instance; };
    for (const auto& o : outs)
        v << "  wire " << outNet(o) << ";\n";
    const std::string primUsed = usePllParams || !pll ? prim : "MMCME2_ADV";
    v << "  wire oc_locked;\n"
      << "  " << primUsed << " #(" << params.str() << ")\n    oc_" << (pll ? "pll" : "mmcm") << " (.CLKIN1(oc_clkin1), .CLKIN2(oc_clkin2), .CLKINSEL("
      << (switchover && hasPort("clk_in_sel") ? "clk_in_sel" : "1'b1") << "), .CLKFBIN(oc_clkfb_i), .CLKFBOUT(oc_clkfb_o), .RST(" << rst
      << "), .PWRDWN(" << (powerDown && hasPort("power_down") ? "power_down" : "1'b0") << "), .LOCKED(oc_locked)";
    for (const auto& o : outs)
        v << ", .CLKOUT" << o.n - 1 << "(" << outNet(o) << ")";
    v << ");\n";
    std::vector<std::string> zero;
    for (const auto& p : ports) {
        if (!p.output)
            continue;
        if (p.name == fbOut && extFeedback)
            continue;
        if (p.name == lockedPort) {
            v << "  assign " << p.name << " = oc_locked;\n";
            continue;
        }
        auto o = std::ranges::find(outs, p.name, &Out::port);
        if (o != outs.end()) {
            if (o->buffered)
                v << "  BUFG oc_bufg" << o->n - 1 << " (.I(" << outNet(*o) << "), .O(" << p.name << "));\n";
            else
                v << "  assign " << p.name << " = " << outNet(*o) << ";\n";
            continue;
        }
        v << "  assign " << p.name << " = " << (p.isVec ? std::to_string(p.width) + "'b0" : "1'b0") << ";\n";
        zero.push_back(p.name);
    }
    v << "endmodule\n";

    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    std::string freqs;
    for (const auto& o : outs) {
        const std::string f = text(m, "C_CLKOUT" + std::to_string(o.n) + "_OUT_FREQ", "?");
        freqs += (freqs.empty() ? "" : ", ") + o.port + " " + f + " MHz";
    }
    out.summary = (pll ? std::string("PLL") : std::string("MMCM")) + ", input " + text(m, "C_PRIM_IN_FREQ", "?") +
                  " MHz" + (diff ? " (differential)" : "") + "; " + freqs;
    if (!zero.empty()) {
        std::string list;
        for (const auto& z : zero)
            list += (list.empty() ? "" : ", ") + z;
        out.notes.push_back("outputs without a model, driven with 0: " + list);
    }
    if (num(m, "C_USE_DYN_RECONFIG", 0) != 0)
        out.notes.push_back("dynamic reconfiguration is not modelled: the output clocks keep the configured frequencies");
    if (!staleNote.empty())
        out.notes.push_back(staleNote);
    return out;
}


// ---------------------------------------------------------------------------
// Block Memory Generator (native interface): memories written on the clock
// of their write port and read through registers on the clock of the read
// port; data written on one port and read on the other crosses domains.
// ---------------------------------------------------------------------------

struct SimplePort {
    std::string name;
    bool output;
    unsigned width;
    bool isVec;
    bool inout = false;
};

std::vector<SimplePort> boundaryPorts(const XciInfo& info) {
    std::vector<SimplePort> ports;
    for (const auto& b : info.ports)
        ports.push_back({b.name, b.output,
                         b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U,
                         b.range.has_value(), b.inout});
    return ports;
}

void emitHeader(std::ostringstream& v, const std::string& module, const std::vector<SimplePort>& ports) {
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << module << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].inout ? "inout  wire " : ports[i].output ? "output wire " : "input  wire ")
          << (ports[i].isVec ? vec(ports[i].width) + " " : std::string()) << ports[i].name
          << (i + 1 < ports.size() ? ",\n" : "\n");
    v << ");\n";
}

Result<XciModel> blockMemoryAxi(XciInfo info);

Result<XciModel> blockMemory(const XciInfo& info) {
    const auto& m = info.model;
    if (num(m, "C_INTERFACE_TYPE", 0) != 0)
        return blockMemoryAxi(info);
    const long long type = num(m, "C_MEM_TYPE", 0); // 0 SP RAM, 1 SDP RAM, 2 TDP RAM, 3 SP ROM, 4 DP ROM
    const bool rom = type == 3 || type == 4;
    const bool hasB = type == 1 || type == 2 || type == 4;
    const bool writeB = type == 2;
    const bool readA = type != 1;
    const unsigned wa = static_cast<unsigned>(num(m, "C_WRITE_WIDTH_A", 32));
    const unsigned ra = static_cast<unsigned>(num(m, "C_READ_WIDTH_A", wa));
    const unsigned wb = static_cast<unsigned>(num(m, "C_WRITE_WIDTH_B", wa));
    const unsigned rb = static_cast<unsigned>(num(m, "C_READ_WIDTH_B", wa));
    const unsigned mw = std::max({wa, ra, hasB ? wb : 1U, hasB ? rb : 1U, 1U});
    const long long latencyA = 1 + num(m, "C_HAS_MEM_OUTPUT_REGS_A", 0) + num(m, "C_HAS_MUX_OUTPUT_REGS_A", 0);
    const long long latencyB = 1 + num(m, "C_HAS_MEM_OUTPUT_REGS_B", 0) + num(m, "C_HAS_MUX_OUTPUT_REGS_B", 0);

    std::vector<SimplePort> ports;
    if (!info.ports.empty()) {
        ports = boundaryPorts(info);
    }
    else {
        auto add = [&](std::string n, bool out, unsigned w, bool isVec) { ports.push_back({std::move(n), out, w, isVec}); };
        add("clka", false, 1, false);
        if (num(m, "C_HAS_RSTA", 0))
            add("rsta", false, 1, false);
        if (num(m, "C_HAS_ENA", 0))
            add("ena", false, 1, false);
        if (num(m, "C_HAS_REGCEA", 0))
            add("regcea", false, 1, false);
        if (!rom)
            add("wea", false, static_cast<unsigned>(num(m, "C_WEA_WIDTH", 1)), true);
        add("addra", false, static_cast<unsigned>(num(m, "C_ADDRA_WIDTH", 10)), true);
        if (!rom)
            add("dina", false, wa, true);
        if (readA)
            add("douta", true, ra, true);
        if (hasB) {
            add("clkb", false, 1, false);
            if (num(m, "C_HAS_RSTB", 0))
                add("rstb", false, 1, false);
            if (num(m, "C_HAS_ENB", 0))
                add("enb", false, 1, false);
            if (num(m, "C_HAS_REGCEB", 0))
                add("regceb", false, 1, false);
            if (writeB)
                add("web", false, static_cast<unsigned>(num(m, "C_WEB_WIDTH", 1)), true);
            add("addrb", false, static_cast<unsigned>(num(m, "C_ADDRB_WIDTH", 10)), true);
            if (writeB)
                add("dinb", false, wb, true);
            add("doutb", true, rb, true);
        }
        if (num(m, "C_HAS_INJECTERR", 0)) {
            add("injectsbiterr", false, 1, false);
            add("injectdbiterr", false, 1, false);
        }
        if (num(m, "C_USE_ECC", 0) || num(m, "C_USE_SOFTECC", 0)) {
            add("sbiterr", true, 1, false);
            add("dbiterr", true, 1, false);
            add("rdaddrecc", true, static_cast<unsigned>(num(m, "C_ADDRB_WIDTH", 10)), true);
        }
        if (num(m, "C_EN_SLEEP_PIN", 0))
            add("sleep", false, 1, false);
        if (num(m, "C_EN_SAFETY_CKT", 0)) {
            add("rsta_busy", true, 1, false);
            if (hasB)
                add("rstb_busy", true, 1, false);
        }
    }
    auto find = [&](const std::string& n) -> const SimplePort* {
        auto it = std::ranges::find(ports, n, &SimplePort::name);
        return it == ports.end() ? nullptr : &*it;
    };
    std::ostringstream v;
    emitHeader(v, info.instance, ports);
    constexpr unsigned kModelAddrBits = 8; // contents do not matter for CDC: 256 entries
    v << "  reg [" << mw - 1 << ":0] oc_mem_a [0:" << (1U << kModelAddrBits) - 1 << "];\n"
      << "  reg [" << mw - 1 << ":0] oc_mem_b [0:" << (1U << kModelAddrBits) - 1 << "];\n";
    auto addr = [&](const char* port) -> std::string {
        const SimplePort* p = find(port);
        if (!p)
            return std::to_string(kModelAddrBits) + "'d0";
        const unsigned w = std::min(p->width, kModelAddrBits);
        const std::string a = p->isVec ? std::string(port) + "[" + std::to_string(w - 1) + ":0]" : std::string(port);
        return w < kModelAddrBits ? "{" + std::to_string(kModelAddrBits - w) + "'d0, " + a + "}" : a;
    };
    auto data = [&](const char* port) -> std::string {
        const SimplePort* p = find(port);
        if (!p)
            return std::to_string(mw) + "'d0";
        return p->width < mw ? "{" + std::to_string(mw - p->width) + "'d0, " + std::string(port) + "}" : std::string(port);
    };
    auto en = [&](const char* port) { return find(port) ? std::string(port) : std::string("1'b1"); };
    auto we = [&](const char* port) { return find(port) ? "(|" + std::string(port) + ")" : std::string("1'b0"); };
    const std::string clka = find("clka") ? "clka" : "1'b0";
    const std::string clkb = find("clkb") ? "clkb" : clka;
    // writes: each port into its own memory (written on its own clock)
    if (!rom) {
        v << "  always @(posedge " << clka << ") if (" << en("ena") << " && " << we("wea") << ") oc_mem_a[" << addr("addra")
          << "] <= " << data("dina") << ";\n";
        if (find("web"))
            v << "  always @(posedge " << clkb << ") if (" << en("enb") << " && " << we("web") << ") oc_mem_b["
              << addr("addrb") << "] <= " << data("dinb") << ";\n";
    }
    // reads: a register on the read port's clock, then the output registers
    auto readPort = [&](const char* side, const std::string& clk, const char* enPort, const char* addrPort,
                        const char* rstPort, const char* regcePort, long long latency, const char* out) {
        const SimplePort* o = find(out);
        if (!o)
            return;
        const std::string pfx = std::string("oc_") + side;
        for (long long k = 1; k <= latency; ++k)
            v << "  reg [" << mw - 1 << ":0] " << pfx << k << " = 0;\n";
        const std::string word = rom ? std::to_string(mw) + "'d0"
                                     : "oc_mem_a[" + addr(addrPort) + "] ^ oc_mem_b[" + addr(addrPort) + "]";
        v << "  always @(posedge " << clk << ") if (" << en(enPort) << ") " << pfx << "1 <= " << word << ";\n";
        for (long long k = 2; k <= latency; ++k) {
            v << "  always @(posedge " << clk << ")";
            if (find(rstPort))
                v << " if (" << rstPort << ") " << pfx << k << " <= 0; else";
            v << " if (" << en(regcePort) << ") " << pfx << k << " <= " << pfx << k - 1 << ";\n";
        }
        v << "  assign " << out << " = " << pfx << latency << "[" << o->width - 1 << ":0];\n";
    };
    readPort("a", clka, "ena", "addra", "rsta", "regcea", latencyA, "douta");
    readPort("b", clkb, "enb", "addrb", "rstb", "regceb", latencyB, "doutb");
    std::vector<std::string> zero;
    for (const auto& p : ports) {
        if (!p.output || p.name == "douta" || p.name == "doutb")
            continue;
        v << "  assign " << p.name << " = " << (p.isVec ? std::to_string(p.width) + "'b0" : "1'b0") << ";\n";
        zero.push_back(p.name);
    }
    v << "endmodule\n";
    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    static const char* kTypes[] = {"single-port RAM", "simple dual-port RAM", "true dual-port RAM", "single-port ROM",
                                   "dual-port ROM"};
    out.summary = std::string(type >= 0 && type <= 4 ? kTypes[type] : "memory") + ", " +
                  std::to_string(num(m, "C_WRITE_DEPTH_A", 0)) + " x " + std::to_string(wa) +
                  (hasB ? (num(m, "C_COMMON_CLK", 0) ? ", common clock" : ", clka/clkb") : std::string()) +
                  ", read latency " + std::to_string(latencyA);
    if (!zero.empty()) {
        std::string list;
        for (const auto& z : zero)
            list += (list.empty() ? "" : ", ") + z;
        out.notes.push_back("outputs without a model, driven with 0: " + list);
    }
    return out;
}

// ---------------------------------------------------------------------------
// XADC Wizard (xadc_wiz), DRP interface: the dynamic reconfiguration port and
// the status outputs belong to dclk_in, as on the XADC primitive; the analog
// inputs are not read. With the AXI4-Lite interface: one clock, s_axi_aclk.
// ---------------------------------------------------------------------------

Result<XciModel> oneClockIp(const XciInfo& info);

Result<XciModel> xadcWizard(const XciInfo& info) {
    const auto& m = info.model;
    if (num(m, "C_HAS_AXI", 0) != 0 || num(m, "C_HAS_AXI4STREAM", 0) != 0) {
        if (num(m, "C_HAS_AXI4STREAM", 0) != 0)
            return unsupported("IP '" + info.instance + "' (XADC Wizard) with an AXI4-Stream output is not modelled",
                               {"add the IP's generated HDL to the sources instead, or set the project option "
                                "unknownModules to blackbox"});
        auto r = oneClockIp(info);
        if (r) {
            XciModel out = std::move(r).value();
            out.summary = "AXI4-Lite interface: one clock 's_axi_aclk'; every output is registered";
            return out;
        }
        return r;
    }
    std::vector<SimplePort> ports;
    if (!info.ports.empty()) {
        ports = boundaryPorts(info);
    }
    else {
        auto add = [&](std::string n, bool out, unsigned w, bool isVec) { ports.push_back({std::move(n), out, w, isVec}); };
        auto has = [&](const char* key) { return num(m, key, 0) != 0; };
        if (has("C_HAS_DRP")) {
            add("di_in", false, 16, true);
            add("daddr_in", false, 7, true);
            add("den_in", false, 1, false);
            add("dwe_in", false, 1, false);
            add("drdy_out", true, 1, false);
            add("do_out", true, 16, true);
        }
        if (has("C_HAS_DCLK"))
            add("dclk_in", false, 1, false);
        if (has("C_HAS_RESET"))
            add("reset_in", false, 1, false);
        if (has("C_HAS_CONVST"))
            add("convst_in", false, 1, false);
        if (has("C_HAS_CONVSTCLK"))
            add("convstclk_in", false, 1, false);
        if (has("C_HAS_VP"))
            add("vp_in", false, 1, false);
        if (has("C_HAS_VN"))
            add("vn_in", false, 1, false);
        for (int n = 0; n < 16; ++n)
            if (num(m, "C_VAUX" + std::to_string(n), 0) != 0) {
                add("vauxp" + std::to_string(n), false, 1, false);
                add("vauxn" + std::to_string(n), false, 1, false);
            }
        if (has("C_HAS_USER_TEMP_ALARM"))
            add("user_temp_alarm_out", true, 1, false);
        if (has("C_HAS_VCCINT_ALARM"))
            add("vccint_alarm_out", true, 1, false);
        if (has("C_HAS_VCCAUX_ALARM"))
            add("vccaux_alarm_out", true, 1, false);
        if (has("C_HAS_VBRAM_ALARM"))
            add("vbram_alarm_out", true, 1, false);
        if (has("C_HAS_VCCPINT_ALARM"))
            add("vccpint_alarm_out", true, 1, false);
        if (has("C_HAS_VCCPAUX_ALARM"))
            add("vccpaux_alarm_out", true, 1, false);
        if (has("C_HAS_VCCDDRO_ALARM"))
            add("vccddro_alarm_out", true, 1, false);
        if (has("C_HAS_OT_ALARM"))
            add("ot_out", true, 1, false);
        if (has("C_HAS_CHANNEL"))
            add("channel_out", true, 5, true);
        if (has("C_HAS_EXTERNAL_MUX") || has("C_EXTERNAL_MUXADDR_ENABLE"))
            add("muxaddr_out", true, 5, true);
        if (has("C_HAS_EOC"))
            add("eoc_out", true, 1, false);
        add("alarm_out", true, 1, false); // the OR of the alarms: always there (low without alarms)
        if (has("C_HAS_EOS"))
            add("eos_out", true, 1, false);
        if (has("C_HAS_BUSY"))
            add("busy_out", true, 1, false);
        if (has("C_HAS_TEMP_BUS"))
            add("temp_out", true, 12, true);
        if (has("C_HAS_JTAGBUSY"))
            add("jtagbusy_out", true, 1, false);
        if (has("C_HAS_JTAGLOCKED"))
            add("jtaglocked_out", true, 1, false);
        if (has("C_HAS_JTAGMODIFIED"))
            add("jtagmodified_out", true, 1, false);
    }
    const bool hasDclk = std::ranges::any_of(ports, [](const SimplePort& p) { return p.name == "dclk_in" && !p.output; });
    if (!hasDclk)
        return unsupported("IP '" + info.instance + "' (XADC Wizard) without dclk_in is not modelled",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    auto analog = [](const std::string& n) {
        return n == "vp_in" || n == "vn_in" || n.starts_with("vauxp") || n.starts_with("vauxn");
    };
    std::ostringstream v;
    emitHeader(v, info.instance, ports);
    std::string inputs;
    for (const auto& p : ports)
        if (!p.output && p.name != "dclk_in" && !analog(p.name))
            inputs += (inputs.empty() ? "" : ", ") + p.name;
    v << "  reg oc_mix = 1'b0;\n"
      << "  always @(posedge dclk_in) oc_mix <= " << (inputs.empty() ? std::string("1'b0") : "^{" + inputs + "}")
      << ";\n";
    std::vector<std::string> noDomain;
    for (const auto& p : ports) {
        if (!p.output)
            continue;
        if (p.name.starts_with("jtag")) { // set over JTAG: no clock domain
            v << "  assign " << p.name << " = " << (p.isVec ? std::to_string(p.width) + "'b0" : "1'b0") << ";\n";
            noDomain.push_back(p.name);
            continue;
        }
        const std::string r = "oc_" + p.name;
        v << "  reg " << (p.isVec ? vec(p.width) + " " : std::string()) << r << " = 0;\n"
          << "  always @(posedge dclk_in) " << r << " <= {" << p.width << "{oc_mix}};\n"
          << "  assign " << p.name << " = " << r << ";\n";
    }
    v << "endmodule\n";
    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    out.summary = "DRP interface: the dynamic reconfiguration port and the status outputs in 'dclk_in' (" +
                  text(m, "C_DCLK_FREQUENCY", "?") + " MHz configured)";
    if (!noDomain.empty()) {
        std::string list;
        for (const auto& n : noDomain)
            list += (list.empty() ? "" : ", ") + n;
        out.notes.push_back("JTAG status outputs are driven with 0: " + list);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Block design glue: Slice, Concat, Constant (combinational wiring)
// ---------------------------------------------------------------------------

Result<XciModel> wiringIp(const XciInfo& info) {
    const auto& m = info.model;
    std::vector<SimplePort> ports;
    std::ostringstream body;
    std::string summary;
    auto portWidth = [&](const std::string& name, unsigned def) {
        for (const auto& b : info.ports)
            if (b.name == name)
                return b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U;
        return def;
    };
    if (info.name == "xlslice") {
        const long long from = num(m, "DIN_FROM", 0);
        const long long to = num(m, "DIN_TO", 0);
        const unsigned din = static_cast<unsigned>(std::max(1LL, num(m, "DIN_WIDTH", std::max(from, to) + 1)));
        if (from < 0 || to < 0 || std::max(from, to) >= din)
            return unsupported("IP '" + info.instance + "': slice range [" + std::to_string(from) + ":" +
                               std::to_string(to) + "] outside the input width " + std::to_string(din));
        const unsigned dout = static_cast<unsigned>(std::llabs(from - to) + 1);
        ports = {{"Din", false, portWidth("Din", din), true}, {"Dout", true, portWidth("Dout", dout), true}};
        if (from >= to) {
            body << "  assign Dout = Din[" << from << ":" << to << "];\n";
        }
        else { // a reversed slice: Dout[0] = Din[from], ...
            for (unsigned i = 0; i < dout; ++i)
                body << "  assign Dout[" << i << "] = Din[" << from + i << "];\n";
        }
        summary = "Din[" + std::to_string(from) + ":" + std::to_string(to) + "]";
    }
    else if (info.name == "xlconcat") {
        const long long n = num(m, "NUM_PORTS", 2);
        if (n < 1 || n > 128)
            return unsupported("IP '" + info.instance + "': NUM_PORTS " + std::to_string(n) + " is not modelled");
        std::string cat;
        unsigned total = 0;
        for (long long i = 0; i < n; ++i) {
            const std::string pn = "In" + std::to_string(i);
            const unsigned w =
                portWidth(pn, static_cast<unsigned>(std::max(1LL, num(m, "IN" + std::to_string(i) + "_WIDTH", 1))));
            ports.push_back({pn, false, w, true});
            cat = pn + (cat.empty() ? "" : ", ") + cat;
            total += w;
        }
        ports.push_back({"dout", true, portWidth("dout", total), true});
        body << "  assign dout = {" << cat << "};\n";
        summary = std::to_string(n) + " inputs, " + std::to_string(total) + " bits";
    }
    else { // xlconstant
        const unsigned w = portWidth("dout", static_cast<unsigned>(std::max(1LL, num(m, "CONST_WIDTH", 1))));
        const std::string val = text(m, "CONST_VAL", "0");
        const long long k = num(m, "CONST_VAL", -1);
        if (k < 0 || w > 62)
            return unsupported("IP '" + info.instance + "': constant value '" + val + "' (" + std::to_string(w) +
                               " bits) is not modelled");
        ports = {{"dout", true, w, true}};
        body << "  assign dout = " << w << "'d" << k << ";\n";
        summary = std::to_string(w) + "'d" + std::to_string(k);
    }
    std::ostringstream v;
    emitHeader(v, info.instance, ports);
    v << body.str() << "endmodule\n";
    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    out.summary = summary;
    return out;
}

// ---------------------------------------------------------------------------
// AXI Quad SPI: the AXI side on s_axi_aclk, the SPI side on ext_spi_clk;
// with Async_Clk the two sides exchange data through asynchronous FIFOs
// (as in the IP), otherwise directly (the clocks must then be the same).
// ---------------------------------------------------------------------------

Result<XciModel> quadSpi(const XciInfo& info) {
    const auto& m = info.model;
    if (info.ports.empty())
        return unsupported("IP '" + info.instance + "' (AXI Quad SPI): its ports are only modelled from a JSON .xci "
                           "or the component file next to an XML .xci (" + info.instance + ".xml)",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    std::vector<SimplePort> ports = boundaryPorts(info);
    auto has = [&](const std::string& n) { return std::ranges::any_of(ports, [&](const SimplePort& p) { return p.name == n; }); };
    if (!has("s_axi_aclk") || !has("ext_spi_clk"))
        return unsupported("IP '" + info.instance + "' (AXI Quad SPI): no s_axi_aclk / ext_spi_clk port");
    const bool async = num(m, "Async_Clk", 0) != 0;
    const bool axi4 = has("s_axi4_aclk"); // the AXI4 memory interface (C_TYPE_OF_AXI4_INTERFACE = 1)
    // the SPI side: pins (io*, sck, ss, spisel)
    auto spiSide = [](const std::string& n) {
        return n.starts_with("io") || n.starts_with("sck_") || n.starts_with("ss_") || n == "spisel";
    };
    auto axi4Side = [](const std::string& n) { return n.starts_with("s_axi4_"); };
    // the STARTUP primitive (C_USE_STARTUP): its configuration clocks, end of startup and
    // program request come from the primitive; its inputs go to it
    static const std::set<std::string> kStartupOut = {"cfgclk", "cfgmclk", "eos", "preq"};
    static const std::set<std::string> kStartupIn = {"gsr",      "gts",      "keyclearb", "usrcclkts",
                                                     "usrdoneo", "usrdonets", "pack",     "clk"};
    std::string axiIn, axi4In, spiIn;
    for (const auto& p : ports) {
        if (p.output || p.name == "s_axi_aclk" || p.name == "ext_spi_clk" || p.name == "s_axi4_aclk" ||
            kStartupIn.contains(p.name))
            continue;
        std::string& list = spiSide(p.name) ? spiIn : axi4Side(p.name) && axi4 ? axi4In : axiIn;
        list += (list.empty() ? "" : ", ") + p.name;
    }
    std::ostringstream v;
    std::vector<std::string> notes;
    // asynchronous FIFOs between the sides (transmit and receive; also for the AXI4 interface)
    std::string fifos;
    if (async) {
        long long depth = num(m, "C_FIFO_DEPTH", 16);
        unsigned bits = std::max(2U, clog2(std::max(depth, 4LL)));
        std::vector<std::string> names = {"tx", "rx"};
        if (axi4) {
            names.push_back("tx4");
            names.push_back("rx4");
        }
        for (const auto& dirName : names) {
            FifoSpec f;
            f.module = info.instance + "_oc_" + dirName + "_fifo";
            f.independent = true;
            f.depthBits = bits;
            f.memWidth = 1;
            f.stages = 2;
            f.ports = {scalar("wr_clk", false, Role::WrClk), scalar("rd_clk", false, Role::RdClk),
                       scalar("rst", false, Role::RstAsyncHigh), vectorPort("din", false, 1, Role::Din),
                       scalar("wr_en", false, Role::WrEn), scalar("rd_en", false, Role::RdEn),
                       vectorPort("dout", true, 1, Role::Dout), scalar("full", true, Role::Full),
                       scalar("empty", true, Role::Empty)};
            fifos += generate(f, notes) + "\n";
        }
    }
    emitHeader(v, info.instance, ports);
    const std::string rst = has("s_axi_aresetn") ? "~s_axi_aresetn" : "1'b0";
    const std::string rst4 = has("s_axi4_aresetn") ? "~s_axi4_aresetn" : rst;
    v << "  reg oc_axi_mix = 1'b0, oc_spi_mix = 1'b0;\n"
      << "  always @(posedge s_axi_aclk) oc_axi_mix <= " << (axiIn.empty() ? std::string("1'b0") : "^{" + axiIn + "}")
      << ";\n"
      << "  always @(posedge ext_spi_clk) oc_spi_mix <= " << (spiIn.empty() ? std::string("1'b0") : "^{" + spiIn + "}")
      << ";\n"
      << "  wire oc_to_spi, oc_to_axi, oc_to_spi4, oc_to_axi4;\n";
    if (axi4)
        v << "  reg oc_axi4_mix = 1'b0;\n  always @(posedge s_axi4_aclk) oc_axi4_mix <= "
          << (axi4In.empty() ? std::string("1'b0") : "^{" + axi4In + "}") << ";\n";
    if (async) {
        v << "  wire oc_tx_full, oc_tx_empty, oc_rx_full, oc_rx_empty;\n"
          << "  " << info.instance << "_oc_tx_fifo u_oc_tx_fifo (.wr_clk(s_axi_aclk), .rd_clk(ext_spi_clk), .rst(" << rst
          << "), .din(oc_axi_mix), .wr_en(1'b1), .rd_en(1'b1), .dout(oc_to_spi), .full(oc_tx_full), "
             ".empty(oc_tx_empty));\n"
          << "  " << info.instance << "_oc_rx_fifo u_oc_rx_fifo (.wr_clk(ext_spi_clk), .rd_clk(s_axi_aclk), .rst(" << rst
          << "), .din(oc_spi_mix), .wr_en(1'b1), .rd_en(1'b1), .dout(oc_to_axi), .full(oc_rx_full), "
             ".empty(oc_rx_empty));\n";
        if (axi4)
            v << "  wire oc_tx4_full, oc_tx4_empty, oc_rx4_full, oc_rx4_empty;\n"
              << "  " << info.instance << "_oc_tx4_fifo u_oc_tx4_fifo (.wr_clk(s_axi4_aclk), .rd_clk(ext_spi_clk), .rst("
              << rst4 << "), .din(oc_axi4_mix), .wr_en(1'b1), .rd_en(1'b1), .dout(oc_to_spi4), .full(oc_tx4_full), "
                 ".empty(oc_tx4_empty));\n"
              << "  " << info.instance << "_oc_rx4_fifo u_oc_rx4_fifo (.wr_clk(ext_spi_clk), .rd_clk(s_axi4_aclk), .rst("
              << rst4 << "), .din(oc_spi_mix), .wr_en(1'b1), .rd_en(1'b1), .dout(oc_to_axi4), .full(oc_rx4_full), "
                 ".empty(oc_rx4_empty));\n";
        else
            v << "  assign oc_to_spi4 = 1'b0;\n  assign oc_to_axi4 = 1'b0;\n";
    }
    else {
        v << "  assign oc_to_spi = oc_axi_mix;\n  assign oc_to_axi = oc_spi_mix;\n"
          << "  assign oc_to_spi4 = " << (axi4 ? "oc_axi4_mix" : "1'b0") << ";\n  assign oc_to_axi4 = oc_spi_mix;\n";
    }
    // the STARTUP primitive: the SPI clock drives the configuration clock pin (USRCCLKO)
    const bool startup = num(m, "C_USE_STARTUP", 0) != 0 || has("cfgclk") || has("eos");
    if (startup) {
        const std::string fam = lower(text(m, "C_FAMILY", ""));
        const bool us = fam.find("ultrascale") != std::string::npos || fam.find("kintexu") != std::string::npos ||
                        fam.find("virtexu") != std::string::npos || fam.find("zynquplus") != std::string::npos;
        auto pin = [&](const char* name) { return has(name) ? std::string(name) : std::string(); };
        v << "  reg oc_sck = 1'b0;\n  always @(posedge ext_spi_clk) oc_sck <= oc_to_spi ^ oc_to_spi4 ^ oc_spi_mix;\n"
          << "  " << (us ? "STARTUPE3 STARTUP3_ULTRASCALE_inst" : "STARTUPE2 STARTUP2_7SERIES_inst") << " (.CFGCLK("
          << pin("cfgclk") << "), .CFGMCLK(" << pin("cfgmclk") << "), .EOS(" << pin("eos") << "), .PREQ(" << pin("preq")
          << "), .GSR(1'b0), .GTS(1'b0), .KEYCLEARB(1'b1), .PACK(1'b0), .USRCCLKO(oc_sck), .USRCCLKTS(1'b0), "
             ".USRDONEO(1'b1), .USRDONETS(1'b1));\n";
    }
    for (const auto& p : ports) {
        if (!p.output)
            continue;
        if (kStartupOut.contains(p.name)) {
            if (!startup)
                v << "  assign " << p.name << " = 1'b0;\n";
            continue;
        }
        const bool spi = spiSide(p.name);
        const bool side4 = axi4 && axi4Side(p.name);
        const std::string r = "oc_" + p.name;
        const std::string w = std::to_string(p.width);
        v << "  reg " << (p.isVec ? vec(p.width) + " " : std::string()) << r << " = 0;\n"
          << "  always @(posedge " << (spi ? "ext_spi_clk" : side4 ? "s_axi4_aclk" : "s_axi_aclk") << ") " << r
          << " <= {" << w << "{"
          << (spi ? "oc_to_spi ^ oc_to_spi4 ^ oc_spi_mix" : side4 ? "oc_to_axi4 ^ oc_axi4_mix" : "oc_to_axi ^ oc_axi_mix")
          << "}};\n"
          << "  assign " << p.name << " = " << r << ";\n";
    }
    v << "endmodule\n\n" << fifos;
    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    out.summary = std::string("AXI side on s_axi_aclk") + (axi4 ? ", AXI4 memory interface on s_axi4_aclk" : "") +
                  ", SPI side on ext_spi_clk, " +
                  (async ? "exchanged through asynchronous FIFOs (Async_Clk)"
                         : "exchanged directly (Async_Clk = 0: the clocks must be the same)");
    if (startup)
        out.notes.push_back("STARTUP primitive (C_USE_STARTUP): the SPI clock drives its USRCCLKO pin; cfgclk/cfgmclk "
                            "are clocks of the configuration oscillator, eos/preq asynchronous outputs");
    return out;
}

// ---------------------------------------------------------------------------
// One-clock datapath IP (CORDIC, floating point, divider, multiplier, adder,
// DDS, FIR, complex multiplier, shift RAM, counter): every output is a
// register of the IP's clock that depends on every input (conservative).
// ---------------------------------------------------------------------------

Result<XciModel> oneClockIp(const XciInfo& info) {
    const auto& m = info.model;
    std::vector<SimplePort> ports;
    if (!info.ports.empty()) {
        ports = boundaryPorts(info);
    }
    else if (info.name == "cordic") {
        auto add = [&](std::string n, bool out, unsigned w, bool isVec) { ports.push_back({std::move(n), out, w, isVec}); };
        add("aclk", false, 1, false);
        if (num(m, "C_HAS_ACLKEN", 0))
            add("aclken", false, 1, false);
        if (num(m, "C_HAS_ARESETN", 0))
            add("aresetn", false, 1, false);
        for (const char* ch : {"phase", "cartesian"}) {
            const std::string up = std::string(ch) == "phase" ? "PHASE" : "CARTESIAN";
            if (!num(m, "C_HAS_S_AXIS_" + up, 0))
                continue;
            add(std::string("s_axis_") + ch + "_tvalid", false, 1, false);
            if (num(m, "C_THROTTLE_SCHEME", 3) != 3 && num(m, "C_THROTTLE_SCHEME", 3) != 4)
                add(std::string("s_axis_") + ch + "_tready", true, 1, false);
            if (num(m, "C_HAS_S_AXIS_" + up + "_TUSER", 0))
                add(std::string("s_axis_") + ch + "_tuser", false,
                    static_cast<unsigned>(num(m, "C_S_AXIS_" + up + "_TUSER_WIDTH", 1)), true);
            if (num(m, "C_HAS_S_AXIS_" + up + "_TLAST", 0))
                add(std::string("s_axis_") + ch + "_tlast", false, 1, false);
            add(std::string("s_axis_") + ch + "_tdata", false,
                static_cast<unsigned>(num(m, "C_S_AXIS_" + up + "_TDATA_WIDTH", 32)), true);
        }
        add("m_axis_dout_tvalid", true, 1, false);
        if (num(m, "C_THROTTLE_SCHEME", 3) == 1 || num(m, "C_THROTTLE_SCHEME", 3) == 2)
            add("m_axis_dout_tready", false, 1, false);
        if (num(m, "C_HAS_S_AXIS_PHASE_TUSER", 0) || num(m, "C_HAS_S_AXIS_CARTESIAN_TUSER", 0))
            add("m_axis_dout_tuser", true, static_cast<unsigned>(num(m, "C_M_AXIS_DOUT_TUSER_WIDTH", 1)), true);
        if (num(m, "C_HAS_S_AXIS_PHASE_TLAST", 0) || num(m, "C_HAS_S_AXIS_CARTESIAN_TLAST", 0))
            add("m_axis_dout_tlast", true, 1, false);
        add("m_axis_dout_tdata", true, static_cast<unsigned>(num(m, "C_M_AXIS_DOUT_TDATA_WIDTH", 16)), true);
    }
    else if (info.name == "floating_point") {
        // PG060: the A channel always, B, C and the operation channel when present
        auto add = [&](std::string n, bool out, unsigned w, bool isVec) { ports.push_back({std::move(n), out, w, isVec}); };
        add("aclk", false, 1, false);
        if (num(m, "C_HAS_ACLKEN", 0))
            add("aclken", false, 1, false);
        if (num(m, "C_HAS_ARESETN", 0))
            add("aresetn", false, 1, false);
        const long long throttle = num(m, "C_THROTTLE_SCHEME", 3);
        for (const char* ch : {"a", "b", "c", "operation"}) {
            std::string up = ch;
            std::ranges::transform(up, up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (up != "A" && !num(m, "C_HAS_" + up, 0))
                continue;
            const std::string pre = std::string("s_axis_") + ch;
            add(pre + "_tvalid", false, 1, false);
            if (throttle != 3 && throttle != 4)
                add(pre + "_tready", true, 1, false);
            add(pre + "_tdata", false, static_cast<unsigned>(num(m, "C_" + up + "_TDATA_WIDTH", 32)), true);
            if (num(m, "C_HAS_" + up + "_TUSER", 0))
                add(pre + "_tuser", false, static_cast<unsigned>(num(m, "C_" + up + "_TUSER_WIDTH", 1)), true);
            if (num(m, "C_HAS_" + up + "_TLAST", 0))
                add(pre + "_tlast", false, 1, false);
        }
        add("m_axis_result_tvalid", true, 1, false);
        if (throttle == 1 || throttle == 2)
            add("m_axis_result_tready", false, 1, false);
        add("m_axis_result_tdata", true, static_cast<unsigned>(num(m, "C_RESULT_TDATA_WIDTH", 32)), true);
        if (num(m, "C_HAS_RESULT_TUSER", 0))
            add("m_axis_result_tuser", true, static_cast<unsigned>(num(m, "C_RESULT_TUSER_WIDTH", 1)), true);
        if (num(m, "C_HAS_RESULT_TLAST", 0))
            add("m_axis_result_tlast", true, 1, false);
    }
    else {
        return unsupported("IP '" + info.instance + "' (" + info.vendor + ":" + info.library + ":" + info.name + ":" +
                               info.version + "): its ports are only modelled from a JSON .xci (Vivado 2020.2+) "
                               "or from the component file next to an XML .xci (" + info.instance + ".xml)",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    }
    // the clock: aclk / clk / CLK
    const SimplePort* clk = nullptr;
    for (const char* c : {"aclk", "clk", "CLK", "ACLK", "clka", "s_axi_aclk"})
        if (auto it = std::ranges::find(ports, std::string(c), &SimplePort::name); it != ports.end() && !it->output) {
            clk = &*it;
            break;
        }
    if (!clk)
        return unsupported("IP '" + info.instance + "': no clock port (aclk/clk) found",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    std::ostringstream v;
    emitHeader(v, info.instance, ports);
    std::string inputs;
    for (const auto& p : ports)
        if (!p.output && p.name != clk->name)
            inputs += (inputs.empty() ? "" : ", ") + p.name;
    v << "  wire oc_mix = " << (inputs.empty() ? std::string("1'b0") : "^{" + inputs + "}") << ";\n";
    for (const auto& p : ports) {
        if (!p.output)
            continue;
        v << "  reg [" << p.width - 1 << ":0] oc_" << p.name << " = 0;\n"
          << "  always @(posedge " << clk->name << ") oc_" << p.name << " <= {" << p.width << "{oc_mix}};\n"
          << "  assign " << p.name << " = oc_" << p.name << ";\n";
    }
    v << "endmodule\n";
    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    out.summary = "one-clock datapath on '" + clk->name + "': every output is registered and depends on every input";
    return out;
}

// ---------------------------------------------------------------------------
// Memory Interface Generator (mig_7series, DDR2/DDR3, one controller): the
// user interface clock ui_clk is derived from the system clock; every user
// interface port (native app_* or AXI s_axi_*) belongs to ui_clk; sys_rst is
// synchronized into ui_clk (rstdiv0_sync_r, as in the IP).
// ---------------------------------------------------------------------------

/// The memory configuration Vivado keeps next to the .xci (XML_INPUT_FILE,
/// e.g. mig_a.prj): only the first controller's clocking is used.
struct MigProject {
    double inputClkMHz = 0;  ///< <InputClkFreq>
    double timePeriodPs = 0; ///< <TimePeriod> (memory clock)
    unsigned phyRatio = 0;   ///< <PHYRatio> 4:1 -> 4
};

std::optional<MigProject> parseMigProject(const std::string& text) {
    auto tag = [&](const char* name) -> std::optional<std::string> {
        const std::regex re(std::string("<") + name + R"(>\s*([^<]*?)\s*</)" + name + ">");
        std::smatch m;
        if (std::regex_search(text, m, re))
            return m[1].str();
        return std::nullopt;
    };
    MigProject p;
    try {
        if (auto v = tag("InputClkFreq"))
            p.inputClkMHz = std::stod(*v);
        if (auto v = tag("TimePeriod"))
            p.timePeriodPs = std::stod(*v);
        if (auto v = tag("PHYRatio"))
            p.phyRatio = static_cast<unsigned>(std::stoul(*v));
    }
    catch (...) {
        return std::nullopt;
    }
    if (p.inputClkMHz <= 0 || p.timePeriodPs <= 0 || (p.phyRatio != 2 && p.phyRatio != 4))
        return std::nullopt;
    return p;
}

std::string fmtReal(double v) {
    std::ostringstream s;
    s.precision(10);
    s << v;
    std::string r = s.str();
    if (r.find_first_of(".eE") == std::string::npos)
        r += ".0";
    return r;
}

Result<XciModel> memoryInterface(const XciInfo& info, const std::optional<std::string>& prjText,
                                 const std::string& prjName) {
    const auto& m = info.model;
    const auto unsupportedMig = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + info.vendor + ":" + info.library + ":" + info.name + ":" +
                               info.version + "): " + why + " is not modelled",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox (its outputs then drive their signals)"});
    };
    if (num(m, "NoOfControllers", 1) != 1)
        return unsupportedMig("more than one controller");
    const std::string memType = lower(text(m, "MEM_TYPE", ""));
    if (memType != "ddr2" && memType != "ddr3")
        return unsupportedMig("memory type '" + text(m, "MEM_TYPE", "?") + "'");
    if (lower(text(m, "ECC", "OFF")) == "on")
        return unsupportedMig("ECC");
    const std::string up = memType == "ddr2" ? "DDR2_" : "DDR3_";
    auto w = [&](const char* key, long long def) {
        return static_cast<unsigned>(std::max(1LL, num(m, up + key, def)));
    };
    const unsigned nCk = w("nCK_PER_CLK", 4);
    const unsigned dq = w("DQ_WIDTH", 16);
    const unsigned appData = 2 * nCk * dq;
    const bool axi = num(m, "USE_AXI", 0) != 0;
    const bool extraClocks = lower(text(m, "UI_EXTRA_CLOCKS", "FALSE")) == "true";
    const bool resetLow = lower(text(m, "POLARITY", "ACTIVE_HIGH")) == "active_low";
    const double uiHz = [&] {
        try {
            return std::stod(text(m, "FREQ_HZ", "0"));
        }
        catch (...) {
            return 0.0;
        }
    }();

    // ports: the JSON boundary as it is, otherwise from the configuration
    struct P {
        std::string name;
        int dir; ///< 0 in, 1 out, 2 inout
        unsigned width;
        bool isVec;
    };
    std::vector<P> ports;
    auto add = [&](std::string n, int dir, unsigned width, bool isVec) { ports.push_back({std::move(n), dir, width, isVec}); };
    if (!info.ports.empty()) {
        for (const auto& b : info.ports) {
            const bool inout = b.inout || b.name.starts_with(memType + "_dq"); // dq, dqs_p, dqs_n
            add(b.name, inout ? 2 : b.output ? 1 : 0,
                b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U, b.range.has_value());
        }
    }
    else {
        const std::string p = memType + "_";
        add(p + "dq", 2, dq, true);
        add(p + "dqs_n", 2, w("DQS_WIDTH", 2), true);
        add(p + "dqs_p", 2, w("DQS_WIDTH", 2), true);
        add(p + "addr", 1, w("ROW_WIDTH", 13), true);
        add(p + "ba", 1, w("BANK_WIDTH", 3), true);
        add(p + "ras_n", 1, 1, false);
        add(p + "cas_n", 1, 1, false);
        add(p + "we_n", 1, 1, false);
        if (memType == "ddr3")
            add(p + "reset_n", 1, 1, false);
        add(p + "ck_p", 1, w("CK_WIDTH", 1), true);
        add(p + "ck_n", 1, w("CK_WIDTH", 1), true);
        add(p + "cke", 1, w("CKE_WIDTH", 1), true);
        if (num(m, up + "USE_CS_PORT", 1))
            add(p + "cs_n", 1, w("CS_WIDTH", 1) * w("nCS_PER_RANK", 1), true);
        if (num(m, up + "USE_DM_PORT", 1))
            add(p + "dm", 1, w("DM_WIDTH", 2), true);
        if (num(m, up + "USE_ODT_PORT", 1))
            add(p + "odt", 1, w("ODT_WIDTH", 1), true);
        const std::string sysType = lower(text(m, "SYSCLK_TYPE", "DIFFERENTIAL"));
        if (sysType == "differential") {
            add("sys_clk_p", 0, 1, false);
            add("sys_clk_n", 0, 1, false);
        }
        else {
            add("sys_clk_i", 0, 1, false);
        }
        const std::string refType = lower(text(m, "REFCLK_TYPE", "NONE"));
        if (refType == "differential") {
            add("clk_ref_p", 0, 1, false);
            add("clk_ref_n", 0, 1, false);
        }
        else if (refType == "single_ended" || refType == "nobuf" || refType == "no_buffer") {
            add("clk_ref_i", 0, 1, false);
        }
        if (axi) {
            const unsigned id = static_cast<unsigned>(std::max(1LL, num(m, "C_S_AXI_ID_WIDTH", 4)));
            const unsigned aw = static_cast<unsigned>(std::max(1LL, num(m, "C_S_AXI_ADDR_WIDTH", 32)));
            const unsigned dw = static_cast<unsigned>(std::max(8LL, num(m, "C_S_AXI_DATA_WIDTH", 32)));
            for (const char* ch : {"aw", "ar"}) {
                const std::string c = std::string("s_axi_") + ch;
                add(c + "id", 0, id, true);
                add(c + "addr", 0, aw, true);
                add(c + "len", 0, 8, true);
                add(c + "size", 0, 3, true);
                add(c + "burst", 0, 2, true);
                add(c + "lock", 0, 1, true);
                add(c + "cache", 0, 4, true);
                add(c + "prot", 0, 3, true);
                add(c + "qos", 0, 4, true);
                add(c + "valid", 0, 1, false);
                add(c + "ready", 1, 1, false);
                if (std::string(ch) == "aw") {
                    add("s_axi_wdata", 0, dw, true);
                    add("s_axi_wstrb", 0, dw / 8, true);
                    add("s_axi_wlast", 0, 1, false);
                    add("s_axi_wvalid", 0, 1, false);
                    add("s_axi_wready", 1, 1, false);
                    add("s_axi_bready", 0, 1, false);
                    add("s_axi_bid", 1, id, true);
                    add("s_axi_bresp", 1, 2, true);
                    add("s_axi_bvalid", 1, 1, false);
                }
            }
            add("s_axi_rready", 0, 1, false);
            add("s_axi_rid", 1, id, true);
            add("s_axi_rdata", 1, dw, true);
            add("s_axi_rresp", 1, 2, true);
            add("s_axi_rlast", 1, 1, false);
            add("s_axi_rvalid", 1, 1, false);
        }
        else {
            add("app_addr", 0, static_cast<unsigned>(std::max(1LL, num(m, up + "ADDR_WIDTH", 27))), true);
            add("app_cmd", 0, 3, true);
            add("app_en", 0, 1, false);
            add("app_wdf_data", 0, appData, true);
            add("app_wdf_end", 0, 1, false);
            add("app_wdf_mask", 0, appData / 8, true);
            add("app_wdf_wren", 0, 1, false);
            add("app_rd_data", 1, appData, true);
            add("app_rd_data_end", 1, 1, false);
            add("app_rd_data_valid", 1, 1, false);
            add("app_rdy", 1, 1, false);
            add("app_wdf_rdy", 1, 1, false);
        }
        add("app_sr_req", 0, 1, false);
        add("app_ref_req", 0, 1, false);
        add("app_zq_req", 0, 1, false);
        add("app_sr_active", 1, 1, false);
        add("app_ref_ack", 1, 1, false);
        add("app_zq_ack", 1, 1, false);
        add("ui_clk", 1, 1, false);
        add("ui_clk_sync_rst", 1, 1, false);
        if (extraClocks)
            for (unsigned n = 0; n < 5; ++n)
                if (lower(text(m, "MMCM_CLKOUT" + std::to_string(n) + "_EN", "FALSE")) == "true")
                    add("ui_addn_clk_" + std::to_string(n), 1, 1, false);
        if (axi) {
            add("mmcm_locked", 1, 1, false);
            add("aresetn", 0, 1, false);
        }
        // the temperature: always an output; an input from the user's XADC with TEMP_MON_CONTROL EXTERNAL
        if (lower(text(m, "TEMP_MON_CONTROL", "INTERNAL")) == "external")
            add("device_temp_i", 0, 12, true);
        add("device_temp", 1, 12, true);
        add("init_calib_complete", 1, 1, false);
        add("sys_rst", 0, 1, false);
    }
    auto has = [&](const std::string& n) { return std::ranges::any_of(ports, [&](const P& p) { return p.name == n; }); };
    if (!has("ui_clk"))
        return unsupportedMig("a configuration without ui_clk");

    // clock periods: from the memory configuration (exact ratio to the input
    // clock), otherwise the configured user interface frequency (absolute)
    std::optional<MigProject> prj;
    if (prjText)
        prj = parseMigProject(*prjText);
    double uiPeriodNs = uiHz > 0 ? 1e9 / uiHz : 0.0;
    double inPeriodNs = 0;
    if (prj) {
        uiPeriodNs = prj->timePeriodPs * prj->phyRatio / 1000.0;
        inPeriodNs = 1000.0 / prj->inputClkMHz;
    }
    if (uiPeriodNs <= 0)
        return unsupportedMig("a configuration without the user interface frequency (FREQ_HZ)");

    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << info.instance << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].dir == 2 ? "inout  wire " : ports[i].dir == 1 ? "output wire " : "input  wire ")
          << (ports[i].isVec ? vec(ports[i].width) + " " : std::string()) << ports[i].name
          << (i + 1 < ports.size() ? ",\n" : "\n");
    v << ");\n";
    // the system clock
    v << "  wire oc_sys_clk;\n";
    if (has("sys_clk_p") && has("sys_clk_n"))
        v << "  IBUFDS oc_ibuf_sys (.I(sys_clk_p), .IB(sys_clk_n), .O(oc_sys_clk));\n";
    else if (has("sys_clk_i"))
        v << "  assign oc_sys_clk = sys_clk_i;\n";
    else
        return unsupportedMig("a configuration without a system clock port");
    // derived clocks: a toggle register on the system clock carries the period
    auto clockAttr = [&](const std::string& pin, double periodNs, const std::string& alias) {
        std::string a = "(* OPENCDC_CLOCK_PIN = \"" + pin + "\"";
        if (!alias.empty())
            a += ", OPENCDC_CLOCK_ALIAS = \"" + alias + "\"";
        if (inPeriodNs > 0)
            a += ", OPENCDC_CLOCK_RATIO = " + fmtReal(periodNs / inPeriodNs) +
                 ", OPENCDC_CLOCK_INPUT_PERIOD = " + fmtReal(inPeriodNs);
        else
            a += ", OPENCDC_CLOCK_PERIOD = " + fmtReal(periodNs);
        return a + " *)";
    };
    auto derived = [&](const std::string& pin, double periodNs, const std::string& alias) {
        v << "  " << clockAttr(pin, periodNs, alias) << "\n  reg oc_" << pin << "_q = 1'b0;\n"
          << "  always @(posedge oc_sys_clk) oc_" << pin << "_q <= ~oc_" << pin << "_q;\n"
          << "  assign " << pin << " = oc_" << pin << "_q;\n";
    };
    derived("ui_clk", uiPeriodNs, "clk_pll_i");
    std::string extraSummary;
    for (unsigned n = 0; n < 5; ++n) {
        const std::string pin = "ui_addn_clk_" + std::to_string(n);
        if (!has(pin))
            continue;
        double mhz = 0;
        try {
            mhz = std::stod(text(m, "MMCM_CLKOUT" + std::to_string(n) + "_FREQ", "0"));
        }
        catch (...) {
        }
        if (mhz <= 0)
            return unsupportedMig("an additional clock without a frequency (MMCM_CLKOUT" + std::to_string(n) + "_FREQ)");
        derived(pin, 1000.0 / mhz, "");
        extraSummary += ", " + pin + " " + text(m, "MMCM_CLKOUT" + std::to_string(n) + "_FREQ", "?") + " MHz";
    }
    // the reset: asserted asynchronously, released synchronized to ui_clk
    // (the IP's rstdiv0_sync_r in u_<ip>_mig/u_<mem>_infrastructure)
    const std::string rstIn = has("sys_rst") ? (resetLow ? "~sys_rst" : "sys_rst") : "1'b0";
    v << "  wire oc_ui_rst;\n"
      << "  " << info.instance << "_mig u_" << info.instance << "_mig (.clk(ui_clk), .rst_async(" << rstIn
      << "), .rst_sync(oc_ui_rst));\n";
    // user interface: every input is sampled in ui_clk, every output is a
    // ui_clk register that depends on the inputs (the controller and memory)
    std::string inputs;
    for (const auto& p : ports)
        if (p.dir == 0 && p.name != "sys_clk_i" && p.name != "sys_clk_p" && p.name != "sys_clk_n" &&
            !p.name.starts_with("clk_ref") && p.name != "sys_rst" && p.name != "device_temp_i")
            inputs += (inputs.empty() ? "" : ", ") + p.name;
    v << "  reg oc_mix = 1'b0;\n"
      << "  always @(posedge ui_clk) oc_mix <= " << (inputs.empty() ? std::string("1'b0") : "^{" + inputs + "}")
      << ";\n";
    for (const auto& p : ports) {
        if (p.dir != 1 || p.name == "ui_clk" || p.name.starts_with("ui_addn_clk_"))
            continue;
        const std::string zeros = p.isVec ? std::to_string(p.width) + "'b0" : "1'b0";
        if (p.name == "ui_clk_sync_rst") {
            v << "  assign " << p.name << " = oc_ui_rst;\n";
            continue;
        }
        if (p.name == "mmcm_locked") {
            v << "  assign " << p.name << " = ~oc_ui_rst;\n";
            continue;
        }
        if (p.name.starts_with(memType + "_ck_")) { // memory clock pins
            v << "  assign " << p.name << " = " << zeros << ";\n";
            continue;
        }
        const std::string r = "oc_" + p.name;
        v << "  reg " << (p.isVec ? vec(p.width) + " " : std::string()) << r << " = " << zeros << ";\n"
          << "  always @(posedge ui_clk) if (oc_ui_rst) " << r << " <= " << zeros << "; else " << r << " <= {"
          << p.width << "{oc_mix}};\n"
          << "  assign " << p.name << " = " << r << ";\n";
    }
    v << "endmodule\n\n";
    // reset synchronizer, named as in the IP so that constraints on it match
    const std::string infra = info.instance + "_" + memType + "_infrastructure";
    v << "module " << info.instance << "_mig (input wire clk, input wire rst_async, output wire rst_sync);\n"
      << "  " << infra << " u_" << memType << "_infrastructure (.clk(clk), .rst_tmp(rst_async), .rstdiv0(rst_sync));\n"
      << "endmodule\n\n"
      << "module " << infra << " (input wire clk, input wire rst_tmp, output wire rstdiv0);\n"
      << "  reg [8:0] rstdiv0_sync_r = 9'h1ff;\n"
      << "  always @(posedge clk or posedge rst_tmp)\n"
      << "    if (rst_tmp) rstdiv0_sync_r <= 9'h1ff;\n"
      << "    else rstdiv0_sync_r <= {rstdiv0_sync_r[7:0], 1'b0};\n"
      << "  assign rstdiv0 = rstdiv0_sync_r[8];\n"
      << "endmodule\n";

    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    std::ostringstream sum;
    sum.precision(6);
    sum << (memType == "ddr2" ? "DDR2" : "DDR3") << " x" << dq << ", " << (axi ? "AXI4" : "native") << " interface, ui_clk "
        << 1000.0 / uiPeriodNs << " MHz";
    if (prj)
        sum << " (memory clock " << 1e6 / prj->timePeriodPs << " MHz / " << prj->phyRatio << ", from input "
            << prj->inputClkMHz << " MHz)";
    out.summary = sum.str() + extraSummary;
    if (!prj)
        out.notes.push_back(
            "the memory configuration '" + prjName +
            "' was not found next to the .xci (or not readable): ui_clk keeps the configured period and is "
            "related to the system clock, without a ratio to it");
    out.notes.push_back("device_temp_i is not read: the controller filters the temperature on its own");
    return out;
}

// ---------------------------------------------------------------------------
// IP from an IP repository (component.xml): the IP's own HDL with a wrapper
// that has the instance name and passes the configured parameters
// ---------------------------------------------------------------------------

struct ComponentEntry {
    std::filesystem::path file; ///< component.xml
    std::string vendor, library, name, version;
};

std::string firstTag(const std::string& text, const char* tag) {
    const std::regex re(std::string("<spirit:") + tag + R"(>\s*([^<]*?)\s*</spirit:)" + tag + ">");
    std::smatch m;
    return std::regex_search(text, m, re) ? m[1].str() : std::string();
}

/// component.xml files under the repositories (cached per repository).
std::vector<ComponentEntry> componentsIn(const std::vector<std::filesystem::path>& repos) {
    static std::mutex mutex;
    static std::map<std::string, std::vector<ComponentEntry>> cache;
    std::vector<ComponentEntry> all;
    for (const auto& repo : repos) {
        std::error_code ec;
        const std::string key = std::filesystem::weakly_canonical(repo, ec).string();
        std::vector<ComponentEntry> found;
        bool cached = false;
        {
            std::lock_guard lock(mutex);
            if (auto it = cache.find(key); it != cache.end()) {
                found = it->second;
                cached = true;
            }
        }
        if (!cached) {
            std::filesystem::recursive_directory_iterator it(repo, std::filesystem::directory_options::skip_permission_denied, ec),
                end;
            for (; !ec && it != end; it.increment(ec)) {
                if (it.depth() > 8) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->path().filename() != "component.xml" || !it->is_regular_file(ec))
                    continue;
                auto t = readSourceText(it->path());
                if (!t)
                    continue;
                // the component's own VLNV: before the first bus interface or model
                const std::string head = t->utf8.substr(0, t->utf8.find("<spirit:busInterfaces"));
                ComponentEntry e{it->path(), firstTag(head, "vendor"), firstTag(head, "library"), firstTag(head, "name"),
                                 firstTag(head, "version")};
                if (!e.name.empty())
                    found.push_back(std::move(e));
            }
            std::lock_guard lock(mutex);
            cache[key] = found;
        }
        all.insert(all.end(), found.begin(), found.end());
    }
    return all;
}

/// A parameter value from the .xci as a Verilog expression, by the type the
/// component declares.
std::string parameterValue(std::string value, const std::string& dataType) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    const std::string t = lower(dataType);
    const std::map<std::string, std::string> one = {{"v", value}};
    if (t == "string")
        return "\"" + value + "\"";
    if (t == "boolean" || t == "bool") {
        const std::string l = lower(value);
        return l == "true" || l == "1" ? "1'b1" : "1'b0";
    }
    if (t == "reg" || t == "std_logic" || t == "std_logic_vector" || t == "bit" || t == "bitstring") {
        if (!value.empty() && std::ranges::all_of(value, [](char c) { return c == '0' || c == '1'; }))
            return std::to_string(value.size()) + "'b" + value;
    }
    const long long n = num(one, "v", std::numeric_limits<long long>::min());
    if (n != std::numeric_limits<long long>::min())
        return std::to_string(n);
    return "\"" + value + "\"";
}

Result<XciModel> userIp(const XciInfo& info, const ComponentEntry& comp) {
    const auto text = readSourceText(comp.file);
    if (!text)
        return unsupported("IP '" + info.instance + "': cannot read " + toUtf8(comp.file));
    const std::string& c = text->utf8;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    // the synthesis view: its top module and file set
    static const std::regex viewRe(R"(<spirit:view>([\s\S]*?)</spirit:view>)");
    std::string modelName, fileSet;
    int best = -1;
    for (auto it = std::sregex_iterator(c.begin(), c.end(), viewRe); it != std::sregex_iterator(); ++it) {
        const std::string v = (*it)[1].str();
        const std::string env = firstTag(v, "envIdentifier");
        if (env.find("synthesis") == std::string::npos)
            continue;
        const std::string name = firstTag(v, "name");
        const int rank = name.find("anylanguage") != std::string::npos ? 3
                         : name.find("verilog") != std::string::npos ? 2 : 1;
        const std::string mn = firstTag(v, "modelName");
        if (mn.empty() || rank <= best)
            continue;
        best = rank;
        modelName = mn;
        fileSet = firstTag(v, "localName");
    }
    if (modelName.empty())
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): its component file " + toUtf8(comp.file) +
                           " has no synthesis view with a top module");
    if (info.ports.empty())
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): found in the IP repository (" +
                               toUtf8(comp.file) + "), but its ports are only known from a JSON .xci or the component "
                               "file next to an XML .xci (" + info.instance + ".xml)",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    XciModel out;
    // HDL files of the file set
    static const std::regex fileSetRe(R"(<spirit:fileSet>([\s\S]*?)</spirit:fileSet>)");
    static const std::regex fileRe(R"(<spirit:file>([\s\S]*?)</spirit:file>)");
    const auto base = comp.file.parent_path();
    for (auto it = std::sregex_iterator(c.begin(), c.end(), fileSetRe); it != std::sregex_iterator(); ++it) {
        const std::string fs = (*it)[1].str();
        if (firstTag(fs, "name") != fileSet)
            continue;
        for (auto f = std::sregex_iterator(fs.begin(), fs.end(), fileRe); f != std::sregex_iterator(); ++f) {
            const std::string body = (*f)[1].str();
            const std::string type = firstTag(body, "fileType");
            const std::string name = firstTag(body, "name");
            if (name.empty() || firstTag(body, "isIncludeFile") == "true")
                continue;
            const bool hdl = type == "verilogSource" || type == "systemVerilogSource" || type == "vhdlSource" ||
                             lower(std::filesystem::path(name).extension().string()) == ".xci";
            if (!hdl)
                continue;
            out.extraSources.push_back((base / std::filesystem::path(name)).lexically_normal());
            out.extraLibraries.push_back(type == "vhdlSource" ? firstTag(body, "logicalName") : std::string());
        }
    }
    if (out.extraSources.empty())
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): the synthesis file set of " + toUtf8(comp.file) +
                           " lists no HDL file");
    // parameters: the configured MODELPARAM values the component declares
    static const std::regex paramRe(R"(<spirit:modelParameter\b([^>]*)>([\s\S]*?)</spirit:modelParameter>)");
    static const std::regex typeRe(R"re(spirit:dataType="([^"]*)")re");
    std::ostringstream params;
    for (auto it = std::sregex_iterator(c.begin(), c.end(), paramRe); it != std::sregex_iterator(); ++it) {
        const std::string attrs = (*it)[1].str();
        const std::string name = firstTag((*it)[2].str(), "name");
        auto v = info.model.find(name);
        if (name.empty() || v == info.model.end())
            continue;
        std::smatch tm;
        const std::string type = std::regex_search(attrs, tm, typeRe) ? tm[1].str() : std::string();
        params << (params.tellp() > 0 ? ", " : "") << "." << name << "(" << parameterValue(v->second, type) << ")";
    }
    const auto ports = boundaryPorts(info);
    std::ostringstream w;
    emitHeader(w, info.instance, ports);
    w << "  " << modelName;
    if (params.tellp() > 0)
        w << " #(" << params.str() << ")";
    w << " inst (";
    for (std::size_t i = 0; i < ports.size(); ++i)
        w << (i ? ", " : "") << "." << ports[i].name << "(" << ports[i].name << ")";
    // ports of the top module that this configuration disables: inputs get
    // their default value (as Vivado's wrapper ties them), outputs stay open
    static const std::regex portRe(R"(<spirit:port>([\s\S]*?)</spirit:port>)");
    static const std::regex defRe(R"(<spirit:defaultValue\b[^>]*>\s*([^<\s]*)\s*</spirit:defaultValue>)");
    const auto modelEnd = c.find("</spirit:ports>");
    const std::string portsText = c.substr(0, modelEnd == std::string::npos ? c.size() : modelEnd);
    for (auto it = std::sregex_iterator(portsText.begin(), portsText.end(), portRe); it != std::sregex_iterator(); ++it) {
        const std::string body = (*it)[1].str();
        const std::string name = firstTag(body, "name");
        const std::string dir = firstTag(body, "direction");
        if (name.empty() || std::ranges::any_of(ports, [&](const SimplePort& sp) { return sp.name == name; }))
            continue;
        if (dir == "in") {
            std::smatch dm;
            std::string def = std::regex_search(body, dm, defRe) ? dm[1].str() : "0";
            const std::map<std::string, std::string> one = {{"v", def}};
            const long long dv = num(one, "v", 0);
            w << ", ." << name << "(" << (dv < 0 ? 0 : dv) << ")";
        }
        else {
            w << ", ." << name << "()";
        }
    }
    w << ");\nendmodule\n";
    out.module = info.instance;
    out.ip = vlnv;
    out.verilog = w.str();
    out.summary = "from the IP repository (" + toUtf8(comp.file) + "): top module '" + modelName + "', " +
                  std::to_string(out.extraSources.size()) + " HDL file(s)";
    return out;
}

} // namespace

/// The contents of the elements <tag ...>...</tag> in `text` (not nested in each other).
std::vector<std::string> elements(const std::string& text, const std::string& tag) {
    std::vector<std::string> out;
    const std::string open = "<" + tag, close = "</" + tag + ">";
    for (std::size_t p = text.find(open); p != std::string::npos; p = text.find(open, p)) {
        const char next = p + open.size() < text.size() ? text[p + open.size()] : '\0';
        if (next != '>' && next != ' ' && next != '\n' && next != '\t' && next != '\r') {
            p += open.size();
            continue; // <spirit:portMaps> while looking for <spirit:portMap>
        }
        const auto body = text.find('>', p);
        const auto end = text.find(close, body);
        if (body == std::string::npos || end == std::string::npos)
            break;
        out.push_back(text.substr(body + 1, end - body - 1));
        p = end + close.size();
    }
    return out;
}

std::optional<XciCellInterface> cellInterfaceFromText(std::string_view text) {
    const std::string t(text);
    const auto first = t.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::nullopt;
    XciCellInterface out;
    auto toNum = [](const nlohmann::json& n) -> long long {
        return n.is_string() ? std::stoll(n.get<std::string>()) : n.get<long long>();
    };
    if (t[first] == '{') { // JSON .xci (Vivado 2020.2+): the boundary
        try {
            const auto root = nlohmann::json::parse(t);
            const auto& ip = root.at("ip_inst");
            if (!ip.contains("boundary"))
                return std::nullopt;
            const auto& b = ip["boundary"];
            if (b.contains("ports"))
                for (const auto& [name, v] : b["ports"].items()) {
                    const auto& e = v.is_array() && !v.empty() ? v[0] : v;
                    XciPort p;
                    p.name = name;
                    const std::string dir = lower(e.value("direction", std::string("in")));
                    p.output = dir == "out";
                    p.inout = dir == "inout";
                    if (e.contains("size_left") && e.contains("size_right"))
                        p.range = std::pair{toNum(e["size_left"]), toNum(e["size_right"])};
                    if (e.contains("driver_value") && !p.output)
                        try {
                            out.defaults[name] = toNum(e["driver_value"]);
                        }
                        catch (const std::exception&) {
                        }
                    out.ports.push_back(std::move(p));
                }
            if (b.contains("interfaces"))
                for (const auto& [name, v] : b["interfaces"].items()) {
                    if (!v.is_object() || !v.contains("port_maps"))
                        continue;
                    XciBusInterface bi;
                    bi.name = name;
                    for (const auto& [logical, maps] : v["port_maps"].items()) {
                        const auto& e = maps.is_array() && !maps.empty() ? maps[0] : maps;
                        if (!e.is_object() || !e.contains("physical_name"))
                            continue;
                        XciPortMap pm;
                        pm.logical = logical;
                        pm.physical = e["physical_name"].get<std::string>();
                        if (e.contains("physical_left") && e.contains("physical_right"))
                            pm.slice = std::pair{toNum(e["physical_left"]), toNum(e["physical_right"])};
                        bi.ports.push_back(std::move(pm));
                    }
                    out.interfaces.push_back(std::move(bi));
                }
        }
        catch (const std::exception&) {
            return std::nullopt;
        }
    }
    else { // IP-XACT component file
        auto ports = parseComponentPorts(t);
        if (!ports)
            return std::nullopt;
        out.ports = std::move(*ports);
        static const std::regex nameRe(R"(<spirit:name>\s*([^<\s]+)\s*</spirit:name>)");
        static const std::regex defRe(R"(<spirit:defaultValue\b[^>]*>\s*(-?[0-9]+|0x[0-9A-Fa-f]+)\s*</spirit:defaultValue>)");
        for (const auto& body : elements(t, "spirit:port")) {
            std::smatch n, d;
            if (std::regex_search(body, n, nameRe) && std::regex_search(body, d, defRe))
                out.defaults[n[1].str()] = std::stoll(d[1].str(), nullptr, 0);
        }
        static const std::regex logRe(R"(<spirit:logicalPort>\s*<spirit:name>\s*([^<\s]+)\s*</spirit:name>)");
        static const std::regex physRe(R"(^\s*<spirit:name>\s*([^<\s]+)\s*</spirit:name>)");
        static const std::regex leftRe(R"(<spirit:left\b(?:[^>"]|"[^"]*")*>\s*(-?[0-9]+)\s*</spirit:left>)");
        static const std::regex rightRe(R"(<spirit:right\b(?:[^>"]|"[^"]*")*>\s*(-?[0-9]+)\s*</spirit:right>)");
        for (const auto& body : elements(t, "spirit:busInterface")) {
            std::smatch n;
            if (!std::regex_search(body, n, nameRe))
                continue;
            XciBusInterface bi;
            bi.name = n[1].str();
            for (const auto& pm : elements(body, "spirit:portMap")) {
                std::smatch l, p;
                const auto phys = elements(pm, "spirit:physicalPort");
                if (!std::regex_search(pm, l, logRe) || phys.empty() || !std::regex_search(phys[0], p, physRe))
                    continue;
                XciPortMap e;
                e.logical = l[1].str();
                e.physical = p[1].str();
                const std::string vecText = phys[0];
                std::smatch a, b;
                if (std::regex_search(vecText, a, leftRe) && std::regex_search(vecText, b, rightRe))
                    e.slice = std::pair{std::stoll(a[1].str()), std::stoll(b[1].str())};
                bi.ports.push_back(std::move(e));
            }
            out.interfaces.push_back(std::move(bi));
        }
        // HDL parameters (a module reference passes them to its module)
        static const std::regex valRe(R"(<spirit:value\b((?:[^>"]|"[^"]*")*)>([^<]*)</spirit:value>)");
        for (const auto& body : elements(t, "spirit:modelParameter")) {
            std::smatch n, v;
            if (!std::regex_search(body, n, nameRe) || !std::regex_search(body, v, valRe))
                continue;
            const std::string attrs = v[1].str();
            std::string value = unescapeXml(v[2].str());
            static const std::regex lenRe(R"re(spirit:bitStringLength="([0-9]+)")re");
            std::smatch len;
            if (attrs.find("format=\"bitString\"") != std::string::npos) {
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                if (value.starts_with("0x") || value.starts_with("0X"))
                    value = (std::regex_search(attrs, len, lenRe) ? len[1].str() : std::string()) + "'h" + value.substr(2);
                else
                    value = (std::regex_search(attrs, len, lenRe) ? len[1].str() : std::to_string(value.size())) +
                            "'b" + value;
            }
            else if (attrs.find("format=\"long\"") != std::string::npos ||
                     attrs.find("format=\"float\"") != std::string::npos) {
                // a number as written
            }
            else if (attrs.find("format=\"bool\"") != std::string::npos) {
                value = lower(value) == "true" ? "1" : "0";
            }
            else {
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                value = "\"" + value + "\"";
            }
            out.parameters.emplace_back(n[1].str(), value);
        }
    }
    if (out.ports.empty())
        return std::nullopt;
    return out;
}

std::optional<std::filesystem::path> generatedProductsDir(const std::filesystem::path& dir) {
    std::filesystem::path out;
    bool found = false;
    for (const auto& part : dir) {
        const std::string s = toUtf8(part);
        if (!found && s.size() > 5 && s.ends_with(".srcs")) {
            out /= std::filesystem::path(s.substr(0, s.size() - 5) + ".gen");
            found = true;
        }
        else {
            out /= part;
        }
    }
    if (!found)
        return std::nullopt;
    return out;
}

bool isXciFile(const std::filesystem::path& file) {
    const std::string ext = lower(file.extension().string());
    return ext == ".xci" || ext == ".xcix";
}

namespace {

/// An integer expression of literals (8, 16-1, 2*8-1, -1); nothing else.
std::optional<long long> literalInt(const vhdl::Expr* e) {
    if (!e)
        return std::nullopt;
    switch (e->kind) {
    case vhdl::ExprKind::Integer:
        if (e->overflow)
            return std::nullopt;
        return e->ival;
    case vhdl::ExprKind::Unary: {
        auto v = literalInt(e->lhs.get());
        if (!v)
            return std::nullopt;
        if (e->op == vhdl::Op::Minus)
            return -*v;
        if (e->op == vhdl::Op::Plus)
            return *v;
        return std::nullopt;
    }
    case vhdl::ExprKind::Binary: {
        auto l = literalInt(e->lhs.get());
        auto r = literalInt(e->rhs.get());
        if (!l || !r || std::llabs(*l) > (1LL << 30) || std::llabs(*r) > (1LL << 30))
            return std::nullopt;
        switch (e->op) {
        case vhdl::Op::Add: return *l + *r;
        case vhdl::Op::Sub: return *l - *r;
        case vhdl::Op::Mul: return *l * *r;
        default: return std::nullopt;
        }
    }
    default:
        return std::nullopt;
    }
}

/// The port's width from its subtype: a scalar bit type or a one-dimensional
/// bit-vector type with a literal range. Anything else: unknown.
bool portShape(const vhdl::SubtypeIndication* st, XciPort& p) {
    if (!st || !st->mark || st->range)
        return false;
    const vhdl::Expr& m = *st->mark;
    static const std::set<std::string> scalars = {"std_logic", "std_ulogic", "bit"};
    static const std::set<std::string> vectors = {"std_logic_vector", "std_ulogic_vector", "bit_vector", "unsigned",
                                                  "signed"};
    auto markKey = [](const vhdl::Expr& e) -> std::string {
        if (e.kind == vhdl::ExprKind::Simple)
            return e.ident.key;
        if (e.kind == vhdl::ExprKind::Selected) // ieee.std_logic_1164.std_logic
            return e.ident.key;
        return {};
    };
    if (m.kind == vhdl::ExprKind::Simple || m.kind == vhdl::ExprKind::Selected)
        return scalars.contains(markKey(m));
    if (m.kind != vhdl::ExprKind::Call || !m.prefix || !vectors.contains(markKey(*m.prefix)) || m.args.size() != 1 ||
        !m.args[0].actual || m.args[0].formal)
        return false;
    const vhdl::Expr& r = *m.args[0].actual;
    if (r.kind != vhdl::ExprKind::Range)
        return false;
    auto l = literalInt(r.lhs.get());
    auto h = literalInt(r.rhs.get());
    if (!l || !h)
        return false;
    p.range = std::pair{*l, *h};
    return true;
}

void collectComponents(const std::vector<vhdl::DeclPtr>& decls, const std::string& fileName,
                       std::map<std::string, XciComponent>& out, std::set<std::string>& conflicting) {
    for (const auto& d : decls) {
        if (!d || d->kind != vhdl::DeclKind::Component || d->names.empty())
            continue;
        XciComponent c;
        c.where = fileName + ":" + std::to_string(d->loc.line);
        bool ok = !d->ports.empty();
        for (const auto& i : d->ports) {
            if (!ok)
                break;
            for (const auto& n : i.names) {
                XciPort p;
                p.name = n.text;
                p.output = i.mode == vhdl::Mode::Out || i.mode == vhdl::Mode::Buffer;
                p.inout = i.mode == vhdl::Mode::InOut;
                if (!portShape(i.subtype.get(), p)) {
                    ok = false; // a width that is not literal: do not guess
                    break;
                }
                c.ports.push_back(std::move(p));
            }
        }
        if (!ok)
            continue;
        const std::string key = d->names.front().key;
        auto [it, inserted] = out.emplace(key, c);
        if (!inserted) {
            // the same component declared again (another architecture, a package):
            // only identical port lists are kept
            auto same = it->second.ports.size() == c.ports.size() &&
                        std::ranges::equal(it->second.ports, c.ports, [](const XciPort& a, const XciPort& b) {
                            return lower(a.name) == lower(b.name) && a.output == b.output && a.inout == b.inout &&
                                   a.range == b.range;
                        });
            if (!same)
                conflicting.insert(key);
        }
    }
}

} // namespace

std::map<std::string, XciComponent> vhdlComponentPorts(std::string_view text, const std::string& fileName) {
    std::map<std::string, XciComponent> out;
    std::set<std::string> conflicting;
    const auto parsed = vhdl::parseVhdl(text, 0);
    for (const auto& u : parsed.file.units)
        collectComponents(u.decls, fileName, out, conflicting);
    for (const auto& k : conflicting)
        out.erase(k);
    return out;
}

namespace {

Result<XciInfo> parseXci(std::string_view text) {
    std::string t(text);
    const auto first = t.find_first_not_of(" \t\r\n");
    return first != std::string::npos && t[first] == '{' ? parseJson(t) : parseXml(t);
}

/// `dir`: the folder of the .xci, where files the configuration refers to are
/// looked up (the memory interface's XML_INPUT_FILE).
Result<XciModel> dispatchModelled(const XciInfo& info, const std::filesystem::path* dir);

/// An IP known only by the clocks of its interfaces (below).
Result<XciModel> interfaceModel(const XciInfo& info);

/// A file of an IP's output products that an analysis cannot use: missing,
/// or encrypted (IEEE 1735 / Xilinx `protect blocks).
std::optional<std::string> unusableFile(const std::filesystem::path& f) {
    std::error_code ec;
    if (!std::filesystem::exists(f, ec))
        return "missing: " + toUtf8(f);
    const auto t = readSourceText(f);
    if (!t)
        return "cannot be read: " + toUtf8(f);
    if (t->utf8.find("protect begin_protected") != std::string::npos ||
        t->utf8.find("pragma protect key_block") != std::string::npos)
        return "encrypted: " + toUtf8(f);
    return std::nullopt;
}

/// An IP without a model, from its output products next to its component
/// file (<instance>.xml): the HDL of its synthesis view (the instance's
/// wrapper in synth/ and the IP's sources in ipshared/), or else its
/// simulation netlist (<instance>_sim_netlist.v), as Vivado synthesizes or
/// simulates it. nullopt with the reason when neither can be used.
Result<XciModel> generatedIp(const XciInfo& info, const std::filesystem::path& dir) {
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    std::vector<std::filesystem::path> where = {dir};
    if (const auto gen = generatedProductsDir(dir))
        where.push_back(*gen);
    std::filesystem::path compFile;
    std::string c;
    for (const auto& d : where)
        if (const auto t = readSourceText(d / (info.instance + ".xml"))) {
            compFile = d / (info.instance + ".xml");
            c = t->utf8;
            break;
        }
    if (c.empty())
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): no component file " + info.instance +
                           ".xml with the IP's output products");
    const auto base = compFile.parent_path();
    // file sets by name: (file, VHDL library)
    std::map<std::string, std::vector<std::pair<std::filesystem::path, std::string>>> sets;
    for (const auto& fs : elements(c, "spirit:fileSet")) {
        auto& list = sets[firstTag(fs, "name")];
        for (const auto& body : elements(fs, "spirit:file")) {
            const std::string type = firstTag(body, "fileType");
            const std::string name = firstTag(body, "name");
            if (name.empty() || firstTag(body, "isIncludeFile") == "true")
                continue;
            if (type != "verilogSource" && type != "systemVerilogSource" && type != "vhdlSource")
                continue;
            list.emplace_back((base / std::filesystem::path(name)).lexically_normal(),
                              type == "vhdlSource" ? firstTag(body, "logicalName") : std::string());
        }
    }
    // the synthesis view (the IP's own HDL) and the synthesis wrapper view (the instance)
    // (a view refers to several file sets: the IP's own and those of the IP it uses, in compile order)
    std::vector<std::string> synthSets, wrapperSets;
    static const std::regex localRe(R"(<spirit:localName>\s*([^<\s]+)\s*</spirit:localName>)");
    for (const auto& v : elements(c, "spirit:view")) {
        const std::string env = firstTag(v, "envIdentifier");
        auto& to = env.ends_with(":synthesis.wrapper") ? wrapperSets : synthSets;
        if (!env.ends_with(":synthesis.wrapper") && !env.ends_with(":synthesis"))
            continue;
        for (auto it = std::sregex_iterator(v.begin(), v.end(), localRe); it != std::sregex_iterator(); ++it)
            to.push_back((*it)[1].str());
    }
    const std::string wrapperSet = wrapperSets.empty() ? std::string() : wrapperSets.front();
    std::vector<std::pair<std::filesystem::path, std::string>> hdl;
    for (const auto* list : {&synthSets, &wrapperSets})
        for (const auto& setName : *list)
            for (const auto& f : sets[setName])
                hdl.push_back(f);
    std::string why;
    if (!wrapperSet.empty() && !hdl.empty()) {
        for (const auto& [f, lib] : hdl)
            if (const auto u = unusableFile(f)) {
                why = *u;
                break;
            }
        if (why.empty()) {
            XciModel out;
            out.module = info.instance;
            out.ip = vlnv;
            for (const auto& [f, lib] : hdl) {
                out.extraSources.push_back(f);
                out.extraLibraries.push_back(lib);
            }
            out.summary = "its generated HDL (synthesis view, " + std::to_string(hdl.size()) + " file(s))";
            out.notes.push_back("the IP's own HDL is analyzed, as Vivado synthesizes it: " + toUtf8(compFile));
            return out;
        }
    }
    else {
        why = "no synthesis view with the instance's wrapper";
    }
    // the simulation netlist
    const auto netlist = base / (info.instance + "_sim_netlist.v");
    if (const auto u = unusableFile(netlist)) {
        return unsupported("IP '" + info.instance + "' (" + vlnv + ") is not modelled, and its output products cannot "
                           "be used (" + why + "; simulation netlist " + *u + ")",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    }
    XciModel out;
    out.module = info.instance;
    out.ip = vlnv;
    out.extraSources.push_back(netlist);
    out.extraLibraries.push_back(std::string());
    out.summary = "its simulation netlist";
    out.notes.push_back("the gate-level simulation netlist is analyzed (" + toUtf8(netlist) + "); its HDL could not be "
                        "used (" + why + ")");
    return out;
}

/// The ports of an IP's synthesis stub (<instance>_stub.v, the black-box
/// declaration Vivado writes with the output products: what synthesis of the
/// design sees, so it is preferred to a component file that may be stale).
std::optional<std::vector<XciPort>> parseStubPorts(const std::string& text, const std::string& module) {
    static const std::regex modRe(R"(\bmodule\s+([A-Za-z_][\w$]*)\s*\()");
    std::smatch m;
    std::string::const_iterator from = text.begin();
    std::size_t start = std::string::npos;
    while (std::regex_search(from, text.end(), m, modRe)) {
        if (m[1].str() == module) {
            start = static_cast<std::size_t>(m.position(0) + (from - text.begin()));
            break;
        }
        from = m[0].second;
    }
    if (start == std::string::npos)
        return std::nullopt;
    const auto end = text.find("endmodule", start);
    std::string body = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // drop comments (the stub's /* synthesis ... */ attribute lists the pins too)
    body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/)"), " ");
    body = std::regex_replace(body, std::regex(R"(//[^\n]*)"), " ");
    static const std::regex declRe(R"(^\s*(input|output|inout)\s*(?:wire\s*)?(?:\[\s*(-?\d+)\s*:\s*(-?\d+)\s*\])?\s*([\s\S]+)$)");
    std::vector<XciPort> out;
    std::stringstream statements(body);
    for (std::string st; std::getline(statements, st, ';');) {
        std::smatch d;
        if (!std::regex_match(st, d, declRe))
            continue;
        std::stringstream names(d[4].str());
        for (std::string n; std::getline(names, n, ',');) {
            n.erase(std::remove_if(n.begin(), n.end(), [](unsigned char c) { return std::isspace(c) != 0; }), n.end());
            if (n.empty())
                continue;
            XciPort p;
            p.name = n;
            p.output = d[1].str() == "output";
            p.inout = d[1].str() == "inout";
            if (d[2].matched)
                p.range = std::pair{std::stoll(d[2].str()), std::stoll(d[3].str())};
            out.push_back(std::move(p));
        }
    }
    if (out.empty())
        return std::nullopt;
    return out;
}

/// The bus interfaces of each clock interface of a component file
/// (ASSOCIATED_BUSIF "M00_AXI:S00_AXI"): lower-case interface -> clock port.
std::map<std::string, std::string> clockAssociations(const std::string& text) {
    std::map<std::string, std::string> out;
    static const std::regex physRe(R"(<spirit:physicalPort>\s*<spirit:name>\s*([^<\s]+)\s*</spirit:name>)");
    static const std::regex assocRe(
        R"(<spirit:name>\s*ASSOCIATED_BUSIF\s*</spirit:name>\s*<spirit:value\b(?:[^>"]|"[^"]*")*>([^<]*)</spirit:value>)");
    for (const auto& body : elements(text, "spirit:busInterface")) {
        if (body.find("spirit:name=\"clock\"") == std::string::npos)
            continue;
        std::smatch p, a;
        if (!std::regex_search(body, p, physRe) || !std::regex_search(body, a, assocRe))
            continue;
        std::stringstream list(a[1].str());
        for (std::string bus; std::getline(list, bus, ':');)
            if (!bus.empty())
                out[lower(bus)] = p[1].str();
    }
    return out;
}

Result<XciModel> dispatch(XciInfo info, const std::filesystem::path* dir, const XciContext* context = nullptr) {
    // XML .xci: the port list from the component file next to it, when present,
    // otherwise from a VHDL component declaration of the IP in the design
    if (!info.json && info.ports.empty() && dir) {
        std::vector<std::filesystem::path> where = {*dir};
        if (const auto gen = generatedProductsDir(*dir))
            where.push_back(*gen); // Vivado 2020.2+: <project>.gen
        for (const auto& d : where)
            if (auto t = readSourceText(d / (info.instance + ".xml")))
                if (auto ports = parseComponentPorts(t->utf8)) {
                    info.ports = std::move(*ports);
                    info.busClock = clockAssociations(t->utf8);
                    if (const auto ci = cellInterfaceFromText(t->utf8))
                        for (const auto& bi : ci->interfaces)
                            if (const auto clk = info.busClock.find(lower(bi.name)); clk != info.busClock.end())
                                for (const auto& pm : bi.ports)
                                    info.portClock[lower(pm.physical)] = clk->second;
                    break;
                }
        // the synthesis stub, when Vivado wrote one, has the ports synthesis sees
        for (const auto& d : where)
            if (auto t = readSourceText(d / (info.instance + "_stub.v")))
                if (auto ports = parseStubPorts(t->utf8, info.instance)) {
                    info.ports = std::move(*ports);
                    break;
                }
    }
    std::string fromComponent;
    if (!info.json && info.ports.empty() && context)
        if (const auto it = context->vhdlComponents.find(lower(info.instance)); it != context->vhdlComponents.end()) {
            info.ports = it->second.ports;
            fromComponent = it->second.where;
        }
    auto r = dispatchModelled(info, dir);
    if (r && !fromComponent.empty())
        r.value().notes.push_back("ports from the VHDL component declaration at " + fromComponent);
    if (r || r.error().code != "FE-XCI-UNSUPPORTED")
        return r;
    // not modelled: the IP itself, from an IP repository
    if (context)
        for (const auto& comp : componentsIn(context->ipRepositories))
            if (lower(comp.vendor) == lower(info.vendor) && lower(comp.library) == lower(info.library) &&
                comp.name == info.name && comp.version == info.version)
                return userIp(info, comp);
    // or its own output products (generated HDL, simulation netlist)
    std::string why;
    if (dir && !info.json) {
        auto g = generatedIp(info, *dir);
        if (g)
            return g;
        why = g.error().message;
    }
    // or else by the clocks of its interfaces
    if (auto im = interfaceModel(info)) {
        if (!why.empty())
            im.value().notes.push_back(why);
        return im;
    }
    auto d = r.error();
    if (!why.empty())
        d.notes.insert(d.notes.begin(), why);
    return d;
}

/// UltraScale / UltraScale+ transceiver wizard (gtwizard_ultrascale): the
/// clock domains of its ports. The user clocks are clocks of their own (a
/// transceiver clock is not related to the fabric clocks by any constraint
/// the IP gives): the source clock (TXOUTCLK/RXOUTCLK) with the configured
/// frequency, USRCLK and USRCLK2 derived from it. User data, gearbox, 8b/10b
/// and the reset-done / active outputs are in the USRCLK2 domain of their
/// direction; the DRP in drpclk; the reset controller's outputs in the
/// free-running clock; PMA / programmable-divider reset-done and power-good
/// outputs are asynchronous; reset and analog-control inputs are synchronized
/// (or static) inside the IP.
Result<XciModel> gtWizard(const XciInfo& info) {
    const auto& m = info.model;
    const auto unsupportedGt = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + info.vendor + ":" + info.library + ":" + info.name + ":" +
                               info.version + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    const unsigned n = static_cast<unsigned>(std::max(1LL, num(m, "C_TOTAL_NUM_CHANNELS", 1)));
    const long long gtType = num(m, "C_GT_TYPE", 0); // 0 GTHE3, 1 GTYE3, 2 GTHE4, 3 GTYE4
    const std::string serial = gtType == 1 || gtType == 3 ? "gty" : "gth";
    auto mhz = [&](const std::string& key) {
        try {
            return std::stod(text(m, key, "0"));
        }
        catch (...) {
            return 0.0;
        }
    };
    // port widths per channel (PG182; the unisim GT*E3/E4_CHANNEL widths)
    const std::map<std::string, unsigned> perChannel = {
        {"loopback_in", 3},          {"txdiffctrl_in", gtType == 0 ? 4U : 5U},
        {"txprecursor_in", 5},       {"txpostcursor_in", 5},
        {"txheader_in", 6},          {"txsequence_in", 7},
        {"rxheader_out", 6},         {"rxheadervalid_out", 2},
        {"rxdatavalid_out", 2},      {"rxstartofseq_out", 2},
        {"txctrl0_in", 16},          {"txctrl1_in", 16},
        {"txctrl2_in", 8},           {"rxctrl0_out", 16},
        {"rxctrl1_out", 16},         {"rxctrl2_out", 8},
        {"rxctrl3_out", 8},          {"rxbufstatus_out", 3},
        {"txbufstatus_out", 2},      {"rxclkcorcnt_out", 2},
        {"drpaddr_in", gtType == 0 ? 9U : 10U},
        {"drpdi_in", 16},            {"drpdo_out", 16},
        {"cpllrefclksel_in", 3}};
    // channel-level 1-bit ports and wizard-level ports (width 1)
    const std::set<std::string> wizardLevel = {
        "gtwiz_userclk_tx_reset_in",   "gtwiz_userclk_tx_srcclk_out", "gtwiz_userclk_tx_usrclk_out",
        "gtwiz_userclk_tx_usrclk2_out", "gtwiz_userclk_tx_active_out", "gtwiz_userclk_tx_active_in",
        "gtwiz_userclk_rx_reset_in",   "gtwiz_userclk_rx_srcclk_out", "gtwiz_userclk_rx_usrclk_out",
        "gtwiz_userclk_rx_usrclk2_out", "gtwiz_userclk_rx_active_out", "gtwiz_userclk_rx_active_in",
        "gtwiz_reset_clk_freerun_in",  "gtwiz_reset_all_in",          "gtwiz_reset_tx_pll_and_datapath_in",
        "gtwiz_reset_tx_datapath_in",  "gtwiz_reset_rx_pll_and_datapath_in", "gtwiz_reset_rx_datapath_in",
        "gtwiz_reset_qpll0lock_in",    "gtwiz_reset_qpll1lock_in",    "gtwiz_reset_cplllock_in",
        "gtwiz_reset_rx_cdr_stable_out", "gtwiz_reset_tx_done_out",   "gtwiz_reset_rx_done_out",
        "gtwiz_reset_qpll0reset_out",  "gtwiz_reset_qpll1reset_out",
        // buffer bypass controller helper block (PG182)
        "gtwiz_buffbypass_tx_reset_in", "gtwiz_buffbypass_tx_start_user_in", "gtwiz_buffbypass_tx_done_out",
        "gtwiz_buffbypass_tx_error_out", "gtwiz_buffbypass_rx_reset_in", "gtwiz_buffbypass_rx_start_user_in",
        "gtwiz_buffbypass_rx_done_out", "gtwiz_buffbypass_rx_error_out"};
    const std::set<std::string> oneBitPerChannel = {
        "drpclk_in",        "drpen_in",          "drpwe_in",           "drprdy_out",          "qpll0clk_in",
        "qpll0refclk_in",   "qpll1clk_in",       "qpll1refclk_in",     "gtrefclk0_in",        "gtrefclk1_in",
        "rxgearboxslip_in", "rxpolarity_in",     "txpolarity_in",      "rx8b10ben_in",        "tx8b10ben_in",
        "rxcommadeten_in",  "rxmcommaalignen_in", "rxpcommaalignen_in", "rxbufreset_in",       "rxbyteisaligned_out",
        "rxbyterealign_out", "rxcommadet_out",   "rxpmaresetdone_out", "txpmaresetdone_out",  "rxprgdivresetdone_out",
        "txprgdivresetdone_out", "gtpowergood_out", "rxcdrlock_out",   "rxresetdone_out",     "txresetdone_out",
        "rxusrclk_in",      "rxusrclk2_in",      "txusrclk_in",        "txusrclk2_in",        "rxoutclk_out",
        "txoutclk_out",     "cplllock_out",      "rxsyncdone_out",     "rxphaligndone_out",   "rxdlysresetdone_out",
        "txpmareset_in",    "txpcsreset_in",     "rxpmareset_in",      "rxpcsreset_in",       "rxcdrreset_in",
        "gtgrefclk_in",     "cplllocken_in",     "cplllockdetclk_in",  "cpllpd_in",           "cpllreset_in",
        "cpllfbclklost_out", "cpllrefclklost_out", "txinhibit_in",     "rxslide_in",          serial + "rxn_in",    serial + "rxp_in",     serial + "txn_out",
        serial + "txp_out"};

    // the ports: the JSON boundary, otherwise the enabled ports of the configuration
    struct P {
        std::string name;
        bool out;
        unsigned width;
    };
    std::vector<P> ports;
    std::vector<std::string> unknown;
    if (!info.ports.empty()) {
        for (const auto& b : info.ports)
            ports.push_back({b.name, b.output,
                             b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U});
    }
    else {
        std::vector<std::string> names;
        for (const auto& [key, value] : info.user)
            if (key.starts_with("INTERNAL_PORT_ENABLED_") && num(info.user, key, 0) > 0) // -1: disabled
                names.push_back(lower(key.substr(std::string_view("INTERNAL_PORT_ENABLED_").size())));
        std::stringstream opt(text(info.user, "ENABLE_OPTIONAL_PORTS", ""));
        for (std::string w; opt >> w;)
            if (std::ranges::find(names, lower(w)) == names.end())
                names.push_back(lower(w));
        if (names.empty())
            return unsupportedGt("the configuration names no ports (INTERNAL_PORT_ENABLED_*, ENABLE_OPTIONAL_PORTS)");
        for (const auto& name : names) {
            const bool out = name.ends_with("_out");
            if (const auto it = perChannel.find(name); it != perChannel.end())
                ports.push_back({name, out, it->second * n});
            else if (name == "gtwiz_userdata_tx_in")
                ports.push_back({name, false, static_cast<unsigned>(std::max(1LL, num(m, "C_TX_USER_DATA_WIDTH", 32))) * n});
            else if (name == "gtwiz_userdata_rx_out")
                ports.push_back({name, true, static_cast<unsigned>(std::max(1LL, num(m, "C_RX_USER_DATA_WIDTH", 32))) * n});
            else if (wizardLevel.contains(name))
                ports.push_back({name, out, 1});
            else if (oneBitPerChannel.contains(name))
                ports.push_back({name, out, n});
            else
                unknown.push_back(name);
        }
    }
    if (!unknown.empty()) {
        std::string list;
        for (const auto& u : unknown)
            list += (list.empty() ? "" : ", ") + u;
        return unsupportedGt("ports whose width or clock domain is not modelled: " + list);
    }
    auto has = [&](const std::string& name) { return std::ranges::any_of(ports, [&](const P& p) { return p.name == name; }); };

    // clock domain of every port
    enum class D { TxUser, RxUser, Drp, Freerun, Async, Ignored, Clock };
    auto domainOf = [&](const std::string& p) -> std::optional<D> {
        static const std::set<std::string> tx = {"gtwiz_userdata_tx_in", "txheader_in", "txsequence_in", "txctrl0_in",
                                                 "txctrl1_in", "txctrl2_in", "tx8b10ben_in", "txpolarity_in",
                                                 "gtwiz_reset_tx_done_out", "gtwiz_userclk_tx_active_out",
                                                 "gtwiz_userclk_tx_active_in", "txbufstatus_out", "txresetdone_out",
                                                 "gtwiz_buffbypass_tx_start_user_in", "gtwiz_buffbypass_tx_done_out",
                                                 "gtwiz_buffbypass_tx_error_out"};
        static const std::set<std::string> rx = {
            "gtwiz_userdata_rx_out", "rxheader_out",       "rxheadervalid_out",  "rxdatavalid_out",
            "rxstartofseq_out",      "rxctrl0_out",        "rxctrl1_out",        "rxctrl2_out",
            "rxctrl3_out",           "rxbyteisaligned_out", "rxbyterealign_out", "rxcommadet_out",
            "rxbufstatus_out",       "rxclkcorcnt_out",    "gtwiz_reset_rx_done_out", "gtwiz_userclk_rx_active_out",
            "gtwiz_userclk_rx_active_in", "rxgearboxslip_in", "rxpolarity_in",   "rx8b10ben_in",
            "rxcommadeten_in",       "rxmcommaalignen_in", "rxpcommaalignen_in", "rxresetdone_out",
            "rxslide_in",            "gtwiz_buffbypass_rx_start_user_in", "gtwiz_buffbypass_rx_done_out",
            "gtwiz_buffbypass_rx_error_out"};
        static const std::set<std::string> asyncOut = {"rxpmaresetdone_out", "txpmaresetdone_out",
                                                       "rxprgdivresetdone_out", "txprgdivresetdone_out",
                                                       "gtpowergood_out", "rxcdrlock_out", "cplllock_out",
                                                       // UG576/UG578: asynchronous status of the channel
                                                       "rxsyncdone_out", "rxphaligndone_out", "rxdlysresetdone_out",
                                                       "cpllfbclklost_out", "cpllrefclklost_out"};
        static const std::set<std::string> freerun = {"gtwiz_reset_rx_cdr_stable_out", "gtwiz_reset_qpll0reset_out",
                                                      "gtwiz_reset_qpll1reset_out"};
        static const std::set<std::string> clocks = {
            "gtwiz_userclk_tx_srcclk_out", "gtwiz_userclk_tx_usrclk_out", "gtwiz_userclk_tx_usrclk2_out",
            "gtwiz_userclk_rx_srcclk_out", "gtwiz_userclk_rx_usrclk_out", "gtwiz_userclk_rx_usrclk2_out",
            "txoutclk_out", "rxoutclk_out", "txusrclk_in", "txusrclk2_in", "rxusrclk_in", "rxusrclk2_in",
            "drpclk_in", "gtwiz_reset_clk_freerun_in"};
        if (tx.contains(p))
            return D::TxUser;
        if (rx.contains(p))
            return D::RxUser;
        if (p.starts_with("drp") && p != "drpclk_in")
            return D::Drp;
        if (freerun.contains(p))
            return D::Freerun;
        if (asyncOut.contains(p))
            return D::Async;
        if (clocks.contains(p))
            return D::Clock;
        // synchronized inside the IP, static analog controls, reference clocks, serial pins
        if (p.starts_with("gtwiz_reset_") || p.starts_with("gtwiz_userclk_") || p == "loopback_in" ||
            p == "txdiffctrl_in" || p == "txprecursor_in" || p == "txpostcursor_in" || p == "rxbufreset_in" ||
            p.starts_with("qpll") || p.starts_with("gtrefclk") || p.starts_with("gthrx") || p.starts_with("gtyrx") ||
            p.starts_with("gthtx") || p.starts_with("gtytx") ||
            // asynchronous resets and static controls of the channel and its CPLL (UG576/UG578),
            // the buffer bypass reset (synchronized in the helper block), the global reference clock
            p == "txpmareset_in" || p == "txpcsreset_in" || p == "rxpmareset_in" || p == "rxpcsreset_in" ||
            p == "rxcdrreset_in" || p == "txinhibit_in" || p == "gtgrefclk_in" || p.starts_with("cpll") ||
            p == "gtwiz_buffbypass_tx_reset_in" || p == "gtwiz_buffbypass_rx_reset_in")
            return D::Ignored;
        return std::nullopt;
    };
    for (const auto& p : ports)
        if (!domainOf(p.name))
            unknown.push_back(p.name);
    if (!unknown.empty()) {
        std::string list;
        for (const auto& u : unknown)
            list += (list.empty() ? "" : ", ") + u;
        return unsupportedGt("ports whose clock domain is not modelled: " + list);
    }

    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << info.instance << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].out ? "output wire " : "input  wire ") << vec(ports[i].width) << " " << ports[i].name
          << (i + 1 < ports.size() ? ",\n" : "\n");
    v << ");\n";

    // user clocks of a direction: from the IP's helper (source, usrclk,
    // usrclk2 out) or from the user (usrclk2 in, the source out)
    std::string summary;
    auto userClock = [&](const std::string& dir) -> std::optional<std::string> {
        const std::string up = dir == "tx" ? "TX" : "RX";
        const double fUsr = mhz("C_" + up + "_USRCLK_FREQUENCY");
        const double fUsr2 = mhz("C_" + up + "_USRCLK2_FREQUENCY");
        const double srcRatio = std::max(1.0, mhz("C_" + up + "_USER_CLOCKING_RATIO_FSRC_FUSRCLK"));
        const double fSrc = fUsr * srcRatio;
        const std::string helper = "gtwiz_userclk_" + dir + "_";
        const std::string srcPin = has(helper + "srcclk_out") ? helper + "srcclk_out"
                                   : has(dir + "outclk_out")  ? dir + "outclk_out"
                                                              : "";
        // the source clock: a transceiver output clock of its own
        v << "  (* OPENCDC_CLOCK_PIN = \"" << (srcPin.empty() ? dir + "outclk" : srcPin) << "\"";
        if (fSrc > 0)
            v << ", OPENCDC_CLOCK_PERIOD = " << fmtReal(1000.0 / fSrc);
        v << " *) wire oc_" << dir << "_src;\n  opencdc_async_source u_oc_" << dir << "_src (oc_" << dir << "_src);\n";
        if (!srcPin.empty())
            v << "  assign " << srcPin << " = {" << n << "{oc_" << dir << "_src}};\n";
        if (has(dir + "usrclk2_in")) { // user clocking in the example design: the user's clock
            summary += ", " + dir + " " + fmtReal(fUsr2) + " MHz (user clocking outside the IP)";
            return dir + "usrclk2_in[0]";
        }
        if (fUsr <= 0 || fUsr2 <= 0)
            return std::nullopt;
        // through the source clock port (when the IP has it), so that
        // get_clocks -of_objects on that pin finds the user clocks
        const std::string srcNet = srcPin.empty() ? "oc_" + dir + "_src" : srcPin + "[0]";
        auto derived = [&](const std::string& pin, double f) {
            v << "  (* OPENCDC_CLOCK_PIN = \"" << pin << "\", OPENCDC_CLOCK_RATIO = " << fmtReal(fSrc / f)
              << " *)\n  reg oc_" << pin << "_q = 1'b0;\n  always @(posedge " << srcNet << ") oc_" << pin
              << "_q <= ~oc_" << pin << "_q;\n";
            if (has(pin))
                v << "  assign " << pin << " = oc_" << pin << "_q;\n";
        };
        derived(helper + "usrclk_out", fUsr);
        derived(helper + "usrclk2_out", fUsr2);
        summary += ", " + dir + " usrclk2 " + fmtReal(fUsr2) + " MHz";
        return "oc_" + helper + "usrclk2_out_q";
    };
    const bool txUsed = std::ranges::any_of(ports, [&](const P& p) { return domainOf(p.name) == D::TxUser; });
    const bool rxUsed = std::ranges::any_of(ports, [&](const P& p) { return domainOf(p.name) == D::RxUser; });
    std::optional<std::string> txClk, rxClk;
    if (txUsed || has("gtwiz_userclk_tx_usrclk2_out") || has("txoutclk_out")) {
        txClk = userClock("tx");
        if (!txClk)
            return unsupportedGt("the transmitter user clock frequencies (C_TX_USRCLK_FREQUENCY, C_TX_USRCLK2_FREQUENCY) "
                                 "are not configured");
    }
    if (rxUsed || has("gtwiz_userclk_rx_usrclk2_out") || has("rxoutclk_out")) {
        rxClk = userClock("rx");
        if (!rxClk)
            return unsupportedGt("the receiver user clock frequencies (C_RX_USRCLK_FREQUENCY, C_RX_USRCLK2_FREQUENCY) are "
                                 "not configured");
    }

    // every domain: one register mixing its inputs; its outputs depend on it
    auto domain = [&](D d, const std::string& clk, const std::string& tag) {
        std::string inputs;
        for (const auto& p : ports)
            if (!p.out && domainOf(p.name) == d)
                inputs += (inputs.empty() ? "" : ", ") + p.name;
        bool outputs = false;
        for (const auto& p : ports)
            outputs = outputs || (p.out && domainOf(p.name) == d);
        if (inputs.empty() && !outputs)
            return;
        v << "  reg oc_" << tag << " = 1'b0;\n  always @(posedge " << clk << ") oc_" << tag << " <= "
          << (inputs.empty() ? std::string("~oc_" + tag) : "^{" + inputs + "}") << ";\n";
        for (const auto& p : ports)
            if (p.out && domainOf(p.name) == d)
                v << "  reg " << vec(p.width) << " oc_" << p.name << "_q = " << p.width << "'b0;\n  always @(posedge "
                  << clk << ") oc_" << p.name << "_q <= {" << p.width << "{oc_" << tag << "}};\n  assign " << p.name
                  << " = oc_" << p.name << "_q;\n";
    };
    if (txClk)
        domain(D::TxUser, *txClk, "tx");
    if (rxClk)
        domain(D::RxUser, *rxClk, "rx");
    if (has("drpclk_in"))
        domain(D::Drp, "drpclk_in[0]", "drp");
    if (has("gtwiz_reset_clk_freerun_in"))
        domain(D::Freerun, "gtwiz_reset_clk_freerun_in[0]", "freerun");
    for (const auto& p : ports) {
        if (!p.out)
            continue;
        const D d = *domainOf(p.name);
        if (d == D::Async) {
            v << "  wire oc_" << p.name << "_a;\n  opencdc_async_source u_oc_" << p.name << " (oc_" << p.name
              << "_a);\n  assign " << p.name << " = {" << p.width << "{oc_" << p.name << "_a}};\n";
        }
        else if (d == D::Ignored) {
            v << "  assign " << p.name << " = " << p.width << "'b0;\n"; // serial pins
        }
        else if ((d == D::Drp && !has("drpclk_in")) || (d == D::Freerun && !has("gtwiz_reset_clk_freerun_in"))) {
            return unsupportedGt("'" + p.name + "' without its clock port");
        }
    }
    v << "endmodule\n";

    XciModel model;
    model.module = info.instance;
    model.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    model.summary = std::to_string(n) + " channel(s), " +
                    (gtType == 0 ? "GTHE3" : gtType == 1 ? "GTYE3" : gtType == 2 ? "GTHE4" : "GTYE4") +
                    ", line rate " + text(m, "C_TX_LINE_RATE", "?") + " Gb/s" + summary;
    model.verilog = v.str();
    model.notes.push_back("transceiver user clocks are clocks of their own (relate them to other clocks with "
                          "set_clock_groups or create_clock on the reference clock as Vivado does); PMA reset-done "
                          "and power-good outputs are asynchronous");
    return model;
}

/// 7-series transceiver wizard (gtwizard 3.x: GTP, GTX, GTH). Its port list
/// does not follow from a few parameters, so the model needs the ports of the
/// instance (a JSON .xci, the component file, or a VHDL component declaration).
/// Ports are gt<N>_<port> per channel; their clock domains as for the
/// UltraScale wizard: user data in the channel's TX/RX USRCLK2 (the user's
/// clocks, inputs of the IP), DRP in drpclk, the reset state machines'
/// outputs in sysclk_in; TXOUTCLK/RXOUTCLK are clocks of their own.
Result<XciModel> gtWizard7(const XciInfo& info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const auto unsupportedGt = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    if (info.ports.empty())
        return unsupportedGt("the 7-series Transceivers Wizard is modelled from the ports of the instance: give a "
                             "JSON .xci, the component file next to the XML .xci (" + info.instance +
                             ".xml), or a VHDL component declaration of the IP in the sources");
    auto real = [&](const std::string& key) {
        try {
            return std::stod(text(m, key, "0"));
        }
        catch (...) {
            return 0.0;
        }
    };
    enum class D { TxUser, RxUser, Drp, Sys, Async, Ignored, ClockIn, ClockOut };
    // the port without its channel prefix
    static const std::regex chanRe(R"(^gt([0-9]+)_(.*)$)");
    auto split = [](const std::string& name) -> std::pair<int, std::string> {
        std::smatch mm;
        const std::string l = lower(name);
        if (std::regex_match(l, mm, chanRe))
            return {std::stoi(mm[1].str()), mm[2].str()};
        return {-1, l};
    };
    static const std::set<std::string> tx = {
        "txdata_in",       "txheader_in",       "txsequence_in",    "txstartseq_in",     "txgearboxready_out",
        "txcharisk_in",    "txchardispmode_in", "txchardispval_in", "tx8b10ben_in",      "txpolarity_in",
        "txresetdone_out", "txbufstatus_out",   "txprbssel_in",     "txprbsforceerr_in", "txelecidle_in"};
    static const std::set<std::string> rx = {
        "rxdata_out",          "rxheader_out",       "rxheadervalid_out",  "rxdatavalid_out",  "rxstartofseq_out",
        "rxgearboxslip_in",    "rxpolarity_in",      "rxresetdone_out",    "rxcharisk_out",    "rxchariscomma_out",
        "rxdisperr_out",       "rxnotintable_out",   "rxbyteisaligned_out", "rxbyterealign_out", "rxcommadet_out",
        "rxmcommaalignen_in",  "rxpcommaalignen_in", "rxbufstatus_out",    "rxclkcorcnt_out",  "rxprbserr_out",
        "rxprbssel_in",        "rxprbscntreset_in",  "rx8b10ben_in",       "rxslide_in",       "rxvalid_out"};
    static const std::set<std::string> sys = {"tx_fsm_reset_done_out", "rx_fsm_reset_done_out", "tx_mmcm_reset_out",
                                              "rx_mmcm_reset_out",     "qpllreset_out",         "pll0reset_out",
                                              "pll1reset_out",         "cpllreset_out"};
    // UG476 / UG482: asynchronous status outputs of the channel
    static const std::set<std::string> asyncOut = {
        "eyescandataerror_out", "dmonitorout_out",   "rxmonitorout_out",    "cplllock_out",         "cpllfbclklost_out",
        "cpllrefclklost_out",   "rxpmaresetdone_out", "txpmaresetdone_out", "rxcdrlock_out",        "rxelecidle_out",
        "rxphaligndone_out",    "rxdlysresetdone_out", "rxsyncdone_out",    "txphaligndone_out",    "txdlysresetdone_out",
        "txsyncdone_out",       "rxoutclkpcs_out",    "rxphmonitor_out",     "rxphslipmonitor_out"};
    // synchronized in the IP, static or analog controls, reference clocks, serial pins
    static const std::set<std::string> ignored = {
        "soft_reset_in",       "soft_reset_tx_in",    "soft_reset_rx_in",    "dont_reset_on_data_error_in",
        "data_valid_in",       "tx_mmcm_lock_in",     "rx_mmcm_lock_in",     "qplllock_in",
        "qpllrefclklost_in",   "pll0lock_in",         "pll1lock_in",         "pll0refclklost_in",
        "pll1refclklost_in",   "loopback_in",         "eyescanreset_in",     "eyescantrigger_in",
        "rxcdrovrden_in",      "rxcdrhold_in",        "rxdfelpmreset_in",    "rxmonitorsel_in",
        "rxlpmen_in",          "rxlpmhfhold_in",      "rxlpmlfhold_in",      "rxlpmhfovrden_in",
        "rxlpmlfklovrden_in",  "rxdfeagcovrden_in",   "rxlpmreset_in",       "txdiffctrl_in",
        "txpostcursor_in",     "txprecursor_in",      "txmaincursor_in",     "gtrxreset_in",
        "gttxreset_in",        "rxuserrdy_in",        "txuserrdy_in",        "rxpcsreset_in",
        "rxpmareset_in",       "txpcsreset_in",       "txpmareset_in",       "rxbufreset_in",
        "cpllreset_in",        "cpllpd_in",           "cplllockdetclk_in",   "txinhibit_in",
        "qplloutclk_in",       "qplloutrefclk_in",    "gtrefclk0_in",        "gtrefclk1_in",
        "gtnorthrefclk0_in",   "gtnorthrefclk1_in",   "gtsouthrefclk0_in",   "gtsouthrefclk1_in",
        "gtgrefclk_in",        "pll0clk_in",          "pll0refclk_in",       "pll1clk_in",
        "pll1refclk_in",       "gtxrxp_in",           "gtxrxn_in",           "gtprxp_in",
        "gtprxn_in",           "gthrxp_in",           "gthrxn_in",           "gtxtxp_out",
        "gtxtxn_out",          "gtptxp_out",          "gtptxn_out",          "gthtxp_out",
        "gthtxn_out",          "pll0outclk_in",       "pll0outrefclk_in",    "pll1outclk_in",
        "pll1outrefclk_in"};
    static const std::set<std::string> clockIn = {"sysclk_in",  "drpclk_in",    "txusrclk_in",
                                                  "txusrclk2_in", "rxusrclk_in", "rxusrclk2_in"};
    static const std::set<std::string> clockOut = {"txoutclk_out", "rxoutclk_out", "txoutclkfabric_out",
                                                   "rxoutclkfabric_out", "txoutclkpcs_out"};
    auto domainOf = [&](const std::string& port) -> std::optional<D> {
        if (tx.contains(port))
            return D::TxUser;
        if (rx.contains(port))
            return D::RxUser;
        if (port.starts_with("drp") && port != "drpclk_in")
            return D::Drp;
        if (sys.contains(port))
            return D::Sys;
        if (asyncOut.contains(port))
            return D::Async;
        if (clockIn.contains(port))
            return D::ClockIn;
        if (clockOut.contains(port))
            return D::ClockOut;
        if (ignored.contains(port))
            return D::Ignored;
        return std::nullopt;
    };
    struct P {
        std::string name; ///< as declared
        int channel;      ///< -1: wizard level
        std::string port; ///< lower case, without the channel prefix
        bool out;
        unsigned width;
        D d;
    };
    std::vector<P> ports;
    std::vector<std::string> unknown;
    std::set<int> channels;
    for (const auto& b : info.ports) {
        auto [ch, port] = split(b.name);
        const auto d = domainOf(port);
        if (!d || b.inout || (b.output != port.ends_with("_out"))) {
            unknown.push_back(b.name);
            continue;
        }
        if (ch >= 0)
            channels.insert(ch);
        ports.push_back({b.name, ch, port, b.output,
                         b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U, *d});
    }
    if (!unknown.empty()) {
        std::string list;
        for (const auto& u : unknown)
            list += (list.empty() ? "" : ", ") + u;
        return unsupportedGt("ports whose clock domain is not modelled: " + list);
    }
    auto find = [&](int ch, const std::string& port) -> const P* {
        for (const auto& p : ports)
            if (p.channel == ch && p.port == port)
                return &p;
        return nullptr;
    };

    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << info.instance << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].out ? "output wire " : "input  wire ") << vec(ports[i].width) << " " << ports[i].name
          << (i + 1 < ports.size() ? ",\n" : "\n");
    v << ");\n";

    // domain registers: one per (domain, channel)
    std::string missing;
    auto domain = [&](D d, int ch, const P* clk, const std::string& tag) {
        bool any = false;
        std::string inputs;
        for (const auto& p : ports)
            if (p.d == d && (p.channel == ch || (d == D::Sys && ch == -1))) {
                any = true;
                if (!p.out)
                    inputs += (inputs.empty() ? "" : ", ") + p.name;
            }
        if (!any)
            return;
        if (!clk) {
            missing += (missing.empty() ? "" : ", ") + tag;
            return;
        }
        const std::string c = clk->name + "[0]";
        v << "  reg oc_" << tag << " = 1'b0;\n  always @(posedge " << c << ") oc_" << tag << " <= "
          << (inputs.empty() ? std::string("~oc_" + tag) : "^{" + inputs + "}") << ";\n";
        for (const auto& p : ports)
            if (p.out && p.d == d && (p.channel == ch || (d == D::Sys && ch == -1)))
                v << "  reg " << vec(p.width) << " oc_" << p.name << "_q = " << p.width << "'b0;\n  always @(posedge "
                  << c << ") oc_" << p.name << "_q <= {" << p.width << "{oc_" << tag << "}};\n  assign " << p.name
                  << " = oc_" << p.name << "_q;\n";
    };
    // the reset state machines run on sysclk_in: their outputs of every channel
    {
        const P* sysclk = find(-1, "sysclk_in");
        bool any = false;
        for (auto& p : ports)
            if (p.d == D::Sys) {
                any = true;
                p.channel = -1; // one domain
            }
        if (any)
            domain(D::Sys, -1, sysclk, "sysclk_in");
    }
    for (int ch : channels) {
        const std::string n = std::to_string(ch);
        domain(D::TxUser, ch, find(ch, "txusrclk2_in"), "gt" + n + "_txusrclk2_in");
        domain(D::RxUser, ch, find(ch, "rxusrclk2_in"), "gt" + n + "_rxusrclk2_in");
        domain(D::Drp, ch, find(ch, "drpclk_in"), "gt" + n + "_drpclk_in");
    }
    if (!missing.empty())
        return unsupportedGt("ports of a clock domain without their clock port: " + missing);

    std::string summary;
    for (const auto& p : ports) {
        if (!p.out)
            continue;
        if (p.d == D::Async) {
            v << "  wire oc_" << p.name << "_a;\n  opencdc_async_source u_oc_" << p.name << " (oc_" << p.name
              << "_a);\n  assign " << p.name << " = {" << p.width << "{oc_" << p.name << "_a}};\n";
        }
        else if (p.d == D::Ignored) {
            v << "  assign " << p.name << " = " << p.width << "'b0;\n"; // serial pins
        }
        else if (p.d == D::ClockOut) {
            // TXOUTCLK/RXOUTCLK (PMA): line rate / internal data width; the fabric
            // outputs: the reference clock; TXOUTCLKPCS: no configured frequency
            const std::string g = "gt" + std::to_string(p.channel) + "_val_";
            const std::string dir = p.port.starts_with("tx") ? "tx" : "rx";
            double mhz = 0;
            if (p.port == dir + "outclk_out") {
                const std::string src = lower(text(m, g + dir + "outclk_source", "AUTO"));
                const double rate = real(g + dir + "_line_rate");
                const double width = real(g + dir + "_int_datawidth");
                if ((src == "auto" || src == dir + "outclkpma") && rate > 0 && width > 0)
                    mhz = rate * 1000.0 / width;
            }
            else if (p.port == dir + "outclkfabric_out") {
                mhz = real(g + dir + "_reference_clock");
            }
            v << "  (* OPENCDC_CLOCK_PIN = \"" << p.name << "\"";
            if (mhz > 0)
                v << ", OPENCDC_CLOCK_PERIOD = " << fmtReal(1000.0 / mhz);
            v << " *) wire oc_" << p.name << "_c;\n  opencdc_async_source u_oc_" << p.name << " (oc_" << p.name
              << "_c);\n  assign " << p.name << " = {" << p.width << "{oc_" << p.name << "_c}};\n";
            if (mhz > 0 && p.port == dir + "outclk_out")
                summary += ", " + p.name + " " + fmtReal(mhz) + " MHz";
        }
    }
    v << "endmodule\n";

    XciModel model;
    model.module = info.instance;
    model.ip = vlnv;
    const std::string type = text(m, "gt_type", text(info.user, "gt_type", "GT"));
    std::string rate = text(m, "gt" + std::to_string(channels.empty() ? 0 : *channels.begin()) + "_val_tx_line_rate", "?");
    model.summary = std::to_string(channels.size()) + " channel(s), 7-series " + type + ", line rate " + rate +
                    " Gb/s" + summary;
    model.verilog = v.str();
    model.notes.push_back("the transceiver output clocks are clocks of their own, the user clocks are the clocks the "
                          "design connects (relate them with set_clock_groups or create_clock as Vivado does); the "
                          "reset state machines' outputs are in sysclk_in; PMA status and eye scan outputs are "
                          "asynchronous");
    return model;
}

/// The clock domain of a port, for IP whose ports are classified one by one.
struct PortClass {
    enum class Kind { Sync, Async, Ignored, ClockIn, ClockOut, Through };
    Kind kind = Kind::Ignored;
    std::string clock; ///< Sync: the clock (an input or output clock port, or an internal clock's name)
    double mhz = 0;    ///< ClockOut: the frequency, 0 when not configured
    std::string source; ///< ClockOut: derived from this clock (an input or output clock port, or an internal clock);
                        ///< Through: the input port the output passes on
    double ratio = 0;   ///< ClockOut with a source: output period / source period
};

/// A clock inside the IP that is not a port (a transceiver's recovered clock).
struct InternalClock {
    std::string name;
    double mhz = 0;
};

using PortClassifier = std::function<std::optional<PortClass>(const std::string& lowerName, bool output)>;

/// Model of an IP from the ports of the instance and a classification of
/// every port: one register per clock domain mixing the domain's inputs,
/// outputs as registers of their clock, output clocks as clocks of their own,
/// asynchronous outputs from the built-in black box, ignored inputs unread.
Result<XciModel> portDomainModel(const XciInfo& info, const std::string& vlnv, const PortClassifier& classify,
                                 const std::vector<InternalClock>& internals,
                                 const std::map<std::string, std::string>& aliases = {}) {
    const auto unsupportedIp = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    if (info.ports.empty())
        return unsupportedIp("the model needs the ports of the instance: give a JSON .xci, the component file next "
                             "to the XML .xci (" + info.instance + ".xml), or a VHDL component declaration of the IP "
                             "in the sources");
    struct P {
        std::string name;
        bool out;
        unsigned width;
        PortClass c;
        bool inout = false;
    };
    std::vector<P> ports;
    std::string unknown;
    for (const auto& b : info.ports) {
        // an inout (a device pin through the IP) can only be ignored
        auto c = classify(lower(b.name), b.output || b.inout);
        if (c && b.inout && c->kind != PortClass::Kind::Ignored)
            c.reset();
        if (!c) {
            unknown += (unknown.empty() ? "" : ", ") + b.name;
            continue;
        }
        ports.push_back({b.name, b.output,
                         b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U, *c,
                         b.inout});
    }
    if (!unknown.empty())
        return unsupportedIp("ports whose clock domain is not modelled: " + unknown);

    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << info.instance << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].inout ? "inout  wire " : ports[i].out ? "output wire " : "input  wire ")
          << vec(ports[i].width) << " " << ports[i].name << (i + 1 < ports.size() ? ",\n" : "\n");
    v << ");\n";
    auto clockSource = [&](const std::string& pin, double mhz, const std::string& wire) {
        v << "  (* OPENCDC_CLOCK_PIN = \"" << pin << "\"";
        if (const auto a = aliases.find(lower(pin)); a != aliases.end())
            v << ", OPENCDC_CLOCK_ALIAS = \"" << a->second << "\"";
        if (mhz > 0)
            v << ", OPENCDC_CLOCK_PERIOD = " << fmtReal(1000.0 / mhz);
        v << " *) wire " << wire << ";\n  opencdc_async_source u_" << wire << " (" << wire << ");\n";
    };
    // output clocks and the internal clocks in use
    std::map<std::string, std::string> clockNet; // lower-case clock name -> net
    for (const auto& p : ports)
        if (p.out && p.c.kind == PortClass::Kind::ClockOut && p.c.source.empty()) {
            clockSource(p.name, p.c.mhz, "oc_" + p.name + "_c");
            v << "  assign " << p.name << " = {" << p.width << "{oc_" << p.name << "_c}};\n";
            clockNet[lower(p.name)] = "oc_" + p.name + "_c";
        }
        else if (!p.out && p.c.kind == PortClass::Kind::ClockIn) {
            clockNet[lower(p.name)] = p.name + "[0]";
        }
    std::set<std::string> domains;
    for (const auto& p : ports)
        if (p.c.kind == PortClass::Kind::Sync)
            domains.insert(lower(p.c.clock));
    std::set<std::string> sources; // the clocks derived clocks come from
    for (const auto& p : ports)
        if (p.out && p.c.kind == PortClass::Kind::ClockOut && !p.c.source.empty())
            sources.insert(lower(p.c.source));
    for (const auto& ic : internals)
        if ((domains.contains(lower(ic.name)) || sources.contains(lower(ic.name))) &&
            !clockNet.contains(lower(ic.name))) {
            clockSource(ic.name, ic.mhz, "oc_" + ic.name + "_ic");
            clockNet[lower(ic.name)] = "oc_" + ic.name + "_ic";
        }
    // derived output clocks: a divider register on the source clock (an MMCM's
    // outputs, related to its input), whose period the clock analysis computes
    std::string noSource;
    for (const auto& p : ports) {
        if (!p.out || p.c.kind != PortClass::Kind::ClockOut || p.c.source.empty())
            continue;
        const auto it = clockNet.find(lower(p.c.source));
        if (it == clockNet.end()) {
            noSource += (noSource.empty() ? "" : ", ") + p.name + " (from " + p.c.source + ")";
            continue;
        }
        const std::string w = "oc_" + p.name + "_d";
        v << "  (* OPENCDC_CLOCK_PIN = \"" << p.name << "\", OPENCDC_CLOCK_RATIO = " << fmtReal(p.c.ratio);
        if (p.c.mhz > 0)
            v << ", OPENCDC_CLOCK_INPUT_PERIOD = " << fmtReal(1000.0 / (p.c.mhz * p.c.ratio));
        v << " *) reg " << w << " = 1'b0;\n  always @(posedge " << it->second << ") " << w << " <= ~" << w
          << ";\n  assign " << p.name << " = {" << p.width << "{" << w << "}};\n";
        clockNet[lower(p.name)] = w;
    }
    if (!noSource.empty())
        return unsupportedIp("derived clocks without their source clock port: " + noSource);
    std::string missing;
    for (const auto& d : domains) {
        const auto it = clockNet.find(d);
        if (it == clockNet.end()) {
            missing += (missing.empty() ? "" : ", ") + d;
            continue;
        }
        std::string inputs;
        for (const auto& p : ports)
            if (!p.out && p.c.kind == PortClass::Kind::Sync && lower(p.c.clock) == d)
                inputs += (inputs.empty() ? "" : ", ") + p.name;
        const std::string tag = "oc_dom_" + d;
        v << "  reg " << tag << " = 1'b0;\n  always @(posedge " << it->second << ") " << tag << " <= "
          << (inputs.empty() ? "~" + tag : "^{" + inputs + "}") << ";\n";
        for (const auto& p : ports)
            if (p.out && p.c.kind == PortClass::Kind::Sync && lower(p.c.clock) == d)
                v << "  reg " << vec(p.width) << " oc_" << p.name << "_q = " << p.width << "'b0;\n  always @(posedge "
                  << it->second << ") oc_" << p.name << "_q <= {" << p.width << "{" << tag << "}};\n  assign "
                  << p.name << " = oc_" << p.name << "_q;\n";
    }
    if (!missing.empty())
        return unsupportedIp("ports of a clock domain without their clock port: " + missing);
    for (const auto& p : ports) {
        if (!p.out)
            continue;
        if (p.c.kind == PortClass::Kind::Async)
            v << "  wire oc_" << p.name << "_a;\n  opencdc_async_source u_oc_" << p.name << " (oc_" << p.name
              << "_a);\n  assign " << p.name << " = {" << p.width << "{oc_" << p.name << "_a}};\n";
        else if (p.c.kind == PortClass::Kind::Ignored || p.c.kind == PortClass::Kind::ClockIn)
            v << "  assign " << p.name << " = " << p.width << "'b0;\n";
        else if (p.c.kind == PortClass::Kind::Through)
            v << "  assign " << p.name << " = " << p.c.source << ";\n";
    }
    v << "endmodule\n";
    XciModel model;
    model.module = info.instance;
    model.ip = vlnv;
    model.verilog = v.str();
    return model;
}

/// 10G/25G Ethernet PCS/PMA (ten_gig_eth_pcs_pma, PG068), the core without
/// shared logic: XGMII, configuration and status in coreclk; the DRP
/// arbitration in dclk; transceiver debug ports in the TX user clock or the
/// receiver's recovered clock; TXOUTCLK and RXRECCLK as clocks of their own.
Result<XciModel> tenGigEthPcsPma(const XciInfo& info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const bool g25 = text(m, "c_speed10_25", "10") == "25";
    const double rate = g25 ? 25.78125 : 10.3125;
    // TXOUTCLK / RXRECCLK come from the transceiver's PMA with its 32-bit internal
    // data path at 10G (322.27 MHz, also with the 64-bit fabric interface
    // c_gtif_width, where txusrclk2 runs at half of it); 25G: not configured here
    const double gtMhz = g25 ? 0.0 : rate * 1000.0 / 32.0;
    const bool noEbuff = lower(text(m, "c_no_ebuff", "false")) == "true";
    // PG068: the XGMII, configuration and status are in the "TX clock source": coreclk on
    // 7 series (and UltraScale 10GBASE-KR), txusrclk2 on UltraScale 10GBASE-R, where
    // coreclk only runs the transceiver reset logic; the RX clock source is the same
    // with the elastic buffer, the recovered clock without it
    const std::string family = lower(text(m, "c_family", ""));
    const bool series7 = family.find('7') != std::string::npos || family == "zynq";
    const bool kr = lower(text(m, "c_is_kr", "false")) == "true";
    const std::string txSource = series7 || kr ? "coreclk" : "txusrclk2";
    const std::string rxSource = noEbuff ? "rxrecclk_out" : txSource;
    static const std::set<std::string> core = {"xgmii_txd",  "xgmii_txc",     "configuration_vector", "status_vector",
                                               "core_status", "pma_pmd_type", "tx_disable",           "tx_resetdone",
                                               "rx_resetdone"};
    static const std::set<std::string> txGt = {"txbufstatus", "txresetdone", "txprbsforceerr", "txpolarity"};
    static const std::set<std::string> rxGt = {"rxbufstatus", "rxresetdone", "rxprbserr", "rxpolarity"};
    static const std::set<std::string> asyncGt = {"eyescandataerror", "dmonitorout", "rxpmaresetdone"};
    static const std::set<std::string> ignoredGt = {"eyescanreset", "eyescantrigger", "rxcdrhold",   "rxrate",
                                                    "txoutclksel",  "txpcsreset",     "txpmareset",  "rxpmareset",
                                                    "rxdfelpmreset", "txprecursor",   "txpostcursor", "txdiffctrl",
                                                    "rxlpmen",      "pcsrsvdin"};
    static const std::set<std::string> ignored = {
        "areset",       "areset_coreclk", "gttxreset",  "gtrxreset",      "sim_speedup_control", "txuserrdy",
        "qplllock",     "qplloutclk",     "qplloutrefclk", "qpll0lock",   "qpll0outclk",         "qpll0outrefclk",
        "reset_counter_done", "signal_detect", "tx_fault", "rxp",         "rxn",                 "txp",
        "txn"};
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "coreclk" || n == "dclk" || n == "txusrclk" || n == "txusrclk2")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "txoutclk" || n == "rxrecclk_out")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, gtMhz, {}, 0}) : std::nullopt;
        if (n == "xgmii_rxd" || n == "xgmii_rxc")
            return PortClass{K::Sync, rxSource, 0, {}, 0};
        if (core.contains(n))
            return PortClass{K::Sync, txSource, 0, {}, 0};
        if (n.starts_with("drp_") || n.starts_with("core_to_gt_drp") || n.starts_with("gt_drp"))
            return PortClass{K::Sync, "dclk", 0, {}, 0};
        if (n == "qpll0reset" || n == "reset_tx_bufg_gt")
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
        if (ignored.contains(n))
            return PortClass{K::Ignored, {}, 0, {}, 0};
        std::string g;
        if (n.starts_with("gt0_"))
            g = n.substr(4);
        else if (n.starts_with("gt_"))
            g = n.substr(3);
        if (g.empty())
            return std::nullopt;
        if (txGt.contains(g))
            return PortClass{K::Sync, "txusrclk2", 0, {}, 0};
        if (rxGt.contains(g))
            return PortClass{K::Sync, "rxrecclk_out", 0, {}, 0};
        if (asyncGt.contains(g))
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
        if (ignoredGt.contains(g))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {{"rxrecclk_out", gtMhz}});
    if (!r)
        return r;
    r.value().summary = std::string(g25 ? "25" : "10") + "GBASE-R PCS/PMA, line rate " + fmtReal(rate) + " Gb/s" +
                        (gtMhz > 0 ? ", TXOUTCLK/RXRECCLK " + fmtReal(gtMhz) + " MHz" : std::string()) + ", " +
                        text(m, "c_gtif_width", "?") + "-bit transceiver interface" +
                        ", XGMII in " + txSource +
                        (noEbuff ? ", no elastic buffer (XGMII RX in the recovered clock)" : "");
    r.value().notes.push_back("XGMII, configuration and status are in the TX clock source (coreclk on 7 series, "
                              "txusrclk2 on UltraScale 10GBASE-R, PG068); the DRP in dclk; transceiver debug "
                              "ports in txusrclk2 or the recovered clock; TXOUTCLK and RXRECCLK are clocks of their own; "
                              "signal_detect, tx_fault and the resets are synchronized in the IP");
    return r;
}

/// 10G/25G High Speed Ethernet subsystem (xxv_ethernet, PG210) as an
/// Ethernet PCS/PMA 64-bit core with the MII data path, one core: TX MII and
/// TX control/status in tx_mii_clk (an output, the transmitter's clock); RX
/// MII in rx_core_clk (asynchronous clocking: the RX FIFO crosses into it);
/// RX control/status in rx_clk_out (the receiver's clock); the transceiver
/// reset helper's outputs in dclk; the DRP in gt_drpclk.
Result<XciModel> xxvEthernet(const XciInfo& info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const std::string coreKind = text(m, "C_CORE", "");
    const auto unsupportedIp = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    if (coreKind.find("PCS/PMA") == std::string::npos || coreKind.find("MAC") != std::string::npos ||
        lower(text(m, "C_DATA_PATH_INTERFACE", "")) != "mii")
        return unsupportedIp("only the Ethernet PCS/PMA 64-bit core with the MII data path is modelled (C_CORE '" +
                             coreKind + "', C_DATA_PATH_INTERFACE '" + text(m, "C_DATA_PATH_INTERFACE", "?") + "')");
    if (num(m, "C_NUM_OF_CORES", 1) != 1)
        return unsupportedIp("only one core is modelled (C_NUM_OF_CORES " + text(m, "C_NUM_OF_CORES", "?") + ")");
    double rate = 0;
    try {
        rate = std::stod(text(m, "C_LINE_RATE", "0"));
    }
    catch (...) {
    }
    const double lineGbps = rate >= 25 ? 25.78125 : rate >= 10 ? 10.3125 : 0;
    const double miiMhz = lineGbps * 1000.0 / 66.0; // 64b/66b, 64-bit MII per clock
    using K = PortClass::Kind;
    static const std::set<std::string> txGt = {"txbufstatus", "txprbssel", "txprbsforceerr", "txpolarity"};
    static const std::set<std::string> rxGt = {"rxbufstatus", "rxprbserr",  "rxstartofseq", "rxprbssel",
                                               "rxprbscntreset", "rxpolarity", "rxslide_in",  "rxcommadeten"};
    static const std::set<std::string> asyncGt = {"dmonitorout", "eyescandataerror"};
    static const std::set<std::string> ignoredGt = {
        "eyescanreset", "eyescantrigger", "pcsrsvdin",  "rxbufreset",  "rxcdrhold",   "rxdfeagchold", "rxdfelpmreset",
        "rxlatclk",     "rxlpmen",        "rxpcsreset", "rxpmareset",  "rxrate",      "txdiffctrl",   "txinhibit",
        "txlatclk",     "txmaincursor",   "txpcsreset", "txpmareset",  "txpostcursor", "txprecursor", "loopback_in",
        "txp_out",      "txn_out",        "rxp_in",     "rxn_in",      "reset_all_in", "tx_reset_in", "rx_reset_in"};
    static const std::set<std::string> ignored = {"rx_serdes_reset", "txoutclksel_in", "rxoutclksel_in",
                                                  "sys_reset",       "qpll0clk_in",    "qpll0refclk_in",
                                                  "qpll1clk_in",     "qpll1refclk_in", "gtwiz_reset_qpll0lock_in",
                                                  "gtwiz_reset_qpll1lock_in", "rx_reset", "tx_reset"};
    PortClassifier classify = [&](const std::string& full, bool out) -> std::optional<PortClass> {
        // per-core ports end with _0
        const std::string n = full.ends_with("_0") ? full.substr(0, full.size() - 2) : full;
        if (n == "rx_core_clk" || n == "dclk" || n == "gt_drpclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "tx_mii_clk" || n == "rx_clk_out")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, miiMhz, {}, 0}) : std::nullopt;
        if (n == "rxrecclkout")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, 0, {}, 0}) : std::nullopt;
        const std::string tx = full.ends_with("_0") ? "tx_mii_clk_0" : "tx_mii_clk";
        const std::string rxs = full.ends_with("_0") ? "rx_clk_out_0" : "rx_clk_out";
        const std::string rxc = full.ends_with("_0") ? "rx_core_clk_0" : "rx_core_clk";
        if (n == "tx_mii_d" || n == "tx_mii_c" || n.starts_with("ctl_tx_") || n.starts_with("stat_tx_"))
            return PortClass{K::Sync, tx, 0, {}, 0};
        if (n == "rx_mii_d" || n == "rx_mii_c")
            return PortClass{K::Sync, rxc, 0, {}, 0};
        if (n.starts_with("ctl_rx_") || n.starts_with("stat_rx_"))
            return PortClass{K::Sync, rxs, 0, {}, 0};
        if (n == "gt_reset_tx_done_out" || n == "gt_reset_rx_done_out" || n == "gtwiz_reset_qpll0reset_out" ||
            n == "gtwiz_reset_qpll1reset_out")
            return out ? std::optional<PortClass>(PortClass{K::Sync, "dclk", 0, {}, 0}) : std::nullopt;
        if (n == "gtpowergood_out")
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
        if (n.starts_with("gt_drp"))
            return PortClass{K::Sync, full.ends_with("_0") ? "gt_drpclk_0" : "gt_drpclk", 0, {}, 0};
        if (ignored.contains(n))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        if (!n.starts_with("gt_"))
            return std::nullopt;
        const std::string g = n.substr(3);
        if (txGt.contains(g))
            return PortClass{K::Sync, tx, 0, {}, 0};
        if (rxGt.contains(g))
            return PortClass{K::Sync, rxs, 0, {}, 0};
        if (asyncGt.contains(g))
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
        if (ignoredGt.contains(g))
            return PortClass{K::Ignored, {}, 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    r.value().summary = "Ethernet PCS/PMA 64-bit, MII, " + text(m, "C_LINE_RATE", "?") + "G" +
                        (miiMhz > 0 ? ", tx_mii_clk/rx_clk_out " + fmtReal(miiMhz) + " MHz" : std::string()) + ", " +
                        text(m, "C_CLOCKING", "?") + " clocking";
    r.value().notes.push_back("TX MII and TX control/status are in tx_mii_clk, RX MII in rx_core_clk, RX control/status "
                              "in rx_clk_out (PG210); tx_mii_clk and rx_clk_out are clocks of their own; the "
                              "transceiver reset helper's outputs are in dclk");
    return r;
}

/// 1G/2.5G Ethernet PCS/PMA or SGMII (gig_ethernet_pcs_pma, PG047) with a
/// device transceiver and without shared logic, or as synchronous SGMII over
/// UltraScale LVDS with shared logic: GMII, configuration and status in the
/// GMII clock (userclk2 or clk125_out); transceiver debug ports in the
/// transceiver's user clocks; the DRP in gt0_drpclk_in; TXOUTCLK/RXOUTCLK as
/// clocks of their own; the LVDS clocks derived from refclk625 by the IP's MMCM.
Result<XciModel> gigEthernetPcsPma(const XciInfo& info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const auto flag = [&](const std::string& k) { return lower(text(m, k, "false")) == "true"; };
    const auto unsupportedIp = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    const bool lvds = flag("c_use_lvds");
    const bool gt = flag("c_use_transceiver");
    const bool shared = flag("c_support_level");
    const bool g25 = flag("c_is_2_5g");
    const std::string family = lower(text(m, "c_family", ""));
    const bool series7 = family.find('7') != std::string::npos || family == "zynq";
    if (flag("c_use_tbi") || (!lvds && !gt))
        return unsupportedIp("only the device transceiver and the UltraScale LVDS variants are modelled, not the "
                             "ten-bit interface (TBI) to an external SerDes");
    if (flag("c_enable_async_sgmii") || flag("c_enable_async_lvds") || flag("c_enable_async_lvds_rx_only"))
        return unsupportedIp("asynchronous SGMII over LVDS is not modelled");
    if (gt && shared)
        return unsupportedIp("the transceiver variant with the shared logic in the core (c_support_level true) is not "
                             "modelled: its user clocks come from an MMCM inside the IP; generate the core without "
                             "the shared logic");
    if (lvds && (series7 || !shared))
        return unsupportedIp("SGMII over LVDS is modelled for UltraScale with the shared logic in the core only "
                             "(c_family '" + family + "', c_support_level " + text(m, "c_support_level", "?") + ")");
    if (num(m, "c_num_of_lanes", 1) != 1)
        return unsupportedIp("only one lane is modelled (c_num_of_lanes " + text(m, "c_num_of_lanes", "?") + ")");
    // the transceiver runs a 16-bit (2-byte) internal data path, 8b/10b on the line:
    // TXOUTCLK = line rate / 20 = 62.5 MHz at 1.25 Gb/s, 156.25 MHz at 3.125 Gb/s
    const double lineGbps = g25 ? 3.125 : 1.25;
    const double gtMhz = lineGbps * 1000.0 / 20.0;
    const bool rxRecovered = lower(text(m, "c_rx_gmii_clk_src", "TXOUTCLK")) == "rxoutclk";
    const std::string gmii = lvds ? "clk125_out" : "userclk2";
    const std::string gmiiRx = lvds || !rxRecovered ? gmii : "rxuserclk2";
    std::set<std::string> have;
    for (const auto& b : info.ports)
        have.insert(lower(b.name));
    // the transceiver's TXUSRCLK2: userclk (2-byte interface)
    const std::string txUser = have.contains("userclk") ? "userclk" : gmii;
    const std::string rxUser = have.contains("rxuserclk2") ? "rxuserclk2" : txUser;
    static const std::set<std::string> gmiiPorts = {
        "gmii_txd",     "gmii_tx_en",           "gmii_tx_er",   "gmii_isolate",        "configuration_vector",
        "configuration_valid", "status_vector", "an_interrupt", "an_adv_config_vector", "an_adv_config_val",
        "an_restart_config",   "speed_is_10_100", "speed_is_100", "basex_or_sgmii",    "link_timer_value",
        "link_timer_basex",    "link_timer_sgmii", "sgmii_clk_r", "sgmii_clk_f",       "sgmii_clk_en",
        "rst_125_out"};
    static const std::set<std::string> txGt = {"txpolarity", "txprbssel", "txprbsforceerr", "txbufstatus",
                                               "txresetdone"};
    static const std::set<std::string> rxGt = {
        "rxpolarity",      "rxprbssel",      "rxprbscntreset", "rxprbserr",  "rxbufstatus", "rxchariscomma",
        "rxcharisk",       "rxbyteisaligned", "rxbyterealign", "rxcommadet", "rxdisperr",   "rxnotintable",
        "rxresetdone"};
    static const std::set<std::string> asyncGt = {"dmonitorout",   "eyescandataerror", "rxpmaresetdone",
                                                  "txpmaresetdone", "rxmonitorout",     "cplllock",
                                                  "pll0reset",     "pll1reset",        "qpll0reset"};
    static const std::set<std::string> ignoredGt = {
        "txdiffctrl",   "txinhibit",      "txpostcursor", "txprecursor",  "txpmareset",    "txpcsreset",
        "rxpmareset",   "rxpcsreset",     "rxbufreset",   "rxcdrhold",    "rxlpmhfhold",   "rxlpmlfhold",
        "rxlpmreset",   "rxlpmhfoverden", "rxdfelpmreset", "rxlpmen",     "rxmonitorsel",  "eyescanreset",
        "eyescantrigger", "loopback",     "pll0outclk",   "pll0outrefclk", "pll1outclk",   "pll1outrefclk",
        "pll0refclklost", "pll0lock",     "pll1lock",     "qplloutclk",   "qplloutrefclk", "qplllock",
        "qpll0outclk",  "qpll0outrefclk", "qpll0lock"};
    static const std::set<std::string> ignored = {"reset", "pma_reset", "mmcm_locked", "signal_detect",
                                                  "txp",   "txn",       "rxp",         "rxn",
                                                  "refclk625_n", "gtrefclk_n"};
    static const std::set<std::string> asyncOut = {"resetdone", "cplllock", "mmcm_reset", "mmcm_locked_out",
                                                   "idelay_rdy_out", "gtpowergood"};
    using K = PortClass::Kind;
    const auto inClock = [](bool out) {
        return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
    };
    const auto sync = [](const std::string& c) { return std::optional<PortClass>(PortClass{K::Sync, c, 0, {}, 0}); };
    const auto async = [](bool out) {
        return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
    };
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (!lvds && (n == "gtrefclk" || n == "gtrefclk_bufg" || n == "independent_clock_bufg" || n == "userclk" ||
                      n == "userclk2" || n == "rxuserclk" || n == "rxuserclk2" || n == "gt0_drpclk_in"))
            return inClock(out);
        if (!lvds && (n == "txoutclk" || n == "rxoutclk"))
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, gtMhz, {}, 0}) : std::nullopt;
        if (lvds && n == "refclk625_p")
            return inClock(out);
        // UltraScale LVDS: the MMCM makes 625, 312.5 and 125 MHz from the 625 MHz reference
        if (lvds && (n == "clk625_out" || n == "clk312_out" || n == "clk125_out")) {
            const double mhz = n == "clk625_out" ? 625.0 : n == "clk312_out" ? 312.5 : 125.0;
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, mhz, "refclk625_p", 625.0 / mhz})
                       : std::nullopt;
        }
        if (n == "gmii_rxd" || n == "gmii_rx_dv" || n == "gmii_rx_er")
            return sync(gmiiRx);
        if (gmiiPorts.contains(n))
            return sync(gmii);
        if (asyncOut.contains(n))
            return async(out);
        if (ignored.contains(n))
            return out && n != "txp" && n != "txn" ? std::nullopt
                                                  : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        if (lvds || !n.starts_with("gt0_"))
            return std::nullopt;
        std::string g = n.substr(4);
        if (g.ends_with("_in") || g.ends_with("_out"))
            g = g.substr(0, g.rfind('_'));
        if (g.starts_with("drp"))
            return sync("gt0_drpclk_in");
        if (txGt.contains(g))
            return sync(txUser);
        if (rxGt.contains(g))
            return sync(rxUser);
        if (asyncGt.contains(g))
            return async(out);
        if (ignoredGt.contains(g))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    r.value().summary =
        std::string(flag("c_is_sgmii") ? "SGMII" : g25 ? "2500BASE-X" : "1000BASE-X") +
        (lvds ? " over UltraScale LVDS, clk125_out/clk312_out/clk625_out from refclk625"
              : ", " + text(m, "c_transceiver_type", "transceiver") + ", line rate " + fmtReal(lineGbps) +
                    " Gb/s, TXOUTCLK/RXOUTCLK " + fmtReal(gtMhz) + " MHz") +
        ", GMII in " + gmii + (gmiiRx != gmii ? " (RX in " + gmiiRx + ")" : "");
    r.value().notes.push_back(
        lvds ? "GMII, configuration and status are in clk125_out (PG047); clk125_out, clk312_out and clk625_out are "
               "derived from refclk625 by the IP's MMCM; reset and signal_detect are synchronized in the IP"
             : "GMII, configuration and status are in userclk2 (PG047), GMII RX in rxuserclk2 when RXOUTCLK is its "
               "clock source; transceiver TX/RX debug ports in userclk / rxuserclk2, the DRP in gt0_drpclk_in; "
               "TXOUTCLK and RXOUTCLK are clocks of their own; reset, pma_reset and signal_detect are synchronized "
               "in the IP");
    return r;
}

/// XAUI (xaui, PG053) with the shared logic in the core (UltraScale): the IP's
/// clk156_out (lane 0's TXOUTCLK, line rate / 20) is the user clock of all four
/// transceivers (the RX elastic buffers cross into it); XGMII, configuration,
/// status, debug and the transceiver status/control in clk156_out; the DRP in dclk.
Result<XciModel> xaui(const XciInfo& info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const auto unsupportedIp = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    bool clk156 = false;
    for (const auto& b : info.ports)
        clk156 = clk156 || (b.output && lower(b.name) == "clk156_out");
    if (!info.ports.empty() && !clk156)
        return unsupportedIp("only the core with the shared logic (the output clock clk156_out) is modelled; without "
                             "it the user clock comes from the design");
    // DXAUI: 6.25 Gb/s per lane; XAUI 3.125 Gb/s; 8b/10b, 2-byte transceiver interface
    const bool dxaui = lower(text(m, "c_is_dxaui", "false")) == "true";
    const double laneGbps = dxaui ? 6.25 : 3.125;
    const double mhz = laneGbps * 1000.0 / 20.0;
    static const std::set<std::string> syncGt = {
        "txresetdone", "rxresetdone", "rxbufstatus", "rxprbserr",   "rxdisperr",       "rxnotintable",
        "rxcommadet",  "rxpolarity",  "txpolarity",  "rxprbssel",   "txprbssel",       "txprbsforceerr",
        "rxprbscntreset"};
    static const std::set<std::string> asyncGt = {"rxpmaresetdone", "txphaligndone",    "txphinitdone",
                                                  "txdlysresetdone", "qplllock",        "eyescandataerror",
                                                  "dmonitorout",    "powergood_out"};
    static const std::set<std::string> ignoredGt = {
        "reset_tx_datapath", "reset_tx_pll_and_datapath", "reset_rx_datapath", "reset_rx_pll_and_datapath",
        "txpmareset",        "txpcsreset",                "rxpmareset",        "rxpcsreset",
        "eyescantrigger",    "eyescanreset",              "rxrate",            "loopback",
        "rxlpmen",           "rxdfelpmreset",             "txpostcursor",      "txprecursor",
        "txdiffctrl",        "txinhibit",                 "rxcdrhold",         "pcsrsvdin"};
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "refclk" || n == "dclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "clk156_out")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, mhz, {}, 0}) : std::nullopt;
        if (n == "xgmii_txd" || n == "xgmii_txc" || n == "xgmii_rxd" || n == "xgmii_rxc" ||
            n == "configuration_vector" || n == "status_vector" || n == "debug")
            return PortClass{K::Sync, "clk156_out", 0, {}, 0};
        if (n == "clk156_lock")
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
        if (n == "reset" || n == "signal_detect" || n.starts_with("xaui_rx_l"))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        if (n.starts_with("xaui_tx_l"))
            return out ? std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0}) : std::nullopt;
        // gt<N>_drp*: the DRP of lane N, in dclk
        if (n.size() > 3 && n.starts_with("gt") && std::isdigit(static_cast<unsigned char>(n[2])) &&
            n.substr(3).starts_with("_drp"))
            return PortClass{K::Sync, "dclk", 0, {}, 0};
        if (!n.starts_with("gt_"))
            return std::nullopt;
        const std::string g = n.substr(3);
        if (syncGt.contains(g))
            return PortClass{K::Sync, "clk156_out", 0, {}, 0};
        if (asyncGt.contains(g))
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
        if (ignoredGt.contains(g))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    r.value().summary = std::string(dxaui ? "DXAUI" : "XAUI") + ", 4 lanes at " + fmtReal(laneGbps) +
                        " Gb/s, clk156_out " + fmtReal(mhz) + " MHz, XGMII in clk156_out";
    r.value().notes.push_back("clk156_out is a clock of its own (lane 0's TXOUTCLK) and the user clock of all lanes "
                              "(PG053); XGMII, configuration, status, debug and the transceiver status/control are "
                              "in clk156_out; the DRP in dclk; reset and signal_detect are synchronized in the IP");
    return r;
}

/// 100G Ethernet (cmac_usplus, PG203) as CAUI-4 with the AXI4-Stream user
/// interface: TX AXIS, TX control/status and usr_tx_reset in gt_txusrclk2 (an
/// output, the transmitter's user clock); RX AXIS, RX control/status and
/// usr_rx_reset in rx_clk (an input: gt_txusrclk2 in synchronous mode, any clock
/// with the RX FIFO in asynchronous mode); RX transceiver debug ports in
/// gt_rxusrclk2; the core DRP in drp_clk, the transceiver DRPs in gt_drpclk;
/// gt_ref_clk_out is the reference clock passed through the IP.
Result<XciModel> cmacUsplus(const XciInfo& info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const auto unsupportedIp = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    if (lower(text(m, "C_USER_INTERFACE", "AXIS")) != "axis")
        return unsupportedIp("only the AXI4-Stream user interface is modelled (C_USER_INTERFACE '" +
                             text(m, "C_USER_INTERFACE", "?") + "')");
    if (text(m, "C_CMAC_CAUI4_MODE", "1") != "1" || num(m, "C_NUM_LANES", 4) != 4)
        return unsupportedIp("only CAUI-4 (4 lanes) is modelled (C_CMAC_CAUI4_MODE " + text(m, "C_CMAC_CAUI4_MODE", "?") +
                             ", C_NUM_LANES " + text(m, "C_NUM_LANES", "?") + ")");
    double rate = 25.78125, ref = 0;
    try {
        rate = std::stod(text(m, "C_LINE_RATE", "25.78125"));
        ref = std::stod(text(m, "C_GT_REF_CLK_FREQ", "0"));
    }
    catch (...) {
    }
    // the GTY's 80-bit internal data path at 25.78125 Gb/s: 322.265625 MHz
    const double usrMhz = rate * 1000.0 / 80.0;
    static const std::set<std::string> txGt = {"txbufstatus", "txresetdone", "txprbssel", "txprbsforceerr",
                                               "txpolarity"};
    static const std::set<std::string> rxGt = {"rxbufstatus", "rxresetdone", "rxprbssel", "rxprbscntreset",
                                               "rxprbserr",   "rxpolarity"};
    static const std::set<std::string> asyncGt = {"eyescandataerror", "powergoodout"};
    static const std::set<std::string> ignoredGt = {"loopback_in", "eyescanreset", "eyescantrigger", "rxcdrhold",
                                                    "rxrate",      "txdiffctrl",   "txinhibit",      "txpippmen",
                                                    "txpippmsel",  "txpostcursor", "txprecursor",    "rxdfelpmreset",
                                                    "rxlpmen",     "rxp_in",       "rxn_in"};
    static const std::set<std::string> ignored = {"sys_reset",     "core_rx_reset", "core_tx_reset", "core_drp_reset",
                                                  "gt_ref_clk_n",  "gtwiz_reset_tx_datapath",
                                                  "gtwiz_reset_rx_datapath"};
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "gt_ref_clk_p" || n == "init_clk" || n == "gt_drpclk" || n == "drp_clk" || n == "rx_clk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "gt_txusrclk2" || n == "gt_rxusrclk2")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, usrMhz, {}, 0}) : std::nullopt;
        if (n == "gt_rxrecclkout")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, 0, {}, 0}) : std::nullopt;
        if (n == "gt_ref_clk_out")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, ref, "gt_ref_clk_p", 1.0}) : std::nullopt;
        if (n.starts_with("tx_axis_") || n == "tx_preamblein" || n == "tx_ovfout" || n == "tx_unfout" ||
            n == "usr_tx_reset" || n.starts_with("ctl_tx_") || n.starts_with("stat_tx_"))
            return PortClass{K::Sync, "gt_txusrclk2", 0, {}, 0};
        if (n.starts_with("rx_axis_") || n == "rx_preambleout" || n.starts_with("rx_otn_") || n == "usr_rx_reset" ||
            n.starts_with("ctl_rx_") || n.starts_with("stat_rx_") || n == "ctl_rsfec_ieee_error_indication_mode")
            return PortClass{K::Sync, "rx_clk", 0, {}, 0};
        if (n.starts_with("drp_"))
            return PortClass{K::Sync, "drp_clk", 0, {}, 0};
        if (n.starts_with("common0_drp") ||
            (n.size() > 3 && n.starts_with("gt") && std::isdigit(static_cast<unsigned char>(n[2])) &&
             n.substr(3).starts_with("_drp")))
            return PortClass{K::Sync, "gt_drpclk", 0, {}, 0};
        if (ignored.contains(n))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        if (n == "gt_txp_out" || n == "gt_txn_out")
            return out ? std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0}) : std::nullopt;
        if (!n.starts_with("gt_"))
            return std::nullopt;
        const std::string g = n.substr(3);
        if (txGt.contains(g))
            return PortClass{K::Sync, "gt_txusrclk2", 0, {}, 0};
        if (rxGt.contains(g))
            return PortClass{K::Sync, "gt_rxusrclk2", 0, {}, 0};
        if (asyncGt.contains(g))
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
        if (ignoredGt.contains(g))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    r.value().summary = "100G CAUI-4, " + fmtReal(rate) + " Gb/s lanes, AXI4-Stream, gt_txusrclk2/gt_rxusrclk2 " +
                        fmtReal(usrMhz) + " MHz" +
                        (ref > 0 ? ", gt_ref_clk_out " + fmtReal(ref) + " MHz from gt_ref_clk_p" : std::string()) +
                        ", " + text(m, "C_CLOCKING_MODE", "?") + " clocking";
    r.value().notes.push_back("TX AXI4-Stream, ctl_tx/stat_tx and usr_tx_reset are in gt_txusrclk2, RX AXI4-Stream, "
                              "ctl_rx/stat_rx and usr_rx_reset in rx_clk (PG203); gt_txusrclk2 and gt_rxusrclk2 are "
                              "clocks of their own, gt_ref_clk_out is derived from gt_ref_clk_p; the core DRP in "
                              "drp_clk, the transceiver DRPs in gt_drpclk");
    return r;
}

/// IEEE 802.3 Clause 74 FEC (ieee802d3_clause74_fec, PG303): the encoder in
/// tx_clk (tx_reset, tx_din, tx_dout and their start flags), the decoder in
/// rx_clk (rx_reset, rx_din, rx_din_slip, rx_dout, ctrl_rx_*, stat_rx_*); the
/// two paths are independent, nothing crosses inside the IP.
Result<XciModel> clause74Fec(const XciInfo& info) {
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "tx_clk" || n == "rx_clk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        // PG303: tx_reset is in tx_clk, rx_reset in rx_clk
        if (n.starts_with("tx_"))
            return PortClass{K::Sync, "tx_clk", 0, {}, 0};
        if (n.starts_with("rx_") || n.starts_with("ctrl_rx_") || n.starts_with("stat_rx_") || n.starts_with("stat_cw_"))
            return PortClass{K::Sync, "rx_clk", 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    r.value().summary = "Clause 74 FEC, encoder in tx_clk, decoder in rx_clk";
    r.value().notes.push_back("the encoder (tx_reset, tx_din, tx_dout) is in tx_clk, the decoder (rx_reset, rx_din, "
                              "rx_din_slip, rx_dout, ctrl_rx_*, stat_rx_*) in rx_clk (PG303); the resets are "
                              "synchronous to their clock");
    return r;
}

/// AXI DMA (axi_dma, PG021) in direct register mode: the AXI4-Lite register
/// interface, axi_resetn and the interrupts in s_axi_lite_aclk; the MM2S
/// channel (AXI4 read master, AXI4-Stream master, its reset output) in
/// m_axi_mm2s_aclk, the S2MM channel in m_axi_s2mm_aclk, when the clocks are
/// asynchronous (C_PRMRY_IS_ACLK_ASYNC 1). In synchronous mode PG021 requires
/// one clock for all clock ports: every port is then in s_axi_lite_aclk, and a
/// different clock on m_axi_*_aclk shows as crossings into the IP.
Result<XciModel> axiDma(const XciInfo& info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const auto unsupportedIp = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnv + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    if (num(m, "C_INCLUDE_SG", 0) != 0)
        return unsupportedIp("the scatter gather engine (C_INCLUDE_SG 1) is not modelled, only direct register mode");
    if (num(m, "C_ENABLE_MULTI_CHANNEL", 0) != 0)
        return unsupportedIp("the multichannel DMA (C_ENABLE_MULTI_CHANNEL 1) is not modelled");
    const bool async = num(m, "C_PRMRY_IS_ACLK_ASYNC", 0) != 0;
    const std::string lite = "s_axi_lite_aclk";
    const std::string mm2s = async ? "m_axi_mm2s_aclk" : lite;
    const std::string s2mm = async ? "m_axi_s2mm_aclk" : lite;
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "s_axi_lite_aclk" || n == "m_axi_mm2s_aclk" || n == "m_axi_s2mm_aclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n.starts_with("s_axi_lite_") || n == "axi_resetn" || n == "mm2s_introut" || n == "s2mm_introut")
            return PortClass{K::Sync, lite, 0, {}, 0};
        if (n.starts_with("m_axi_mm2s_") || n.starts_with("m_axis_mm2s_") || n == "mm2s_prmry_reset_out_n")
            return PortClass{K::Sync, mm2s, 0, {}, 0};
        if (n.starts_with("m_axi_s2mm_") || n.starts_with("s_axis_s2mm_") || n == "s2mm_prmry_reset_out_n")
            return PortClass{K::Sync, s2mm, 0, {}, 0};
        // the debug test vector: no documented clock
        if (n == "axi_dma_tstvec")
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0, {}, 0}) : std::nullopt;
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    r.value().summary = std::string("direct register mode, ") +
                        (async ? "asynchronous clocks: MM2S in m_axi_mm2s_aclk, S2MM in m_axi_s2mm_aclk, registers in "
                                 "s_axi_lite_aclk"
                               : "synchronous clocks (C_PRMRY_IS_ACLK_ASYNC 0): every port in s_axi_lite_aclk");
    r.value().notes.push_back(async ? "the AXI4-Lite registers, axi_resetn and the interrupts are in s_axi_lite_aclk, "
                                      "the MM2S channel in m_axi_mm2s_aclk, the S2MM channel in m_axi_s2mm_aclk (PG021); "
                                      "the IP crosses between them"
                                    : "PG021: in synchronous mode all clock ports must be connected to one clock; every "
                                      "port is modelled in s_axi_lite_aclk, so another clock on m_axi_mm2s_aclk or "
                                      "m_axi_s2mm_aclk shows as crossings into the IP");
    return r;
}

/// The ports of an AXI interface of a Xilinx infrastructure IP (the clock
/// converter, the crossbar) from its configuration: AXI4 (C_AXI_PROTOCOL 0),
/// AXI3 (1) or AXI4-Lite (2), `slots` interfaces concatenated in each port.
/// AXI4 has region and QoS, AXI3 QoS and WID (as the Xilinx IP ports are).
std::vector<BoundaryPort> axiInterfacePorts(const std::string& prefix, bool master, long long slots, long long protocol,
                                            long long id, long long addr, long long data, bool user,
                                            const std::map<std::string, std::string>& m) {
    std::vector<BoundaryPort> ports;
    // `fromMaster`: the signal goes from the AXI master to the slave
    auto add = [&](const std::string& n, long long width, bool fromMaster) {
        if (width <= 0)
            return;
        BoundaryPort b;
        b.name = prefix + "_" + n;
        // the IP's slave interface receives the master's signals, its master interface drives them
        b.output = master == fromMaster;
        if (width * slots > 1 || n.ends_with("id") || n.ends_with("data") || n.ends_with("strb") || n.ends_with("addr"))
            b.range = std::pair<long long, long long>{width * slots - 1, 0};
        ports.push_back(b);
    };
    const bool lite = protocol == 2;
    const bool axi3 = protocol == 1;
    const auto uw = [&](const char* k) { return user ? num(m, k, 1) : 0LL; };
    for (const char* ch : {"aw", "ar"}) {
        const std::string c = ch;
        if (!lite) {
            add(c + "id", id, true);
        }
        add(c + "addr", addr, true);
        if (!lite) {
            add(c + "len", axi3 ? 4 : 8, true);
            add(c + "size", 3, true);
            add(c + "burst", 2, true);
            add(c + "lock", axi3 ? 2 : 1, true);
            add(c + "cache", 4, true);
        }
        add(c + "prot", 3, true);
        if (!lite) {
            if (!axi3)
                add(c + "region", 4, true);
            add(c + "qos", 4, true);
            add(c + "user", uw(c == "aw" ? "C_AXI_AWUSER_WIDTH" : "C_AXI_ARUSER_WIDTH"), true);
        }
        add(c + "valid", 1, true);
        add(c + "ready", 1, false);
    }
    if (axi3)
        add("wid", id, true);
    add("wdata", data, true);
    add("wstrb", data / 8, true);
    if (!lite) {
        add("wlast", 1, true);
        add("wuser", uw("C_AXI_WUSER_WIDTH"), true);
    }
    add("wvalid", 1, true);
    add("wready", 1, false);
    if (!lite)
        add("bid", id, false);
    add("bresp", 2, false);
    if (!lite)
        add("buser", uw("C_AXI_BUSER_WIDTH"), false);
    add("bvalid", 1, false);
    add("bready", 1, true);
    if (!lite)
        add("rid", id, false);
    add("rdata", data, false);
    add("rresp", 2, false);
    if (!lite) {
        add("rlast", 1, false);
        add("ruser", uw("C_AXI_RUSER_WIDTH"), false);
    }
    add("rvalid", 1, false);
    add("rready", 1, true);
    return ports;
}

/// AXI Clock Converter (axi_clock_converter, PG164): the slave interface and
/// s_axi_aresetn in s_axi_aclk, the master interface and m_axi_aresetn in
/// m_axi_aclk; the IP crosses between them (asynchronous FIFOs with
/// C_AXI_IS_ACLK_ASYNC 1, a synchronous integer ratio otherwise).
Result<XciModel> axiClockConverter(XciInfo info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const long long protocol = num(m, "C_AXI_PROTOCOL", 0);
    const bool fromConfig = info.ports.empty();
    if (fromConfig) {
        const long long id = protocol == 2 ? 0 : num(m, "C_AXI_ID_WIDTH", 0);
        const long long addr = num(m, "C_AXI_ADDR_WIDTH", 32), data = num(m, "C_AXI_DATA_WIDTH", 32);
        const bool user = num(m, "C_AXI_SUPPORTS_USER_SIGNALS", 0) != 0;
        info.ports = {{"s_axi_aclk", false, false, {}}, {"s_axi_aresetn", false, false, {}},
                      {"m_axi_aclk", false, false, {}}, {"m_axi_aresetn", false, false, {}}};
        for (auto& p : axiInterfacePorts("s_axi", false, 1, protocol, id, addr, data, user, m))
            info.ports.push_back(p);
        for (auto& p : axiInterfacePorts("m_axi", true, 1, protocol, id, addr, data, user, m))
            info.ports.push_back(p);
    }
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "s_axi_aclk" || n == "m_axi_aclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n.starts_with("s_axi_"))
            return PortClass{K::Sync, "s_axi_aclk", 0, {}, 0};
        if (n.starts_with("m_axi_"))
            return PortClass{K::Sync, "m_axi_aclk", 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    const bool async = num(m, "C_AXI_IS_ACLK_ASYNC", 1) != 0;
    r.value().summary = std::string(protocol == 2 ? "AXI4-Lite" : protocol == 1 ? "AXI3" : "AXI4") +
                        ", s_axi_aclk to m_axi_aclk, " +
                        (async ? "asynchronous clocks (" + text(m, "C_SYNCHRONIZER_STAGE", "?") + " synchronizer stages)"
                               : "synchronous clocks, ratio " + text(m, "C_S_AXI_ACLK_RATIO", "?") + ":" +
                                     text(m, "C_M_AXI_ACLK_RATIO", "?")) +
                        (fromConfig ? ", ports from the configuration" : "");
    r.value().notes.push_back("the slave interface is in s_axi_aclk, the master interface in m_axi_aclk (PG164); the IP "
                              "crosses between them");
    return r;
}

/// AXI Crossbar (axi_crossbar, PG059): one clock, aclk, for every interface.
Result<XciModel> axiCrossbar(XciInfo info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const long long protocol = num(m, "C_AXI_PROTOCOL", 0);
    const bool fromConfig = info.ports.empty();
    if (fromConfig) {
        const long long id = protocol == 2 ? 0 : num(m, "C_AXI_ID_WIDTH", 0);
        const long long addr = num(m, "C_AXI_ADDR_WIDTH", 32), data = num(m, "C_AXI_DATA_WIDTH", 32);
        const bool user = num(m, "C_AXI_SUPPORTS_USER_SIGNALS", 0) != 0;
        info.ports = {{"aclk", false, false, {}}, {"aresetn", false, false, {}}};
        for (auto& p : axiInterfacePorts("s_axi", false, num(m, "C_NUM_SLAVE_SLOTS", 1), protocol, id, addr, data, user, m))
            info.ports.push_back(p);
        for (auto& p : axiInterfacePorts("m_axi", true, num(m, "C_NUM_MASTER_SLOTS", 1), protocol, id, addr, data, user, m))
            info.ports.push_back(p);
    }
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "aclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "aresetn" || n.starts_with("s_axi_") || n.starts_with("m_axi_"))
            return PortClass{K::Sync, "aclk", 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    r.value().summary = std::string(protocol == 2 ? "AXI4-Lite" : protocol == 1 ? "AXI3" : "AXI4") + ", " +
                        text(m, "C_NUM_SLAVE_SLOTS", "?") + " x " + text(m, "C_NUM_MASTER_SLOTS", "?") +
                        ", one clock (aclk)" + (fromConfig ? ", ports from the configuration" : "");
    r.value().notes.push_back("every interface of the crossbar is in aclk (PG059)");
    return r;
}

/// Block Memory Generator with the AXI4 or AXI4-Lite slave interface (PG058,
/// C_INTERFACE_TYPE 1): one clock, s_aclk, for the AXI interface, s_aresetn
/// and the ECC ports; the ports from the configuration (32-bit addresses,
/// C_WRITE_WIDTH_A data, C_AXI_ID_WIDTH when C_HAS_AXI_ID).
Result<XciModel> blockMemoryAxi(XciInfo info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    const bool lite = num(m, "C_AXI_TYPE", 1) == 0;
    const bool fromConfig = info.ports.empty();
    if (fromConfig) {
        const long long data = num(m, "C_WRITE_WIDTH_A", 32);
        const long long id = !lite && num(m, "C_HAS_AXI_ID", 0) != 0 ? num(m, "C_AXI_ID_WIDTH", 4) : 0;
        auto add = [&](const std::string& n, long long width, bool out, bool vector = false) {
            if (width <= 0)
                return;
            BoundaryPort b;
            b.name = n;
            b.output = out;
            if (width > 1 || vector)
                b.range = std::pair<long long, long long>{width - 1, 0};
            info.ports.push_back(b);
        };
        add("s_aclk", 1, false);
        add("s_aresetn", 1, false);
        for (const char* ch : {"aw", "ar"}) {
            const std::string c = std::string("s_axi_") + ch;
            add(c + "id", id, false, true);
            add(c + "addr", 32, false);
            if (!lite) {
                add(c + "len", 8, false);
                add(c + "size", 3, false);
                add(c + "burst", 2, false);
            }
            add(c + "valid", 1, false);
            add(c + "ready", 1, true);
        }
        add("s_axi_wdata", data, false, true);
        add("s_axi_wstrb", data / 8, false, true);
        if (!lite)
            add("s_axi_wlast", 1, false);
        add("s_axi_wvalid", 1, false);
        add("s_axi_wready", 1, true);
        add("s_axi_bid", id, true, true);
        add("s_axi_bresp", 2, true);
        add("s_axi_bvalid", 1, true);
        add("s_axi_bready", 1, false);
        add("s_axi_rid", id, true, true);
        add("s_axi_rdata", data, true, true);
        add("s_axi_rresp", 2, true);
        if (!lite)
            add("s_axi_rlast", 1, true);
        add("s_axi_rvalid", 1, true);
        add("s_axi_rready", 1, false);
        if (num(m, "C_USE_ECC", 0) != 0 || num(m, "C_USE_SOFTECC", 0) != 0) {
            add("s_axi_injectsbiterr", 1, false);
            add("s_axi_injectdbiterr", 1, false);
            add("s_axi_sbiterr", 1, true);
            add("s_axi_dbiterr", 1, true);
            add("s_axi_rdaddrecc", num(m, "C_ADDRB_WIDTH", 1), true);
        }
    }
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "s_aclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "s_aresetn" || n.starts_with("s_axi_") || n == "rsta_busy" || n == "rstb_busy")
            return PortClass{K::Sync, "s_aclk", 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    r.value().summary = std::string(lite ? "AXI4-Lite" : "AXI4") + " slave, " + text(m, "C_WRITE_DEPTH_A", "?") +
                        " x " + text(m, "C_WRITE_WIDTH_A", "?") + ", one clock (s_aclk)" +
                        (fromConfig ? ", ports from the configuration" : "");
    r.value().notes.push_back("the AXI interface of the Block Memory Generator is in s_aclk (PG058)");
    return r;
}

/// AXI Interconnect (axi_interconnect 1.x, PG059): each slave port Sxx_AXI
/// and the master port M00_AXI in their own clock (Sxx_AXI_ACLK, M00_AXI_ACLK)
/// with the reset outputs Sxx_AXI_ARESET_OUT_N; INTERCONNECT_ARESETN in
/// INTERCONNECT_ACLK; the IP crosses between them (clock converters per port,
/// C_Sxx_AXI_IS_ACLK_ASYNC). The ports come from the instance.
Result<XciModel> axiInterconnect(const XciInfo& info) {
    const auto& m = info.model;
    const std::string vlnv = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    using K = PortClass::Kind;
    // "s00_axi_awaddr" -> "s00_axi"
    const auto iface = [](const std::string& n) -> std::string {
        if (n.size() > 8 && (n[0] == 's' || n[0] == 'm') && std::isdigit(static_cast<unsigned char>(n[1])) &&
            std::isdigit(static_cast<unsigned char>(n[2])) && n.substr(3, 5) == "_axi_")
            return n.substr(0, 7);
        return {};
    };
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "interconnect_aclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "interconnect_aresetn")
            return PortClass{K::Sync, "interconnect_aclk", 0, {}, 0};
        const std::string i = iface(n);
        if (i.empty())
            return std::nullopt;
        if (n == i + "_aclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        return PortClass{K::Sync, i + "_aclk", 0, {}, 0};
    };
    auto r = portDomainModel(info, vlnv, classify, {});
    if (!r)
        return r;
    const long long slaves = num(m, "C_NUM_SLAVE_PORTS", 1);
    std::string async;
    for (long long k = 0; k < slaves; ++k) {
        const std::string sk = (k < 10 ? "S0" : "S") + std::to_string(k);
        if (num(m, "C_" + sk + "_AXI_IS_ACLK_ASYNC", 0) != 0)
            async += (async.empty() ? "" : ", ") + sk;
    }
    if (num(m, "C_M00_AXI_IS_ACLK_ASYNC", 0) != 0)
        async += (async.empty() ? "" : ", ") + std::string("M00");
    r.value().summary = text(m, "C_NUM_SLAVE_PORTS", "?") + " slave port(s) to M00, each port in its own clock" +
                        (async.empty() ? std::string() : ", asynchronous to the interconnect clock: " + async);
    r.value().notes.push_back("each AXI port of the interconnect is in its own clock (Sxx_AXI_ACLK, M00_AXI_ACLK), "
                              "with its reset output; INTERCONNECT_ARESETN in INTERCONNECT_ACLK (PG059); the IP "
                              "crosses between the clocks");
    return r;
}

// ---------------------------------------------------------------------------
// Block design infrastructure: reset, logic, AXI conversion and peripherals
// ---------------------------------------------------------------------------

std::string upperCopy(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string vlnvOf(const XciInfo& info) {
    return info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
}

/// Processor System Reset (proc_sys_reset 5.0, PG164): the external, auxiliary
/// and debug resets and dcm_locked are synchronized into slowest_sync_clk;
/// every reset output is a register of that clock.
Result<XciModel> procSysReset(const XciInfo& info) {
    using K = PortClass::Kind;
    PortClassifier classify = [](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "slowest_sync_clk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (!out && (n == "ext_reset_in" || n == "aux_reset_in" || n == "mb_debug_sys_rst" || n == "dcm_locked"))
            return PortClass{K::Async, {}, 0, {}, 0};
        if (out && (n == "mb_reset" || n == "bus_struct_reset" || n == "peripheral_reset" ||
                    n == "interconnect_aresetn" || n == "peripheral_aresetn"))
            return PortClass{K::Sync, "slowest_sync_clk", 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    r.value().summary = "reset outputs synchronous to slowest_sync_clk";
    r.value().notes.push_back("ext_reset_in, aux_reset_in, mb_debug_sys_rst and dcm_locked are synchronized into "
                              "slowest_sync_clk inside the IP (PG164); the reset outputs are registers of that clock");
    return r;
}

/// Utility Vector Logic (util_vector_logic 2.0): Res = Op1 op Op2 (or ~Op1), combinational.
Result<XciModel> utilVectorLogic(const XciInfo& info) {
    const std::string op = lower(text(info.model, "C_OPERATION", text(info.user, "C_OPERATION", "and")));
    const long long size = num(info.model, "C_SIZE", num(info.user, "C_SIZE", 1));
    static const std::map<std::string, std::string> kOps = {{"and", "&"}, {"or", "|"}, {"xor", "^"}, {"not", "~"}};
    const auto it = kOps.find(op);
    if (it == kOps.end() || size < 1)
        return unsupported("IP '" + info.instance + "' (" + vlnvOf(info) + "): operation '" + op + "' is not known");
    const std::string w = size > 1 ? "[" + std::to_string(size - 1) + ":0] " : std::string();
    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << info.instance << " (\n  input  wire " << w << "Op1,\n";
    if (op != "not")
        v << "  input  wire " << w << "Op2,\n";
    v << "  output wire " << w << "Res\n);\n"
      << "  assign Res = " << (op == "not" ? "~Op1" : "Op1 " + it->second + " Op2") << ";\nendmodule\n";
    XciModel out;
    out.module = info.instance;
    out.ip = vlnvOf(info);
    out.verilog = v.str();
    out.summary = op == "not" ? "Res = ~Op1, " + std::to_string(size) + " bit(s), combinational"
                              : "Res = Op1 " + it->second + " Op2, " + std::to_string(size) + " bit(s), combinational";
    return out;
}

/// One-clock AXI infrastructure (axi_protocol_converter, axi_data_fifo,
/// axi_register_slice, axis_register_slice, axis_dwidth_converter, ...):
/// every port in `clock`.
Result<XciModel> axiOneClock(const XciInfo& info, const std::string& clock, const std::string& what) {
    using K = PortClass::Kind;
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == clock)
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "aresetn" || n == "aclken" || n.starts_with("s_axi_") || n.starts_with("m_axi_") ||
            n.starts_with("s_axis_") || n.starts_with("m_axis_") || n.starts_with("m_apb_"))
            return PortClass{K::Sync, clock, 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    r.value().summary = what + ", one clock (" + clock + ")";
    return r;
}

/// A converter between two AXI interfaces: the slave side (s_*) in
/// `sclk`, the master side (m_*) in `mclk`, crossing inside the IP when the
/// clocks are asynchronous; otherwise every port in `sclk`.
Result<XciModel> axiTwoSided(const XciInfo& info, const std::string& sclk, const std::string& mclk, bool async,
                             const std::string& what) {
    using K = PortClass::Kind;
    const std::string sp = sclk.substr(0, sclk.find("_aclk") + 1), mp = mclk.substr(0, mclk.find("_aclk") + 1);
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == sclk || n == mclk)
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n.starts_with(sp))
            return PortClass{K::Sync, sclk, 0, {}, 0};
        if (n.starts_with(mp))
            return PortClass{K::Sync, async ? mclk : sclk, 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    r.value().summary = what + (async ? ", " + sclk + " to " + mclk + ", asynchronous clocks"
                                      : ", synchronous: every port in " + sclk);
    if (!async)
        r.value().notes.push_back("without asynchronous clocks both sides must be on one clock; every port is "
                                  "modelled in " + sclk + ", so another clock on " + mclk +
                                  " shows as crossings into the IP");
    return r;
}

/// AXI BRAM Controller (axi_bram_ctrl 4.x, PG078): every port in s_axi_aclk;
/// the BRAM clocks pass s_axi_aclk on.
Result<XciModel> axiBramCtrl(const XciInfo& info) {
    using K = PortClass::Kind;
    PortClassifier classify = [](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "s_axi_aclk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (out && (n == "bram_clk_a" || n == "bram_clk_b"))
            return PortClass{K::Through, {}, 0, "s_axi_aclk", 0};
        if (n == "s_axi_aresetn" || n.starts_with("s_axi_") || n.starts_with("bram_") || n == "ecc_interrupt" ||
            n == "ecc_ue")
            return PortClass{K::Sync, "s_axi_aclk", 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    r.value().summary = "every port in s_axi_aclk; bram_clk_a/b are s_axi_aclk";
    return r;
}

/// AXI Interrupt Controller (axi_intc 4.1, PG099): the registers and irq in
/// s_axi_aclk (irq in processor_clk with C_ENABLE_ASYNC); interrupt inputs
/// marked asynchronous in C_ASYNC_INTR are synchronized inside the IP.
Result<XciModel> axiIntc(const XciInfo& info) {
    const auto& m = info.model;
    using K = PortClass::Kind;
    const auto unsupportedIp = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnvOf(info) + "): " + why,
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    };
    if (num(m, "C_HAS_FAST", 0) != 0 || num(m, "C_EN_CASCADE_MODE", 0) != 0)
        return unsupportedIp("fast interrupt mode and cascade mode are not modelled");
    const bool procAsync = num(m, "C_ENABLE_ASYNC", 0) != 0;
    const long long n = num(m, "C_NUM_INTR_INPUTS", 1);
    if (n < 1 || n > 32)
        return unsupportedIp("C_NUM_INTR_INPUTS out of range");
    const unsigned long long all = n == 64 ? ~0ULL : ((1ULL << n) - 1);
    const unsigned long long asyncMask =
        num(m, "C_DISABLE_SYNCHRONIZERS", 0) != 0 ? 0ULL : static_cast<unsigned long long>(num(m, "C_ASYNC_INTR", 0)) & all;
    const std::string irqClock = procAsync ? "processor_clk" : "s_axi_aclk";
    PortClassifier classify = [&](const std::string& p, bool out) -> std::optional<PortClass> {
        if (p == "s_axi_aclk" || p == "processor_clk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (p.starts_with("s_axi_"))
            return PortClass{K::Sync, "s_axi_aclk", 0, {}, 0};
        if (p == "intr")
            return PortClass{asyncMask == all ? K::Async : K::Sync, asyncMask == all ? "" : "s_axi_aclk", 0, {}, 0};
        if (p == "irq")
            return PortClass{K::Sync, irqClock, 0, {}, 0};
        if (p == "processor_rst" || p == "processor_ack" || p == "irq_in" || p == "interrupt_address_in")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0});
        if (p == "interrupt_address" || p == "processor_ack_out")
            return out ? std::optional<PortClass>(PortClass{K::Ignored, {}, 0, {}, 0}) : std::nullopt;
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    auto& model = r.value();
    if (asyncMask != 0 && asyncMask != all) {
        // the synchronous interrupt inputs only: the others are synchronized inside the IP
        std::ostringstream mask;
        mask << n << "'h" << std::hex << (all & ~asyncMask);
        const std::string masked = "(intr & " + mask.str() + ")";
        static const std::regex intrRe(R"(\bintr\b(?![\w\[]))");
        std::string out, line;
        std::istringstream in(model.verilog);
        while (std::getline(in, line)) {
            if (line.find("oc_dom_") != std::string::npos && line.find("<=") != std::string::npos)
                line = std::regex_replace(line, intrRe, masked);
            out += line + "\n";
        }
        model.verilog = out;
    }
    std::ostringstream ss;
    ss << n << " interrupt input(s) (" << std::popcount(asyncMask) << " asynchronous), registers in s_axi_aclk, irq in "
       << irqClock;
    model.summary = ss.str();
    if (asyncMask != 0)
        model.notes.push_back("interrupt inputs marked asynchronous (C_ASYNC_INTR) are synchronized inside the IP "
                              "(C_NUM_SYNC_FF stages); the others are sampled in s_axi_aclk");
    return r;
}

/// AXI Ethernet Lite (axi_ethernetlite 3.0, PG135), MII: the registers, the
/// interrupt, the PHY reset and MDIO in s_axi_aclk; receive data in
/// phy_rx_clk, transmit data in phy_tx_clk; carrier sense and collision are
/// asynchronous (synchronized inside the IP).
Result<XciModel> axiEthernetLite(const XciInfo& info) {
    using K = PortClass::Kind;
    PortClassifier classify = [](const std::string& n, bool out) -> std::optional<PortClass> {
        if (n == "s_axi_aclk" || n == "phy_tx_clk" || n == "phy_rx_clk")
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n.starts_with("s_axi_") || n == "ip2intc_irpt" || n == "phy_rst_n" || n == "phy_mdc" ||
            n == "phy_mdio_o" || n == "phy_mdio_t")
            return PortClass{K::Sync, "s_axi_aclk", 0, {}, 0};
        if (!out && (n == "phy_rx_data" || n == "phy_dv" || n == "phy_rx_er"))
            return PortClass{K::Sync, "phy_rx_clk", 0, {}, 0};
        if (out && (n == "phy_tx_data" || n == "phy_tx_en"))
            return PortClass{K::Sync, "phy_tx_clk", 0, {}, 0};
        if (!out && (n == "phy_crs" || n == "phy_col" || n == "phy_mdio_i"))
            return PortClass{K::Async, {}, 0, {}, 0};
        return std::nullopt;
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    r.value().summary = "MII: registers in s_axi_aclk, receive in phy_rx_clk, transmit in phy_tx_clk";
    r.value().notes.push_back("phy_crs, phy_col and phy_mdio_i are asynchronous inputs synchronized inside the IP; the "
                              "IP crosses between s_axi_aclk and the PHY clocks (PG135)");
    return r;
}

/// System ILA (system_ila 1.x): a debug core that only observes its inputs.
Result<XciModel> systemIla(const XciInfo& info) {
    using K = PortClass::Kind;
    PortClassifier classify = [](const std::string&, bool out) -> std::optional<PortClass> {
        if (out)
            return std::nullopt;
        return PortClass{K::Ignored, {}, 0, {}, 0};
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    r.value().summary = "debug core, inputs only";
    return r;
}

/// Zynq-7000 Processing System (processing_system7 5.5, PG082/UG585): the
/// fabric clocks FCLK_CLK0..3 are clocks of their own (the PS PLLs, period
/// from PCW_ACT_FPGAn_PERIPHERAL_FREQMHZ, Vivado's names clk_fpga_n); every AXI
/// port in its interface clock (M_AXI_GPn_ACLK, S_AXI_GPn/HPn/ACP_ACLK), the
/// DMA request interfaces in DMAn_ACLK, EMIO GMII in the GMII clocks; the
/// fabric resets, interrupts to the fabric and the other EMIO signals come from
/// PS clocks (no fabric clock domain); interrupts and EMIO inputs are
/// synchronized in the PS; MIO/DDR pins are not read.
Result<XciModel> processingSystem7(const XciInfo& info) {
    using K = PortClass::Kind;
    std::map<std::string, std::string> aliases;
    std::vector<std::string> clocks;
    double mhz[4] = {0, 0, 0, 0};
    for (int n = 0; n < 4; ++n) {
        const std::string key = "PCW_ACT_FPGA" + std::to_string(n) + "_PERIPHERAL_FREQMHZ";
        try {
            mhz[n] = std::stod(text(info.user, key, text(info.model, key, "0")));
        }
        catch (...) {
        }
        aliases["fclk_clk" + std::to_string(n)] = "clk_fpga_" + std::to_string(n);
    }
    static const std::regex axiRe(R"(^((m_axi_gp[01]|s_axi_gp[01]|s_axi_hp[0-3]|s_axi_acp)_)(.*)$)");
    static const std::regex dmaRe(R"(^(dma[0-3])_(.*)$)");
    static const std::regex gmiiRe(R"(^(enet[01])_gmii_(.*)$)");
    static const std::regex pinRe(R"(^(mio|ddr_.*|ps_srstb|ps_clk|ps_porb)$)");
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        std::smatch m;
        if (std::regex_match(n, m, std::regex(R"(^fclk_clk([0-3])$)")))
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, mhz[std::stoi(m[1].str())], {}, 0})
                       : std::nullopt;
        if (std::regex_match(n, pinRe))
            return PortClass{K::Ignored, {}, 0, {}, 0};
        if (std::regex_match(n, m, axiRe)) {
            if (m[3].str() == "aclk")
                return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
            return PortClass{K::Sync, m[1].str() + "aclk", 0, {}, 0};
        }
        if (std::regex_match(n, m, dmaRe)) {
            if (m[2].str() == "aclk")
                return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
            return PortClass{K::Sync, m[1].str() + "_aclk", 0, {}, 0};
        }
        if (std::regex_match(n, m, gmiiRe)) {
            const std::string sig = m[2].str();
            if (sig == "tx_clk" || sig == "rx_clk")
                return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
            if (sig == "col" || sig == "crs")
                return PortClass{K::Async, {}, 0, {}, 0};
            if (sig.starts_with("tx") && out)
                return PortClass{K::Sync, m[1].str() + "_gmii_tx_clk", 0, {}, 0};
            if (sig.starts_with("rx") && !out)
                return PortClass{K::Sync, m[1].str() + "_gmii_rx_clk", 0, {}, 0};
            return std::nullopt;
        }
        // EMIO GPIO: the GPIO controller runs on the APB clock (CPU_1x)
        if (out && (n == "gpio_o" || n == "gpio_t"))
            return PortClass{K::Sync, "cpu_1x", 0, {}, 0};
        // fabric resets, interrupts, other EMIO peripherals, trace, events: PS clocks
        return PortClass{K::Async, {}, 0, {}, 0};
    };
    // CPU_1x = the CPU clock / 6 (6:2:1) or / 4 (4:2:1)
    double cpu1x = 0;
    try {
        const double apu = std::stod(text(info.user, "PCW_ACT_APU_PERIPHERAL_FREQMHZ",
                                          text(info.model, "PCW_ACT_APU_PERIPHERAL_FREQMHZ", "0")));
        const std::string ratio = text(info.user, "PCW_APU_CLK_RATIO_ENABLE", "6:2:1");
        cpu1x = apu / (ratio.starts_with("4") ? 4.0 : 6.0);
    }
    catch (...) {
    }
    auto r = portDomainModel(info, vlnvOf(info), classify, {{"cpu_1x", cpu1x}}, aliases);
    if (!r)
        return r;
    std::string fclk;
    for (const auto& p : info.ports) {
        std::smatch m;
        const std::string n = lower(p.name);
        if (std::regex_match(n, m, std::regex(R"(^fclk_clk([0-3])$)"))) {
            const int k = std::stoi(m[1].str());
            fclk += (fclk.empty() ? "" : ", ") + p.name + " " +
                    (mhz[k] > 0 ? fmtReal(mhz[k]) + " MHz" : std::string("(no frequency)")) + " (clk_fpga_" +
                    std::to_string(k) + ")";
        }
    }
    r.value().summary = "fabric clocks " + (fclk.empty() ? std::string("none") : fclk) +
                        "; AXI ports in their interface clocks";
    r.value().notes.push_back("the fabric clocks are clocks of their own (PS PLLs, Vivado's names clk_fpga_n); "
                              "EMIO GPIO outputs are registers of the PS clock CPU_1x (a clock of its own, unrelated to "
                              "the fabric clocks); FCLK_RESETn_N, the interrupts to the fabric and the other EMIO "
                              "signals come from PS clocks and have no fabric clock domain; IRQ_F2P and EMIO inputs "
                              "are synchronized in the PS");
    return r;
}

/// SmartConnect (smartconnect 1.0, PG247): each AXI interface in the clock
/// its clock interface associates with it (ASSOCIATED_BUSIF of aclk, aclk1,
/// ...: the configuration's clock assignment); aresetn in aclk; the IP
/// crosses between the clocks.
Result<XciModel> smartConnect(const XciInfo& info) {
    using K = PortClass::Kind;
    if (info.busClock.empty())
        return unsupported("IP '" + info.instance + "' (" + vlnvOf(info) + "): the clock of each interface is not "
                           "known (no ASSOCIATED_BUSIF in the component file or the JSON .xci)",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    std::set<std::string> clocks;
    for (const auto& [bus, clk] : info.busClock)
        clocks.insert(lower(clk));
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (clocks.contains(n) || (n.starts_with("aclk") && !out))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (n == "aresetn")
            return PortClass{K::Sync, clocks.contains("aclk") ? "aclk" : *clocks.begin(), 0, {}, 0};
        std::string best, clk;
        for (const auto& [bus, c] : info.busClock)
            if (n.starts_with(bus + "_") && bus.size() > best.size()) {
                best = bus;
                clk = c;
            }
        if (best.empty())
            return std::nullopt;
        return PortClass{K::Sync, clk, 0, {}, 0};
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    std::map<std::string, std::string> byClock;
    for (const auto& [bus, clk] : info.busClock)
        byClock[clk] += (byClock[clk].empty() ? "" : ", ") + upperCopy(bus);
    std::string s;
    for (const auto& [clk, buses] : byClock)
        s += (s.empty() ? "" : "; ") + clk + ": " + buses;
    r.value().summary = std::to_string(byClock.size()) + " clock(s): " + s;
    if (byClock.size() > 1)
        r.value().notes.push_back("the interfaces are in the clocks the configuration associates them with "
                                  "(ASSOCIATED_BUSIF); the IP crosses between the clocks");
    return r;
}

/// An IP without a model whose output products cannot be used (encrypted
/// HDL and netlist, an HLS IP exported without them), known by the clocks of
/// its interfaces only: the ports of every bus interface associated with a
/// clock (ASSOCIATED_BUSIF and the port maps of the component file or the JSON
/// .xci) are in that clock, as Vivado times them; the other ports (resets,
/// interrupts, ports outside the bus interfaces) have no clock domain and are
/// not checked. Crossings inside the IP are not analyzed: the IP is assumed
/// to cross between its clocks itself. Not applicable to an IP with an output
/// clock (its period and relation are not known).
Result<XciModel> interfaceModel(const XciInfo& info) {
    using K = PortClass::Kind;
    const auto no = [&](const std::string& why) {
        return unsupported("IP '" + info.instance + "' (" + vlnvOf(info) + "): " + why, {});
    };
    if (info.portClock.empty() || info.ports.empty())
        return no("the clocks of its interfaces are not known");
    std::set<std::string> clocks;
    for (const auto& [port, clk] : info.portClock)
        clocks.insert(lower(clk));
    for (const auto& p : info.ports)
        if (clocks.contains(lower(p.name)) && (p.output || p.inout))
            return no("its interface clock " + p.name + " is an output");
    std::size_t sync = 0;
    std::string unchecked;
    for (const auto& p : info.ports) {
        const auto n = lower(p.name);
        if (clocks.contains(n))
            continue;
        if (info.portClock.contains(n))
            ++sync;
        else if (!p.inout)
            unchecked += (unchecked.empty() ? "" : ", ") + p.name;
    }
    if (sync == 0)
        return no("none of its ports is in a bus interface associated with a clock");
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        if (clocks.contains(n))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (const auto it = info.portClock.find(n); it != info.portClock.end())
            return PortClass{K::Sync, lower(it->second), 0, {}, 0};
        return PortClass{K::Async, {}, 0, {}, 0};
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {});
    if (!r)
        return r;
    std::string clockList;
    for (const auto& p : info.ports)
        if (clocks.contains(lower(p.name)))
            clockList += (clockList.empty() ? "" : ", ") + p.name;
    r.value().interfaceOnly = true;
    r.value().summary = std::to_string(sync) + " port(s) of its bus interfaces in " + std::to_string(clocks.size()) +
                        " clock(s) (" + clockList + ")";
    r.value().notes.push_back("only the clocks of the IP's interfaces are known (ASSOCIATED_BUSIF): crossings inside "
                              "the IP are not analyzed; add the IP's HDL, or a waiver once the IP is known to be safe");
    if (!unchecked.empty())
        r.value().notes.push_back("ports without a clock domain, not checked: " + unchecked);
    return r;
}

/// Zynq UltraScale+ MPSoC Processing System (zynq_ultra_ps_e 3.x, PG201):
/// the PL clocks pl_clk0..3 are clocks of their own (period from
/// PSU__CRL_APB__PLn_REF_CTRL__ACT_FREQMHZ, Vivado's names clk_pl_n); every
/// AXI port in the clock its interface is associated with (maxihpm0_fpd_aclk
/// for maxigp0_*, saxihp0_fpd_aclk for saxigp2_*, ...: the component file's
/// ASSOCIATED_BUSIF and port maps); the EMIO GPIO outputs are registers of the
/// LPD peripheral clock (LPD_LSBUS); pl_resetn, interrupts and the other EMIO
/// signals have no fabric clock domain; inputs to the PS are synchronized there.
Result<XciModel> zynqUltraPs(const XciInfo& info) {
    using K = PortClass::Kind;
    if (info.portClock.empty())
        return unsupported("IP '" + info.instance + "' (" + vlnvOf(info) + "): the clock of each AXI port is not known "
                           "(no ASSOCIATED_BUSIF and port maps in the component file or the JSON .xci)",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    double mhz[4] = {0, 0, 0, 0};
    std::map<std::string, std::string> aliases;
    for (int n = 0; n < 4; ++n) {
        const std::string key = "PSU__CRL_APB__PL" + std::to_string(n) + "_REF_CTRL__ACT_FREQMHZ";
        try {
            mhz[n] = std::stod(text(info.user, key, text(info.model, key, "0")));
        }
        catch (...) {
        }
        aliases["pl_clk" + std::to_string(n)] = "clk_pl_" + std::to_string(n);
    }
    double lsbus = 0;
    try {
        lsbus = std::stod(text(info.user, "PSU__CRL_APB__LPD_LSBUS_CTRL__ACT_FREQMHZ",
                               text(info.model, "PSU__CRL_APB__LPD_LSBUS_CTRL__ACT_FREQMHZ", "0")));
    }
    catch (...) {
    }
    std::set<std::string> clocks;
    for (const auto& [port, clk] : info.portClock)
        clocks.insert(lower(clk));
    PortClassifier classify = [&](const std::string& n, bool out) -> std::optional<PortClass> {
        std::smatch m;
        if (std::regex_match(n, m, std::regex(R"(^pl_clk([0-3])$)")))
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, mhz[std::stoi(m[1].str())], {}, 0})
                       : std::nullopt;
        if (clocks.contains(n) || (n.ends_with("_aclk") && !out))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0, {}, 0});
        if (const auto it = info.portClock.find(n); it != info.portClock.end())
            return PortClass{K::Sync, lower(it->second), 0, {}, 0};
        if (out && (n == "emio_gpio_o" || n == "emio_gpio_t"))
            return PortClass{K::Sync, "lpd_lsbus", 0, {}, 0};
        // PL resets, interrupts, other EMIO peripherals, trace, events: PS clocks
        return PortClass{K::Async, {}, 0, {}, 0};
    };
    auto r = portDomainModel(info, vlnvOf(info), classify, {{"lpd_lsbus", lsbus}}, aliases);
    if (!r)
        return r;
    std::string plclk;
    for (const auto& p : info.ports) {
        std::smatch m;
        const std::string n = lower(p.name);
        if (std::regex_match(n, m, std::regex(R"(^pl_clk([0-3])$)"))) {
            const int k = std::stoi(m[1].str());
            plclk += (plclk.empty() ? "" : ", ") + p.name + " " +
                     (mhz[k] > 0 ? fmtReal(mhz[k]) + " MHz" : std::string("(no frequency)")) + " (clk_pl_" +
                     std::to_string(k) + ")";
        }
    }
    r.value().summary = "PL clocks " + (plclk.empty() ? std::string("none") : plclk) + "; AXI ports in " +
                        std::to_string(clocks.size()) + " interface clock(s)";
    r.value().notes.push_back("the PL clocks are clocks of their own (PS PLLs, Vivado's names clk_pl_n); the EMIO GPIO "
                              "outputs are registers of the PS clock LPD_LSBUS; pl_resetn, the interrupts and the other "
                              "EMIO signals have no fabric clock domain; inputs to the PS are synchronized there");
    return r;
}

Result<XciModel> dispatchModelled(const XciInfo& info, const std::filesystem::path* dir) {
    if (info.name == "fifo_generator")
        return fifoGenerator(info);
    if (info.name == "axis_data_fifo")
        return axisDataFifo(info);
    if (info.name == "clk_wiz")
        return clockingWizard(info);
    if (info.name == "blk_mem_gen")
        return blockMemory(info);
    if (info.name == "mig_7series") {
        const std::string prjName = text(info.user, "XML_INPUT_FILE", "mig_a.prj");
        std::optional<std::string> prj;
        if (dir)
            if (auto t = readSourceText(*dir / std::filesystem::path(prjName)))
                prj = t->utf8;
        return memoryInterface(info, prj, prjName);
    }
    static const std::set<std::string> kOneClock = {"cordic",      "floating_point", "div_gen",  "mult_gen",
                                                    "c_addsub",    "dds_compiler",   "fir_compiler", "cmpy",
                                                    "c_shift_ram", "c_counter_binary", "xbip_dsp48_macro",
                                                    "c_accum",     "cic_compiler"};
    if (kOneClock.contains(info.name))
        return oneClockIp(info);
    // AXI peripherals on one clock (s_axi_aclk); external clocks are not modelled
    static const std::set<std::string> kAxiOneClock = {"axi_gpio", "axi_iic", "axi_timer", "axi_uartlite",
                                                       "axi_uart16550"};
    if (kAxiOneClock.contains(info.name)) {
        if (num(info.model, "C_HAS_EXTERNAL_XIN", 0) != 0 || num(info.model, "C_HAS_EXTERNAL_RCLK", 0) != 0)
            return unsupported("IP '" + info.instance + "' (AXI UART 16550) with an external clock (xin/rclk) is not "
                               "modelled",
                               {"add the IP's generated HDL to the sources instead, or set the project option "
                                "unknownModules to blackbox"});
        return oneClockIp(info);
    }
    if (info.name == "axi_quad_spi")
        return quadSpi(info);
    if (info.name == "xadc_wiz")
        return xadcWizard(info);
    if (info.name == "xlslice" || info.name == "xlconcat" || info.name == "xlconstant")
        return wiringIp(info);
    if (info.name == "ila" || info.name == "vio")
        return debugCore(info, info.name == "vio");
    if (info.name == "gtwizard_ultrascale")
        return gtWizard(info);
    if (info.name == "gtwizard")
        return gtWizard7(info);
    if (info.name == "ten_gig_eth_pcs_pma")
        return tenGigEthPcsPma(info);
    if (info.name == "xxv_ethernet")
        return xxvEthernet(info);
    if (info.name == "gig_ethernet_pcs_pma")
        return gigEthernetPcsPma(info);
    if (info.name == "xaui")
        return xaui(info);
    if (info.name == "cmac_usplus")
        return cmacUsplus(info);
    if (info.name == "ieee802d3_clause74_fec")
        return clause74Fec(info);
    if (info.name == "axi_dma")
        return axiDma(info);
    if (info.name == "axi_clock_converter")
        return axiClockConverter(info);
    if (info.name == "axi_crossbar")
        return axiCrossbar(info);
    if (info.name == "axi_interconnect")
        return axiInterconnect(info);
    if (info.name == "proc_sys_reset")
        return procSysReset(info);
    if (info.name == "processing_system7")
        return processingSystem7(info);
    if (info.name == "smartconnect")
        return smartConnect(info);
    if (info.name == "zynq_ultra_ps_e")
        return zynqUltraPs(info);
    if (info.name == "util_vector_logic")
        return utilVectorLogic(info);
    if (info.name == "axi_protocol_converter")
    {
        auto proto = [&](const char* key) {
            const long long v = num(info.model, key, -1);
            return v == 0 ? std::string("AXI4") : v == 1 ? std::string("AXI3") : v == 2 ? std::string("AXI4-Lite") : "?";
        };
        return axiOneClock(info, "aclk",
                           "AXI protocol converter " + proto("C_S_AXI_PROTOCOL") + " to " + proto("C_M_AXI_PROTOCOL"));
    }
    if (info.name == "axi_data_fifo")
        return axiOneClock(info, "aclk", "AXI data FIFO");
    if (info.name == "axi_register_slice")
        return axiOneClock(info, "aclk", "AXI register slice");
    if (info.name == "axis_register_slice" || info.name == "axis_dwidth_converter" ||
        info.name == "axis_subset_converter" || info.name == "axis_broadcaster" || info.name == "axis_combiner")
        return axiOneClock(info, "aclk", "AXI4-Stream " + info.name.substr(5));
    if (info.name == "axi_dwidth_converter") {
        // asynchronous clocks (C_AXI_IS_ACLK_ASYNC) or the ratio mode of the FIFO (C_FIFO_MODE 2)
        const bool split = num(info.model, "C_AXI_IS_ACLK_ASYNC", 0) != 0 || num(info.model, "C_FIFO_MODE", 0) == 2;
        return axiTwoSided(info, "s_axi_aclk", "m_axi_aclk", split,
                           "AXI data width converter " + text(info.model, "C_S_AXI_DATA_WIDTH", "?") + " to " +
                               text(info.model, "C_M_AXI_DATA_WIDTH", "?") + " bits");
    }
    if (info.name == "axis_clock_converter")
        return axiTwoSided(info, "s_axis_aclk", "m_axis_aclk", num(info.model, "C_IS_ACLK_ASYNC", 1) != 0,
                           "AXI4-Stream clock converter");
    if (info.name == "axi_bram_ctrl")
        return axiBramCtrl(info);
    if (info.name == "axi_apb_bridge")
        return axiOneClock(info, "s_axi_aclk", "AXI to APB bridge");
    if (info.name == "axi_intc")
        return axiIntc(info);
    if (info.name == "axi_ethernetlite")
        return axiEthernetLite(info);
    if (info.name == "system_ila")
        return systemIla(info);
    return unsupported("IP '" + info.instance + "' (" + info.vendor + ":" + info.library + ":" + info.name + ":" +
                           info.version + ") is not modelled from its .xci",
                       {"add the IP's generated HDL to the sources instead (Vivado: <ip>/synth/" + info.instance +
                            ".v or .vhd, or the simulation netlist), or set the project option unknownModules to "
                            "blackbox"});
}

} // namespace

Result<XciModel> modelFromXciText(std::string_view text) {
    auto parsed = parseXci(text);
    if (!parsed)
        return std::move(parsed).error();
    return dispatch(parsed.value(), nullptr);
}

Result<XciModel> modelFromXciText(std::string_view text, const std::filesystem::path& dir) {
    auto parsed = parseXci(text);
    if (!parsed)
        return std::move(parsed).error();
    return dispatch(parsed.value(), &dir);
}

namespace {

/// A Vivado .xcix (core container: a ZIP archive of the IP directory with the
/// .xci and its output products) unpacked into a directory of its own; the
/// path of the .xci in it, or an error.
Result<std::filesystem::path> unpackXcix(const std::filesystem::path& file) {
    const auto fail = [&](const std::string& why) {
        return Diagnostic{"FE-XCI-PARSE", Severity::Warning, "cannot read the IP container (.xcix): " + why,
                          SourceLocation{toUtf8(file), 0, 0}, {}};
    };
    auto zip = readZipFile(file);
    if (!zip)
        return fail("not a readable ZIP archive (stored or deflated entries)");
    const std::string stem = file.stem().string();
    std::string xci;
    for (const auto& [name, data] : *zip) {
        const std::filesystem::path p(name);
        if (lower(p.extension().string()) != ".xci")
            continue;
        // the IP's own .xci (<ip>/<ip>.xci), not one of a sub-core
        if (xci.empty() || lower(p.stem().string()) == lower(stem))
            xci = name;
    }
    if (xci.empty())
        return fail("no .xci in the archive");
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(file, ec);
    const auto dir = std::filesystem::temp_directory_path(ec) / "opencdc-xcix" /
                     (stem + "_" + std::to_string(std::hash<std::string>{}(toUtf8(ec ? file : canonical)) % 100000000));
    for (const auto& [name, data] : *zip) {
        const std::filesystem::path rel = std::filesystem::path(name).lexically_normal();
        // no absolute paths or ".." out of the directory (zip slip)
        if (rel.is_absolute() || rel.has_root_name() || rel.empty() || *rel.begin() == "..")
            continue;
        const auto out = dir / rel;
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ifstream old(out, std::ios::binary);
        const std::string current((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        old.close();
        if (current != data) {
            std::ofstream o(out, std::ios::binary | std::ios::trunc);
            o.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!o)
                return fail("cannot write " + toUtf8(out));
        }
    }
    return dir / std::filesystem::path(xci).lexically_normal();
}

} // namespace

Result<XciModel> modelFromXci(const std::filesystem::path& original, const XciContext& context) {
    std::filesystem::path file = original;
    if (lower(original.extension().string()) == ".xcix") {
        auto unpacked = unpackXcix(original);
        if (!unpacked)
            return std::move(unpacked).error();
        file = unpacked.value();
        auto r = modelFromXci(file, context);
        if (!r) {
            Diagnostic d = std::move(r).error();
            d.location.file = toUtf8(original);
            return d;
        }
        r.value().notes.push_back("from the IP container " + toUtf8(original.filename()));
        return r;
    }
    auto text = readSourceText(file);
    if (!text)
        return Diagnostic{"FE-XCI-PARSE", Severity::Warning, "cannot read IP configuration file",
                          SourceLocation{toUtf8(file), 0, 0}, {}};
    auto parsed = parseXci(text->utf8);
    const auto dir = file.parent_path();
    Result<XciModel> r = parsed ? dispatch(parsed.value(), &dir, &context) : Result<XciModel>(std::move(parsed).error());
    if (!r) {
        Diagnostic d = std::move(r).error();
        d.location.file = toUtf8(file);
        return d;
    }
    return r;
}

Result<XciModel> modelFromXci(const std::filesystem::path& file) {
    if (lower(file.extension().string()) == ".xcix")
        return modelFromXci(file, XciContext{});
    auto text = readSourceText(file);
    if (!text)
        return Diagnostic{"FE-XCI-PARSE", Severity::Warning, "cannot read IP configuration file",
                          SourceLocation{toUtf8(file), 0, 0}, {}};
    auto r = modelFromXciText(text->utf8, file.parent_path());
    if (!r) {
        Diagnostic d = std::move(r).error();
        d.location.file = toUtf8(file);
        return d;
    }
    return r;
}

} // namespace opencdc::frontend

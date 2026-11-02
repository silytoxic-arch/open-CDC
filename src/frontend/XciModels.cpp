#include "frontend/XciModels.h"

#include "core/PathUtil.h"
#include "core/TextEncoding.h"
#include "frontend/vhdl/VhdlParser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
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
    static const std::regex leftRe(R"(<spirit:left\b[^>]*>\s*(-?[0-9]+)\s*</spirit:left>)");
    static const std::regex rightRe(R"(<spirit:right\b[^>]*>\s*(-?[0-9]+)\s*</spirit:right>)");
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
    std::ostringstream params;
    if (pll) {
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
    v << "  wire oc_locked;\n"
      << "  " << prim << " #(" << params.str() << ")\n    oc_" << (pll ? "pll" : "mmcm") << " (.CLKIN1(oc_clkin1), .CLKIN2(oc_clkin2), .CLKINSEL("
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

Result<XciModel> blockMemory(const XciInfo& info) {
    const auto& m = info.model;
    if (num(m, "C_INTERFACE_TYPE", 0) != 0)
        return unsupported("IP '" + info.instance + "': Block Memory Generator with an AXI4 interface is not modelled",
                           {"use the IP's generated HDL, or set the project option unknownModules to blackbox"});
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
    for (const auto& p : ports)
        if (p.name.starts_with("s_axi4_") || p.name == "cfgclk" || p.name == "cfgmclk" || p.name == "eos" ||
            p.name == "preq" || p.name == "usrcclkts")
            return unsupported("IP '" + info.instance + "' (AXI Quad SPI): the AXI4 memory interface and the STARTUP "
                               "primitive ports are not modelled",
                               {"add the IP's generated HDL to the sources instead, or set the project option "
                                "unknownModules to blackbox"});
    if (!has("s_axi_aclk") || !has("ext_spi_clk"))
        return unsupported("IP '" + info.instance + "' (AXI Quad SPI): no s_axi_aclk / ext_spi_clk port");
    const bool async = num(m, "Async_Clk", 0) != 0;
    // the SPI side: pins (io*, sck, ss, spisel)
    auto spiSide = [](const std::string& n) {
        return n.starts_with("io") || n.starts_with("sck_") || n.starts_with("ss_") || n == "spisel";
    };
    std::string axiIn, spiIn;
    for (const auto& p : ports) {
        if (p.output || p.name == "s_axi_aclk" || p.name == "ext_spi_clk")
            continue;
        std::string& list = spiSide(p.name) ? spiIn : axiIn;
        list += (list.empty() ? "" : ", ") + p.name;
    }
    std::ostringstream v;
    std::vector<std::string> notes;
    // asynchronous FIFOs between the sides (transmit and receive)
    std::string fifos;
    if (async) {
        long long depth = num(m, "C_FIFO_DEPTH", 16);
        unsigned bits = std::max(2U, clog2(std::max(depth, 4LL)));
        for (const char* dirName : {"tx", "rx"}) {
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
    v << "  reg oc_axi_mix = 1'b0, oc_spi_mix = 1'b0;\n"
      << "  always @(posedge s_axi_aclk) oc_axi_mix <= " << (axiIn.empty() ? std::string("1'b0") : "^{" + axiIn + "}")
      << ";\n"
      << "  always @(posedge ext_spi_clk) oc_spi_mix <= " << (spiIn.empty() ? std::string("1'b0") : "^{" + spiIn + "}")
      << ";\n"
      << "  wire oc_to_spi, oc_to_axi;\n";
    if (async) {
        v << "  wire oc_tx_full, oc_tx_empty, oc_rx_full, oc_rx_empty;\n"
          << "  " << info.instance << "_oc_tx_fifo u_oc_tx_fifo (.wr_clk(s_axi_aclk), .rd_clk(ext_spi_clk), .rst(" << rst
          << "), .din(oc_axi_mix), .wr_en(1'b1), .rd_en(1'b1), .dout(oc_to_spi), .full(oc_tx_full), "
             ".empty(oc_tx_empty));\n"
          << "  " << info.instance << "_oc_rx_fifo u_oc_rx_fifo (.wr_clk(ext_spi_clk), .rd_clk(s_axi_aclk), .rst(" << rst
          << "), .din(oc_spi_mix), .wr_en(1'b1), .rd_en(1'b1), .dout(oc_to_axi), .full(oc_rx_full), "
             ".empty(oc_rx_empty));\n";
    }
    else {
        v << "  assign oc_to_spi = oc_axi_mix;\n  assign oc_to_axi = oc_spi_mix;\n";
    }
    for (const auto& p : ports) {
        if (!p.output)
            continue;
        const bool spi = spiSide(p.name);
        const std::string r = "oc_" + p.name;
        const std::string w = std::to_string(p.width);
        v << "  reg " << (p.isVec ? vec(p.width) + " " : std::string()) << r << " = 0;\n"
          << "  always @(posedge " << (spi ? "ext_spi_clk" : "s_axi_aclk") << ") " << r << " <= {" << w << "{"
          << (spi ? "oc_to_spi ^ oc_spi_mix" : "oc_to_axi ^ oc_axi_mix") << "}};\n"
          << "  assign " << p.name << " = " << r << ";\n";
    }
    v << "endmodule\n\n" << fifos;
    XciModel out;
    out.module = info.instance;
    out.ip = info.vendor + ":" + info.library + ":" + info.name + ":" + info.version;
    out.verilog = v.str();
    out.summary = std::string("AXI side on s_axi_aclk, SPI side on ext_spi_clk, ") +
                  (async ? "exchanged through asynchronous FIFOs (Async_Clk)"
                         : "exchanged directly (Async_Clk = 0: both clocks must be the same)");
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
        if (lower(text(m, "TEMP_MON_CONTROL", "INTERNAL")) == "external")
            add("device_temp_i", 0, 12, true);
        else
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

bool isXciFile(const std::filesystem::path& file) {
    return lower(file.extension().string()) == ".xci";
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

Result<XciModel> dispatch(XciInfo info, const std::filesystem::path* dir, const XciContext* context = nullptr) {
    // XML .xci: the port list from the component file next to it, when present,
    // otherwise from a VHDL component declaration of the IP in the design
    if (!info.json && info.ports.empty() && dir)
        if (auto t = readSourceText(*dir / (info.instance + ".xml")))
            if (auto ports = parseComponentPorts(t->utf8))
                info.ports = std::move(*ports);
    std::string fromComponent;
    if (!info.json && info.ports.empty() && context)
        if (const auto it = context->vhdlComponents.find(lower(info.instance)); it != context->vhdlComponents.end()) {
            info.ports = it->second.ports;
            fromComponent = it->second.where;
        }
    auto r = dispatchModelled(info, dir);
    if (r && !fromComponent.empty())
        r.value().notes.push_back("ports from the VHDL component declaration at " + fromComponent);
    if (r || !context || context->ipRepositories.empty() || r.error().code != "FE-XCI-UNSUPPORTED")
        return r;
    // not modelled: the IP itself, from an IP repository
    for (const auto& comp : componentsIn(context->ipRepositories))
        if (lower(comp.vendor) == lower(info.vendor) && lower(comp.library) == lower(info.library) &&
            comp.name == info.name && comp.version == info.version)
            return userIp(info, comp);
    return r;
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
    enum class Kind { Sync, Async, Ignored, ClockIn, ClockOut };
    Kind kind = Kind::Ignored;
    std::string clock; ///< Sync: the clock (an input or output clock port, or an internal clock's name)
    double mhz = 0;    ///< ClockOut: the frequency, 0 when not configured
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
                                 const std::vector<InternalClock>& internals) {
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
    };
    std::vector<P> ports;
    std::string unknown;
    for (const auto& b : info.ports) {
        auto c = b.inout ? std::nullopt : classify(lower(b.name), b.output);
        if (!c) {
            unknown += (unknown.empty() ? "" : ", ") + b.name;
            continue;
        }
        ports.push_back({b.name, b.output,
                         b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U, *c});
    }
    if (!unknown.empty())
        return unsupportedIp("ports whose clock domain is not modelled: " + unknown);

    std::ostringstream v;
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << info.instance << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].out ? "output wire " : "input  wire ") << vec(ports[i].width) << " " << ports[i].name
          << (i + 1 < ports.size() ? ",\n" : "\n");
    v << ");\n";
    auto clockSource = [&](const std::string& pin, double mhz, const std::string& wire) {
        v << "  (* OPENCDC_CLOCK_PIN = \"" << pin << "\"";
        if (mhz > 0)
            v << ", OPENCDC_CLOCK_PERIOD = " << fmtReal(1000.0 / mhz);
        v << " *) wire " << wire << ";\n  opencdc_async_source u_" << wire << " (" << wire << ");\n";
    };
    // output clocks and the internal clocks in use
    std::map<std::string, std::string> clockNet; // lower-case clock name -> net
    for (const auto& p : ports)
        if (p.out && p.c.kind == PortClass::Kind::ClockOut) {
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
    for (const auto& ic : internals)
        if (domains.contains(lower(ic.name)) && !clockNet.contains(lower(ic.name))) {
            clockSource(ic.name, ic.mhz, "oc_" + ic.name + "_ic");
            clockNet[lower(ic.name)] = "oc_" + ic.name + "_ic";
        }
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
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0});
        if (n == "txoutclk" || n == "rxrecclk_out")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, gtMhz}) : std::nullopt;
        if (n == "xgmii_rxd" || n == "xgmii_rxc")
            return PortClass{K::Sync, rxSource, 0};
        if (core.contains(n))
            return PortClass{K::Sync, txSource, 0};
        if (n.starts_with("drp_") || n.starts_with("core_to_gt_drp") || n.starts_with("gt_drp"))
            return PortClass{K::Sync, "dclk", 0};
        if (n == "qpll0reset" || n == "reset_tx_bufg_gt")
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0}) : std::nullopt;
        if (ignored.contains(n))
            return PortClass{K::Ignored, {}, 0};
        std::string g;
        if (n.starts_with("gt0_"))
            g = n.substr(4);
        else if (n.starts_with("gt_"))
            g = n.substr(3);
        if (g.empty())
            return std::nullopt;
        if (txGt.contains(g))
            return PortClass{K::Sync, "txusrclk2", 0};
        if (rxGt.contains(g))
            return PortClass{K::Sync, "rxrecclk_out", 0};
        if (asyncGt.contains(g))
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0}) : std::nullopt;
        if (ignoredGt.contains(g))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0});
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
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::ClockIn, {}, 0});
        if (n == "tx_mii_clk" || n == "rx_clk_out")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, miiMhz}) : std::nullopt;
        if (n == "rxrecclkout")
            return out ? std::optional<PortClass>(PortClass{K::ClockOut, {}, 0}) : std::nullopt;
        const std::string tx = full.ends_with("_0") ? "tx_mii_clk_0" : "tx_mii_clk";
        const std::string rxs = full.ends_with("_0") ? "rx_clk_out_0" : "rx_clk_out";
        const std::string rxc = full.ends_with("_0") ? "rx_core_clk_0" : "rx_core_clk";
        if (n == "tx_mii_d" || n == "tx_mii_c" || n.starts_with("ctl_tx_") || n.starts_with("stat_tx_"))
            return PortClass{K::Sync, tx, 0};
        if (n == "rx_mii_d" || n == "rx_mii_c")
            return PortClass{K::Sync, rxc, 0};
        if (n.starts_with("ctl_rx_") || n.starts_with("stat_rx_"))
            return PortClass{K::Sync, rxs, 0};
        if (n == "gt_reset_tx_done_out" || n == "gt_reset_rx_done_out" || n == "gtwiz_reset_qpll0reset_out" ||
            n == "gtwiz_reset_qpll1reset_out")
            return out ? std::optional<PortClass>(PortClass{K::Sync, "dclk", 0}) : std::nullopt;
        if (n == "gtpowergood_out")
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0}) : std::nullopt;
        if (n.starts_with("gt_drp"))
            return PortClass{K::Sync, full.ends_with("_0") ? "gt_drpclk_0" : "gt_drpclk", 0};
        if (ignored.contains(n))
            return out ? std::nullopt : std::optional<PortClass>(PortClass{K::Ignored, {}, 0});
        if (!n.starts_with("gt_"))
            return std::nullopt;
        const std::string g = n.substr(3);
        if (txGt.contains(g))
            return PortClass{K::Sync, tx, 0};
        if (rxGt.contains(g))
            return PortClass{K::Sync, rxs, 0};
        if (asyncGt.contains(g))
            return out ? std::optional<PortClass>(PortClass{K::Async, {}, 0}) : std::nullopt;
        if (ignoredGt.contains(g))
            return PortClass{K::Ignored, {}, 0};
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

Result<XciModel> modelFromXci(const std::filesystem::path& file, const XciContext& context) {
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

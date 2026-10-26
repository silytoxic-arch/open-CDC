#include "frontend/XciModels.h"

#include "core/PathUtil.h"
#include "core/TextEncoding.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
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

struct BoundaryPort {
    std::string name;
    bool output = false;
    std::optional<std::pair<long long, long long>> range; ///< [left:right], absent for scalars
};

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
                p.output = lower(e.value("direction", std::string("in"))) == "out";
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
};

std::vector<SimplePort> boundaryPorts(const XciInfo& info) {
    std::vector<SimplePort> ports;
    for (const auto& b : info.ports)
        ports.push_back({b.name, b.output,
                         b.range ? static_cast<unsigned>(std::llabs(b.range->first - b.range->second) + 1) : 1U,
                         b.range.has_value()});
    return ports;
}

void emitHeader(std::ostringstream& v, const std::string& module, const std::vector<SimplePort>& ports) {
    v << "// Generated by OpenCDC from a Vivado IP configuration (.xci): a CDC model, not a functional model.\n"
      << "module " << module << " (\n";
    for (std::size_t i = 0; i < ports.size(); ++i)
        v << "  " << (ports[i].output ? "output wire " : "input  wire ")
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
                               info.version + "): its ports are only modelled from a JSON .xci (Vivado 2020.2+)",
                           {"add the IP's generated HDL to the sources instead, or set the project option "
                            "unknownModules to blackbox"});
    }
    // the clock: aclk / clk / CLK
    const SimplePort* clk = nullptr;
    for (const char* c : {"aclk", "clk", "CLK", "ACLK", "clka"})
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

} // namespace

bool isXciFile(const std::filesystem::path& file) {
    return lower(file.extension().string()) == ".xci";
}

Result<XciModel> modelFromXciText(std::string_view text) {
    std::string t(text);
    const auto first = t.find_first_not_of(" \t\r\n");
    auto parsed = first != std::string::npos && t[first] == '{' ? parseJson(t) : parseXml(t);
    if (!parsed)
        return std::move(parsed).error();
    const XciInfo& info = parsed.value();
    if (info.name == "fifo_generator")
        return fifoGenerator(info);
    if (info.name == "axis_data_fifo")
        return axisDataFifo(info);
    if (info.name == "clk_wiz")
        return clockingWizard(info);
    if (info.name == "blk_mem_gen")
        return blockMemory(info);
    static const std::set<std::string> kOneClock = {"cordic",      "floating_point", "div_gen",  "mult_gen",
                                                    "c_addsub",    "dds_compiler",   "fir_compiler", "cmpy",
                                                    "c_shift_ram", "c_counter_binary", "xbip_dsp48_macro",
                                                    "c_accum",     "cic_compiler"};
    if (kOneClock.contains(info.name))
        return oneClockIp(info);
    if (info.name == "ila" || info.name == "vio")
        return debugCore(info, info.name == "vio");
    return unsupported("IP '" + info.instance + "' (" + info.vendor + ":" + info.library + ":" + info.name + ":" +
                           info.version + ") is not modelled from its .xci",
                       {"add the IP's generated HDL to the sources instead (Vivado: <ip>/synth/" + info.instance +
                            ".v or .vhd, or the simulation netlist), or set the project option unknownModules to "
                            "blackbox"});
}

Result<XciModel> modelFromXci(const std::filesystem::path& file) {
    auto text = readSourceText(file);
    if (!text)
        return Diagnostic{"FE-XCI-PARSE", Severity::Warning, "cannot read IP configuration file",
                          SourceLocation{toUtf8(file), 0, 0}, {}};
    auto r = modelFromXciText(text->utf8);
    if (!r) {
        Diagnostic d = std::move(r).error();
        d.location.file = toUtf8(file);
        return d;
    }
    return r;
}

} // namespace opencdc::frontend

#include "ir/Dump.h"

#include <sstream>

namespace opencdc::ir {

namespace {

std::string rangeText(const Net& n) {
    if (n.arrayDepth > 0)
        return "[" + std::to_string(n.elemWidth) + " x " + std::to_string(n.arrayDepth) + "]";
    if (n.width == 1 && n.msb == n.lsb)
        return {};
    return "[" + std::to_string(n.msb) + ":" + std::to_string(n.lsb) + "]";
}

std::string chunkText(const ModuleBody& body, const SigChunk& c) {
    if (c.isConst())
        return c.constant.toString();
    const Net& n = body.net(c.net);
    if (c.offset == 0 && c.width == n.width)
        return n.name;
    if (n.arrayDepth > 0) // flattened memory: show raw bit offsets
        return n.name + "{" + std::to_string(c.offset) + "+:" + std::to_string(c.width) + "}";
    if (c.width == 1)
        return n.name + "[" + std::to_string(n.indexOf(c.offset)) + "]";
    return n.name + "[" + std::to_string(n.indexOf(c.offset + c.width - 1)) + ":" +
           std::to_string(n.indexOf(c.offset)) + "]";
}

std::string resetText(const ModuleBody& body, const ResetSpec& r) {
    return std::string(r.activeHigh ? "" : "!") + formatSig(body, r.signal) + " -> " + formatSig(body, r.value);
}

std::string inputName(CellKind k, std::size_t i) {
    switch (k) {
        case CellKind::Mux: return i == 0 ? "A" : i == 1 ? "B" : "S";
        case CellKind::DynSelect: return i == 0 ? "A" : "S";
        case CellKind::DynInsert: return i == 0 ? "A" : i == 1 ? "S" : "D";
        case CellKind::Opaque: return "I" + std::to_string(i);
        default: return i == 0 ? "A" : "B";
    }
}

} // namespace

std::string formatSig(const ModuleBody& body, const SigSpec& sig) {
    if (sig.empty())
        return "<none>";
    const auto& chunks = sig.chunks();
    if (chunks.size() == 1)
        return chunkText(body, chunks.front());
    std::string out = "{";
    for (std::size_t i = chunks.size(); i-- > 0;) { // MSB first, like Verilog
        out += chunkText(body, chunks[i]);
        if (i != 0)
            out += ", ";
    }
    out += "}";
    return out;
}

std::string dumpBody(const ModuleBody& b) {
    std::ostringstream os;
    os << "module " << b.name << b.params << "\n";

    for (const auto& n : b.nets) {
        if (n.kind == NetKind::Temp)
            continue; // temps are printed with their cells
        os << "  ";
        if (n.dir != PortDir::None)
            os << toString(n.dir) << ' ';
        os << toString(n.kind);
        if (n.isSigned)
            os << " signed";
        if (auto r = rangeText(n); !r.empty())
            os << ' ' << r;
        os << ' ' << n.name;
        for (const auto& a : n.attrs)
            os << " (* " << a.name << (a.value.empty() ? "" : " = " + a.value) << " *)";
        if (n.init)
            os << " init=" << n.init->toString();
        os << "\n";
    }

    for (const auto& c : b.cells) {
        os << "  cell " << b.net(c.output).name << " [" << c.width << "] = " << toString(c.kind);
        if (c.isSigned)
            os << ".s";
        if (c.kind == CellKind::DynSelect || c.kind == CellKind::DynInsert)
            os << "<" << c.elemWidth << "@" << c.rangeLeft << ":" << c.rangeRight << ">";
        os << "(";
        for (std::size_t i = 0; i < c.inputs.size(); ++i) {
            if (i)
                os << ", ";
            os << inputName(c.kind, i) << "=" << formatSig(b, c.inputs[i]);
        }
        os << ")";
        if (!c.label.empty())
            os << " \"" << c.label << "\"";
        os << "\n";
    }

    for (const auto& c : b.connections) {
        os << "  " << (c.kind == ConnKind::Process ? "comb  " : c.kind == ConnKind::PortExpr ? "portx " : "assign ")
           << formatSig(b, c.lhs) << " = " << formatSig(b, c.rhs) << "\n";
    }

    for (const auto& r : b.registers) {
        os << "  reg " << formatSig(b, r.q) << " <= " << formatSig(b, r.d) << " @("
           << (r.clock.edge == Edge::Pos ? "posedge " : "negedge ") << formatSig(b, r.clock.signal) << ")";
        for (const auto& ar : r.asyncResets)
            os << " arst(" << resetText(b, ar) << ")";
        if (r.syncReset)
            os << " srst(" << resetText(b, *r.syncReset) << ")";
        if (r.enable)
            os << " en(" << formatSig(b, *r.enable) << ")";
        os << "\n";
    }

    for (const auto& l : b.latches)
        os << "  latch " << formatSig(b, l.q) << " <= " << formatSig(b, l.d) << "\n";

    for (const auto& ch : b.children) {
        os << "  inst " << ch.name << " : " << ch.definition << (ch.body.valid() ? "" : " <unresolved>") << " (";
        for (std::size_t i = 0; i < ch.ports.size(); ++i) {
            const auto& p = ch.ports[i];
            if (i)
                os << ", ";
            os << "." << p.port << "(" << (p.actual.empty() ? "" : formatSig(b, p.actual)) << ")";
        }
        os << ")\n";
    }
    os << "endmodule\n";
    return os.str();
}

std::string dumpDesign(const Design& d) {
    std::ostringstream os;
    for (const auto& b : d.bodies)
        os << dumpBody(b) << "\n";

    os << "hierarchy\n";
    for (const auto& inst : d.instances) {
        std::size_t depth = 0;
        for (auto p = inst.parent; p.valid(); p = d.instance(p).parent)
            ++depth;
        os << std::string(2 + depth * 2, ' ') << inst.name << " (" << d.body(inst.body).name << ")\n";
    }

    if (!d.unsupported.empty()) {
        os << "unsupported\n";
        for (const auto& u : d.unsupported)
            os << "  " << u.code << " in " << u.module << ": " << u.what << "\n";
    }
    return os.str();
}

} // namespace opencdc::ir

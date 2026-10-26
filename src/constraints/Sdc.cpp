// SDC/XDC subset: a small Tcl-like interpreter plus design object resolution.
//
// Supported Tcl: commands separated by newlines or ';', '#' comments,
// backslash-newline continuation, "quoted" and {braced} words, [command]
// substitution, $var / ${var}, set, expr, if/elseif/else, foreach, for,
// while, break, continue, incr, llength, lindex, lappend, puts and
// get_property (PERIOD of clocks, cell properties). proc, source, switch
// and eval are not supported and are reported. Only commands that matter for
// clock and CDC analysis are executed; every other command is counted and
// reported.

#include "constraints/Sdc.h"

#include "core/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <cmath>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace opencdc::constraints {

using graph::BitRef;

// ============================================================================
// Glob matching
// ============================================================================

bool globMatch(std::string_view p, std::string_view t, bool nocase) {
    auto eq = [nocase](char a, char b) {
        return nocase ? std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b))
                      : a == b;
    };
    std::size_t pi = 0;
    std::size_t ti = 0;
    std::size_t starP = std::string_view::npos;
    std::size_t starT = 0;
    while (ti < t.size()) {
        if (pi < p.size() && p[pi] == '*') {
            starP = pi++;
            starT = ti;
            continue;
        }
        if (pi < p.size()) {
            char pc = p[pi];
            std::size_t adv = 1;
            if (pc == '\\' && pi + 1 < p.size()) {
                pc = p[pi + 1];
                adv = 2;
                if (eq(pc, t[ti])) {
                    pi += adv;
                    ++ti;
                    continue;
                }
            }
            else if (pc == '?' || eq(pc, t[ti])) {
                pi += adv;
                ++ti;
                continue;
            }
        }
        if (starP != std::string_view::npos) {
            pi = starP + 1;
            ti = ++starT;
            continue;
        }
        return false;
    }
    while (pi < p.size() && p[pi] == '*')
        ++pi;
    return pi == p.size();
}

namespace {

/// Splits "base[idx]" (unescaped brackets) at the last '['.
std::optional<std::pair<std::string, std::string>> splitIndex(std::string_view pattern) {
    if (pattern.size() < 3 || pattern.back() != ']')
        return std::nullopt;
    // Find the matching '[' of the trailing ']' (allow escaped brackets).
    std::size_t open = std::string_view::npos;
    for (std::size_t i = pattern.size() - 1; i-- > 0;) {
        if (pattern[i] == '[') {
            open = i;
            break;
        }
    }
    if (open == std::string_view::npos || open == 0)
        return std::nullopt;
    std::size_t baseEnd = open;
    std::size_t idxEnd = pattern.size() - 1;
    if (pattern[open - 1] == '\\')
        --baseEnd;
    if (pattern[idxEnd - 1] == '\\')
        --idxEnd;
    return std::make_pair(std::string(pattern.substr(0, baseEnd)), std::string(pattern.substr(open + 1, idxEnd - open - 1)));
}

} // namespace

// ============================================================================
// Design object resolver
// ============================================================================

DesignObjectResolver::DesignObjectResolver(const graph::DesignView& view) : view_(view) {}

namespace {

/// Vivado leaves unnamed generate blocks out of names ("oddr[0].oddr_inst"
/// for "genblk1.oddr[0].genblk1.oddr_inst"): the name without them, or empty
/// when there are none.
std::string withoutGenblk(std::string_view name) {
    if (name.find("genblk") == std::string_view::npos)
        return {};
    std::string out;
    std::size_t i = 0;
    while (i < name.size()) {
        const bool atStart = i == 0 || name[i - 1] == '.' || name[i - 1] == '/' || name[i - 1] == '|';
        if (atStart && name.substr(i).starts_with("genblk")) {
            std::size_t j = i + 6;
            while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j])))
                ++j;
            if (j > i + 6 && j < name.size() && name[j] == '.') {
                i = j + 1;
                continue;
            }
        }
        out += name[i++];
    }
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> instancePrefixes(const ir::Design& d) {
    // Relative instance path prefixes ("" for tops, "u_rx/" ...).
    std::vector<std::string> prefix(d.instances.size());
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const auto& node = d.instances[i];
        if (node.parent.valid())
            prefix[i] = prefix[node.parent.index()] + node.name + "/";
    }
    return prefix;
}

} // namespace

void DesignObjectResolver::buildNames() {
    if (namesBuilt_)
        return;
    namesBuilt_ = true;
    const auto& d = view_.design();
    const auto prefix = instancePrefixes(d);
    auto named = [](std::string n, BitRef first, const ir::Net& net) {
        Named x;
        x.name = std::move(n);
        x.first = first;
        x.width = net.width;
        x.msb = net.msb;
        x.lsb = net.lsb;
        x.indexable = net.width > 1 && net.arrayDepth == 0;
        return x;
    };
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const ir::InstanceId inst{static_cast<std::uint32_t>(i)};
        const auto& node = d.instances[i];
        const auto& body = d.body(node.body);
        for (const auto& p : body.ports) {
            if (!node.parent.valid())
                ports_.push_back(named(p.name, BitRef{inst, p.net, 0}, body.net(p.net)));
            else
                pins_.push_back(named(prefix[i] + p.name, BitRef{inst, p.net, 0}, body.net(p.net)));
        }
        for (std::size_t n = 0; n < body.nets.size(); ++n) {
            const auto& net = body.nets[n];
            if (net.kind != ir::NetKind::Temp)
                nets_.push_back(named(prefix[i] + net.name, BitRef{inst, ir::NetId{static_cast<std::uint32_t>(n)}, 0}, net));
        }
    }
}

void DesignObjectResolver::buildRegisters() {
    if (registersBuilt_)
        return;
    registersBuilt_ = true;
    const auto& d = view_.design();
    const auto prefix = instancePrefixes(d);
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const ir::InstanceId inst{static_cast<std::uint32_t>(i)};
        const auto& body = d.body(d.instances[i].body);
        for (const auto& r : body.registers) {
            const auto& ch = r.q.chunks().front();
            const auto& net = body.net(ch.net);
            auto clk = view_.bitOf(inst, r.clock.signal, 0);
            auto add = [&](Named x) {
                regs_.push_back(std::move(x));
                regClock_.push_back(clk ? *clk : BitRef{});
                CellInfo info;
                info.parent = prefix[i].empty() ? std::string() : prefix[i].substr(0, prefix[i].size() - 1);
                info.attrs = &net.attrs;
                info.reg = &r;
                info.inst = inst;
                info.qOffset = ch.offset;
                regInfo_.push_back(std::move(info));
                regsByNet_[(static_cast<std::uint64_t>(inst.value) << 32) | ch.net.value].push_back(regs_.size() - 1);
            };
            for (const auto& a : net.attrs)
                attrNames_.insert(upper(a.name));
            if (net.arrayDepth > 0) {
                // An array written at a computed index is a memory (RAM after
                // synthesis, no flip-flops). One written only at constant
                // indices (a pipeline such as m_axis_pipe_reg[j]) is made of
                // flip-flops named per element: "<net>_reg[<element>]"
                // (elements counted from 0 in storage order).
                bool memory = false;
                if (auto b = view_.bitOf(inst, r.d, 0)) {
                    const auto drv = view_.driver(*b);
                    if (drv.kind == graph::DriverKind::Cell) {
                        const auto& db = d.body(d.instances[drv.bit.inst.index()].body);
                        memory = drv.index < db.cells.size() && db.cells[drv.index].kind == ir::CellKind::DynInsert;
                    }
                }
                const std::uint32_t ew = net.elemWidth;
                if (memory || ew == 0)
                    continue;
                for (std::uint32_t e = ch.offset / ew; e * ew < ch.offset + ch.width; ++e) {
                    Named x;
                    x.name = prefix[i] + net.name + "_reg[" + std::to_string(e) + "]";
                    const std::uint32_t lo = std::max(e * ew, ch.offset);
                    const std::uint32_t hi = std::min((e + 1) * ew, ch.offset + ch.width);
                    x.first = BitRef{inst, ch.net, lo};
                    x.width = hi - lo;
                    x.lsb = static_cast<std::int32_t>(lo - e * ew);
                    x.msb = static_cast<std::int32_t>(hi - e * ew - 1);
                    x.indexable = true;
                    add(std::move(x));
                }
                continue;
            }
            Named x;
            x.name = prefix[i] + net.name + "_reg";
            x.first = BitRef{inst, ch.net, ch.offset};
            x.width = ch.width;
            x.msb = net.indexOf(ch.offset + ch.width - 1);
            x.lsb = net.indexOf(ch.offset);
            // a 1-bit register also answers to "<name>_reg[0]" (Vivado names
            // a [0:0] vector so; the IR does not record the declared range)
            x.indexable = true;
            add(std::move(x));
        }
    }
}

std::vector<Object> DesignObjectResolver::match(const std::vector<Named>& items, ObjectKind kind,
                                                std::string_view pattern, bool hierarchical, bool nocase) const {
    std::vector<Object> out;
    const auto split = splitIndex(pattern);
    std::string stripped;
    auto candidates = [&](const std::string& name) {
        std::vector<std::string_view> c{name};
        stripped = withoutGenblk(name);
        if (!stripped.empty())
            c.push_back(stripped);
        if (hierarchical)
            for (const std::string_view n : std::vector<std::string_view>(c))
                for (std::size_t i = 0; i < n.size(); ++i)
                    if (n[i] == '/')
                        c.push_back(n.substr(i + 1));
        return c;
    };
    for (const auto& it : items) {
        Object o;
        o.kind = kind;
        o.item = static_cast<std::int32_t>(&it - items.data());
        for (auto cand : candidates(it.name)) {
            if (globMatch(pattern, cand, nocase)) {
                o.name = it.name;
                if (!it.explicitBits.empty())
                    o.bits = it.explicitBits;
                else
                    for (std::uint32_t b = 0; b < it.width; ++b)
                        o.bits.push_back(BitRef{it.first.inst, it.first.net, it.first.bit + b});
                break;
            }
            if (split && it.indexable && globMatch(split->first, cand, nocase)) {
                for (std::uint32_t b = 0; b < it.width; ++b) {
                    const std::int32_t index = it.msb >= it.lsb ? it.lsb + static_cast<std::int32_t>(b)
                                                                : it.lsb - static_cast<std::int32_t>(b);
                    if (globMatch(split->second, std::to_string(index), nocase))
                        o.bits.push_back(BitRef{it.first.inst, it.first.net, it.first.bit + b});
                }
                if (!o.bits.empty()) {
                    o.name = it.name + "[" + split->second + "]";
                    break;
                }
            }
        }
        if (!o.bits.empty())
            out.push_back(std::move(o));
    }
    return out;
}

std::vector<Object> DesignObjectResolver::ports(std::string_view pattern, bool nocase) {
    buildNames();
    return match(ports_, ObjectKind::Port, pattern, false, nocase);
}

std::vector<Object> DesignObjectResolver::pins(std::string_view pattern, bool hierarchical, bool nocase) {
    buildNames();
    auto out = match(pins_, ObjectKind::Pin, pattern, hierarchical, nocase);
    // Synthesized flip-flop pins: <reg>_reg[/bit]/Q and /C (Vivado naming).
    const auto slash = pattern.rfind('/');
    if (slash == std::string_view::npos)
        return out;
    const auto pin = pattern.substr(slash + 1);
    const bool q = globMatch(pin, "Q", nocase);
    const bool c = globMatch(pin, "C", nocase);
    if (!q && !c)
        return out;
    buildRegisters();
    const auto regPattern = pattern.substr(0, slash);
    for (auto o : match(regs_, ObjectKind::Pin, regPattern, hierarchical, nocase)) {
        if (q) {
            Object qo = o;
            qo.name += "/Q";
            qo.pin = "Q";
            out.push_back(std::move(qo));
        }
        if (c) {
            // Map every matched Q bit to the clock of its register.
            Object co;
            co.kind = ObjectKind::Pin;
            co.name = o.name + "/C";
            co.item = o.item;
            co.pin = "C";
            for (const auto& b : o.bits) {
                auto it = regsByNet_.find((static_cast<std::uint64_t>(b.inst.value) << 32) | b.net.value);
                if (it == regsByNet_.end())
                    continue;
                for (const std::size_t r : it->second) {
                    const auto& reg = regs_[r];
                    if (reg.first.inst == b.inst && reg.first.net == b.net && b.bit >= reg.first.bit &&
                        b.bit < reg.first.bit + reg.width && regClock_[r].valid()) {
                        if (std::ranges::find(co.bits, regClock_[r]) == co.bits.end())
                            co.bits.push_back(regClock_[r]);
                        break;
                    }
                }
            }
            if (!co.bits.empty())
                out.push_back(std::move(co));
        }
    }
    return out;
}

void DesignObjectResolver::buildInstances() {
    if (instancesBuilt_)
        return;
    instancesBuilt_ = true;
    const auto& d = view_.design();
    const auto prefix = instancePrefixes(d);
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const auto& node = d.instances[i];
        if (!node.parent.valid())
            continue;
        insts_.push_back(prefix[i].substr(0, prefix[i].size() - 1));
        CellInfo info;
        const auto& pp = prefix[node.parent.index()];
        info.parent = pp.empty() ? std::string() : pp.substr(0, pp.size() - 1);
        info.ref = d.body(node.body).name;
        const auto& parentBody = d.body(d.instances[node.parent.index()].body);
        if (node.childIndex < parentBody.children.size()) {
            info.attrs = &parentBody.children[node.childIndex].attrs;
            for (const auto& a : *info.attrs)
                attrNames_.insert(upper(a.name));
        }
        instInfo_.push_back(std::move(info));
    }
}

std::vector<Object> DesignObjectResolver::cells(std::string_view pattern, bool hierarchical, bool nocase) {
    buildRegisters();
    buildInstances();
    // Without -hierarchical a pattern names cells at the level it spells out:
    // "u_*" is a top-level cell, "u_s0/*" a cell directly inside u_s0 ('*'
    // does not cross hierarchy levels)
    const auto levels = [](std::string_view n) { return std::ranges::count(n, '/'); };
    auto out = match(regs_, ObjectKind::Cell, pattern, hierarchical, nocase);
    if (!hierarchical)
        std::erase_if(out, [&](const Object& o) { return levels(regs_[static_cast<std::size_t>(o.item)].name) != levels(pattern); });
    for (std::size_t i = 0; i < insts_.size(); ++i) {
        bool hit = false;
        const std::string alias = withoutGenblk(insts_[i]);
        for (const std::string& name : {insts_[i], alias}) {
            if (name.empty() || hit)
                continue;
            hit = globMatch(pattern, name, nocase) && (hierarchical || levels(name) == levels(pattern));
            for (std::size_t k = 0; !hit && hierarchical && k < name.size(); ++k)
                hit = name[k] == '/' && globMatch(pattern, std::string_view(name).substr(k + 1), nocase);
        }
        const std::string& name = insts_[i];
        if (hit)
            out.push_back({ObjectKind::Cell, name, {}, static_cast<std::int32_t>(i), true});
    }
    return out;
}

std::vector<Object> DesignObjectResolver::cellsRegexp(std::string_view regex, bool hierarchical, bool nocase) {
    buildRegisters();
    buildInstances();
    auto flags = std::regex::ECMAScript;
    if (nocase)
        flags |= std::regex::icase;
    const std::regex re{std::string(regex), flags};
    auto hitsOne = [&](const std::string& name) {
        if (std::regex_match(name, re))
            return true;
        if (hierarchical)
            for (std::size_t k = 0; k < name.size(); ++k)
                if (name[k] == '/' && std::regex_match(name.begin() + static_cast<std::ptrdiff_t>(k) + 1, name.end(), re))
                    return true;
        return false;
    };
    auto hits = [&](const std::string& name) {
        if (hitsOne(name))
            return true;
        const std::string alias = withoutGenblk(name);
        return !alias.empty() && hitsOne(alias);
    };
    std::vector<Object> out;
    for (std::size_t r = 0; r < regs_.size(); ++r) {
        const auto& it = regs_[r];
        Object o{ObjectKind::Cell, it.name, {}, static_cast<std::int32_t>(r), false};
        if (hits(it.name)) {
            for (std::uint32_t b = 0; b < it.width; ++b)
                o.bits.push_back(BitRef{it.first.inst, it.first.net, it.first.bit + b});
        }
        else if (it.indexable) {
            std::string last;
            for (std::uint32_t b = 0; b < it.width; ++b) {
                const std::int32_t index = it.msb >= it.lsb ? it.lsb + static_cast<std::int32_t>(b)
                                                            : it.lsb - static_cast<std::int32_t>(b);
                std::string bitName = it.name + "[" + std::to_string(index) + "]";
                if (hits(bitName)) {
                    o.bits.push_back(BitRef{it.first.inst, it.first.net, it.first.bit + b});
                    last = std::move(bitName);
                }
            }
            if (o.bits.size() == 1)
                o.name = last;
        }
        if (!o.bits.empty())
            out.push_back(std::move(o));
    }
    for (std::size_t i = 0; i < insts_.size(); ++i)
        if (hits(insts_[i]))
            out.push_back({ObjectKind::Cell, insts_[i], {}, static_cast<std::int32_t>(i), true});
    return out;
}

bool DesignObjectResolver::cellPropertyKnown(std::string_view prop) {
    buildRegisters();
    buildInstances();
    const std::string p = upper(prop);
    return p == "NAME" || p == "PARENT" || p == "REF_NAME" || p == "ORIG_REF_NAME" || p == "IS_SEQUENTIAL" ||
           attrNames_.contains(p);
}

std::optional<std::string> DesignObjectResolver::cellProperty(const Object& cell, std::string_view prop) {
    if (cell.kind != ObjectKind::Cell || cell.item < 0)
        return std::nullopt;
    buildRegisters();
    buildInstances();
    const auto idx = static_cast<std::size_t>(cell.item);
    if (cell.instance ? idx >= instInfo_.size() : idx >= regInfo_.size())
        return std::nullopt;
    const CellInfo& info = cell.instance ? instInfo_[idx] : regInfo_[idx];
    const std::string p = upper(prop);
    if (p == "NAME")
        return cell.name;
    if (p == "PARENT")
        return info.parent;
    if (p == "REF_NAME" || p == "ORIG_REF_NAME") {
        if (!cell.instance)
            return std::nullopt; // the flip-flop primitive is chosen by synthesis
        return info.ref;
    }
    if (p == "IS_SEQUENTIAL")
        return cell.instance ? "0" : "1";
    if (info.attrs)
        for (const auto& a : *info.attrs)
            if (upper(a.name) == p) {
                std::string v = a.value;
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                    v = v.substr(1, v.size() - 2);
                return v.empty() ? std::string("1") : v;
            }
    return std::nullopt;
}

std::vector<BitRef> DesignObjectResolver::cellClocks(const Object& cell) {
    if (cell.kind != ObjectKind::Cell || cell.instance || cell.item < 0)
        return {};
    buildRegisters();
    const auto idx = static_cast<std::size_t>(cell.item);
    if (idx >= regClock_.size() || !regClock_[idx].valid())
        return {};
    return {regClock_[idx]};
}


std::string DesignObjectResolver::bitCellName(std::size_t r, const BitRef& q) const {
    const auto& reg = regs_[r];
    if (reg.width == 1 && reg.msb == reg.lsb && reg.lsb == 0 && reg.name.back() != ']')
        return reg.name;
    const auto k = static_cast<std::int32_t>(q.bit - reg.first.bit);
    const std::int32_t index = reg.msb >= reg.lsb ? reg.lsb + k : reg.lsb - k;
    return reg.name + "[" + std::to_string(index) + "]";
}

std::vector<Object> DesignObjectResolver::registerPins(std::size_t r, const std::string& cellName,
                                                       const std::vector<BitRef>& qbits) {
    std::vector<Object> out;
    const auto& info = regInfo_[r];
    if (!info.reg)
        return out;
    const ir::Register& ff = *info.reg;
    auto add = [&](const std::string& pin, std::vector<BitRef> bits) {
        if (!bits.empty())
            out.push_back({ObjectKind::Pin, cellName + "/" + pin, std::move(bits), static_cast<std::int32_t>(r), false, pin});
    };
    auto first = [&](const ir::SigSpec& s) {
        std::vector<BitRef> v;
        if (auto b = view_.bitOf(info.inst, s, 0))
            v.push_back(*b);
        return v;
    };
    // the reset value of the first bit of the cell decides PRE/CLR (S/R)
    const std::uint32_t k0 = qbits.empty() ? 0 : qbits.front().bit - info.qOffset;
    auto setsOne = [&](const ir::ResetSpec& rs) {
        if (k0 >= rs.value.width())
            return false;
        const auto c = rs.value.bit(k0).asConst();
        return c && c->width() == 1 && (*c)[0] == ir::Logic::L1;
    };
    add("C", regClock_[r].valid() ? std::vector<BitRef>{regClock_[r]} : std::vector<BitRef>{});
    std::vector<BitRef> d;
    for (const auto& q : qbits)
        if (auto b = view_.bitOf(info.inst, ff.d, q.bit - info.qOffset))
            d.push_back(*b);
    add("D", std::move(d));
    add("Q", qbits);
    if (ff.enable)
        add("CE", first(*ff.enable));
    for (const auto& rs : ff.asyncResets)
        add(setsOne(rs) ? "PRE" : "CLR", first(rs.signal));
    if (ff.syncReset)
        add(setsOne(*ff.syncReset) ? "S" : "R", first(ff.syncReset->signal));
    return out;
}

std::vector<Object> DesignObjectResolver::cellPins(const Object& cell) {
    std::vector<Object> out;
    if (cell.kind != ObjectKind::Cell || cell.item < 0)
        return out;
    buildNames();
    buildRegisters();
    buildInstances();
    const auto idx = static_cast<std::size_t>(cell.item);
    if (cell.instance) {
        if (idx >= insts_.size())
            return out;
        const std::string prefix = insts_[idx] + "/";
        for (std::size_t p = 0; p < pins_.size(); ++p) {
            const auto& n = pins_[p];
            if (!n.name.starts_with(prefix) || n.name.find('/', prefix.size()) != std::string::npos)
                continue;
            Object o{ObjectKind::Pin, n.name, {}, static_cast<std::int32_t>(p), false, {}};
            for (std::uint32_t b = 0; b < n.width; ++b)
                o.bits.push_back(BitRef{n.first.inst, n.first.net, n.first.bit + b});
            out.push_back(std::move(o));
        }
        return out;
    }
    if (idx >= regs_.size())
        return out;
    return registerPins(idx, cell.name, cell.bits);
}

std::vector<Object> DesignObjectResolver::pinNets(const Object& pin) {
    if (pin.kind != ObjectKind::Pin || pin.bits.empty())
        return {};
    return {{ObjectKind::Net, view_.name(pin.bits.front()), pin.bits, -1, false, {}}};
}

namespace {
std::tuple<int, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t> driverKey(const graph::Driver& d) {
    return {static_cast<int>(d.kind), d.bit.inst.value, d.bit.net.value, d.bit.bit, d.index};
}
} // namespace

void DesignObjectResolver::buildLoads() {
    if (loadsBuilt_)
        return;
    loadsBuilt_ = true;
    buildRegisters();
    for (std::size_t r = 0; r < regs_.size(); ++r) {
        const auto& reg = regs_[r];
        std::vector<BitRef> q;
        for (std::uint32_t b = 0; b < reg.width; ++b)
            q.push_back(BitRef{reg.first.inst, reg.first.net, reg.first.bit + b});
        // per flip-flop bit: its input pins
        for (const auto& qb : q)
            for (const auto& p : registerPins(r, reg.name, {qb})) {
                if (p.pin == "Q")
                    continue;
                for (const auto& b : p.bits) {
                    const auto d = view_.driver(b);
                    if (d.kind == graph::DriverKind::Undriven || d.kind == graph::DriverKind::Constant)
                        continue;
                    loads_[driverKey(d)].emplace_back(r, p.pin, qb);
                }
            }
    }
}

std::vector<Object> DesignObjectResolver::netPins(const Object& net) {
    std::vector<Object> out;
    if (net.kind != ObjectKind::Net)
        return out;
    buildRegisters();
    buildLoads();
    // one object per (register bit, pin)
    auto add = [&](std::size_t r, const std::string& pin, const BitRef& q, const BitRef& bit) {
        const std::string name = bitCellName(r, q) + "/" + pin;
        auto it = std::ranges::find(out, name, &Object::name);
        if (it == out.end())
            out.push_back({ObjectKind::Pin, name, {bit}, static_cast<std::int32_t>(r), false, pin});
        else if (std::ranges::find(it->bits, bit) == it->bits.end())
            it->bits.push_back(bit);
    };
    for (const auto& b : net.bits) {
        const auto d = view_.driver(b);
        if (d.kind == graph::DriverKind::Register) {
            const auto key = (static_cast<std::uint64_t>(d.bit.inst.value) << 32) | d.bit.net.value;
            if (auto it = regsByNet_.find(key); it != regsByNet_.end())
                for (const std::size_t r : it->second) {
                    const auto& reg = regs_[r];
                    if (d.bit.bit >= reg.first.bit && d.bit.bit < reg.first.bit + reg.width)
                        add(r, "Q", d.bit, d.bit);
                }
        }
        if (d.kind == graph::DriverKind::Undriven || d.kind == graph::DriverKind::Constant)
            continue;
        if (auto it = loads_.find(driverKey(d)); it != loads_.end())
            for (const auto& [r, pin, q] : it->second)
                add(r, pin, q, b);
    }
    return out;
}

std::vector<Object> DesignObjectResolver::pinCells(const Object& pin) {
    if (pin.kind != ObjectKind::Pin)
        return {};
    const auto slash = pin.name.rfind('/');
    const std::string cellName = slash == std::string::npos ? std::string() : pin.name.substr(0, slash);
    buildRegisters();
    buildInstances();
    if (pin.pin.empty()) {
        // a port of an instance (by its table entry: the pin may carry a Quartus name)
        buildNames();
        const std::string inst = pin.item >= 0 && static_cast<std::size_t>(pin.item) < pins_.size()
                                     ? pins_[static_cast<std::size_t>(pin.item)].name.substr(
                                           0, pins_[static_cast<std::size_t>(pin.item)].name.rfind('/'))
                                     : cellName;
        for (std::size_t i = 0; i < insts_.size(); ++i)
            if (insts_[i] == inst)
                return {{ObjectKind::Cell, inst, {}, static_cast<std::int32_t>(i), true, {}}};
        return {};
    }
    // a flip-flop pin: the cell with the same name ("x_reg[3]" or "x_reg")
    if (!cellName.empty())
        for (auto& c : cells(cellName, false, false))
            if (!c.instance && c.item == pin.item)
                return {std::move(c)};
    // otherwise (a Quartus name): the register of the pin, restricted to the
    // pin's bits when it is the output
    if (pin.item < 0 || static_cast<std::size_t>(pin.item) >= regs_.size())
        return {};
    const auto& reg = regs_[static_cast<std::size_t>(pin.item)];
    Object c{ObjectKind::Cell, reg.name, {}, pin.item, false, {}};
    if (pin.pin == "Q")
        c.bits = pin.bits;
    else
        for (std::uint32_t b = 0; b < reg.width; ++b)
            c.bits.push_back(BitRef{reg.first.inst, reg.first.net, reg.first.bit + b});
    return {std::move(c)};
}


void DesignObjectResolver::buildQuartus() {
    if (quartusBuilt_)
        return;
    quartusBuilt_ = true;
    buildNames();
    buildRegisters();
    buildInstances();
    auto bars = [](std::string s) {
        std::ranges::replace(s, '/', '|');
        return s;
    };
    for (const auto& r : regs_) {
        // "<prefix><net>_reg" / "<prefix><net>_reg[<element>]" -> "<prefix|><net>" / "<prefix|><net>[<element>]"
        std::string n = r.name;
        if (n.ends_with("_reg"))
            n.erase(n.size() - 4);
        else if (const auto at = n.rfind("_reg["); at != std::string::npos)
            n.erase(at, 4);
        Named q = r;
        q.name = bars(std::move(n));
        qregs_.push_back(std::move(q));
    }
    for (const auto& p : pins_) {
        Named q = p;
        q.name = bars(p.name);
        qpins_.push_back(std::move(q));
    }
    for (const auto& i : insts_)
        qinsts_.push_back(bars(i));
    // full names: every instance level as "<definition>:<instance>"
    std::map<std::string, std::string, std::less<>> fullOf; // "a|b" -> "da:a|db:b"
    std::map<std::string, std::size_t, std::less<>> indexOf;
    for (std::size_t i = 0; i < qinsts_.size(); ++i)
        indexOf[qinsts_[i]] = i;
    std::function<std::string(const std::string&)> fullInst = [&](const std::string& q) -> std::string {
        if (auto it = fullOf.find(q); it != fullOf.end())
            return it->second;
        const auto bar = q.rfind('|');
        const std::string leaf = bar == std::string::npos ? q : q.substr(bar + 1);
        std::string parent = bar == std::string::npos ? std::string() : fullInst(q.substr(0, bar));
        const auto idx = indexOf.find(q);
        std::string f = (parent.empty() ? std::string() : parent + "|") +
                        (idx == indexOf.end() ? leaf : instInfo_[idx->second].ref + ":" + leaf);
        fullOf[q] = f;
        return f;
    };
    for (const auto& q : qinsts_)
        fullInst(q);
    auto full = [&](const std::string& q) {
        const auto bar = q.rfind('|');
        if (bar == std::string::npos)
            return q;
        const auto it = fullOf.find(std::string_view(q).substr(0, bar));
        return it == fullOf.end() ? q : it->second + q.substr(bar);
    };
    for (const auto& r : qregs_) {
        Named f = r;
        f.name = full(r.name);
        qregsFull_.push_back(std::move(f));
    }
    for (const auto& p : qpins_) {
        Named f = p;
        f.name = full(p.name);
        qpinsFull_.push_back(std::move(f));
    }
    for (const auto& q : qinsts_)
        qinstsFull_.push_back(fullOf.contains(q) ? fullOf[q] : q);
}

std::vector<Object> DesignObjectResolver::quartus(std::string_view pattern, QuartusKind kind, bool nocase) {
    buildQuartus();
    // "entity:instance" levels in the pattern: match the full names
    const bool fullNames = pattern.find(':') != std::string_view::npos;
    const auto& qregs = fullNames ? qregsFull_ : qregs_;
    const auto& qpins = fullNames ? qpinsFull_ : qpins_;
    const auto& qinsts = fullNames ? qinstsFull_ : qinsts_;
    std::vector<Object> out;
    auto append = [&](std::vector<Object> r) {
        for (auto& o : r)
            out.push_back(std::move(o));
    };
    if (kind == QuartusKind::Registers || kind == QuartusKind::Keepers || kind == QuartusKind::Nodes ||
        kind == QuartusKind::Cells)
        append(match(qregs, ObjectKind::Cell, pattern, false, nocase));
    if (kind == QuartusKind::Keepers || kind == QuartusKind::Nodes)
        append(match(ports_, ObjectKind::Port, pattern, false, nocase));
    if (kind == QuartusKind::Cells)
        for (std::size_t i = 0; i < qinsts.size(); ++i)
            if (globMatch(pattern, qinsts[i], nocase))
                out.push_back({ObjectKind::Cell, qinsts[i], {}, static_cast<std::int32_t>(i), true, {}});
    if (kind != QuartusKind::Pins)
        return out;

    append(match(qpins, ObjectKind::Pin, pattern, false, nocase));
    // register pins: <register>|clk d q ena clrn prn sclr
    const auto bar = pattern.rfind('|');
    if (bar != std::string_view::npos) {
        static const std::map<std::string, std::string, std::less<>> pinNames = {
            {"clk", "C"}, {"d", "D"}, {"q", "Q"}, {"ena", "CE"}, {"clrn", "CLR"}, {"prn", "PRE"}, {"sclr", "R"}};
        const auto leaf = pattern.substr(bar + 1);
        for (const auto& [qp, ours] : pinNames) {
            if (!globMatch(leaf, qp, true))
                continue;
            for (const auto& cell : match(qregs, ObjectKind::Cell, pattern.substr(0, bar), false, nocase))
                for (auto& p : registerPins(static_cast<std::size_t>(cell.item), cell.name, cell.bits))
                    if (p.pin == ours) {
                        p.name = cell.name + "|" + qp;
                        out.push_back(std::move(p));
                    }
        }
    }
    if (!out.empty() || pattern.find_first_of("*?") != std::string_view::npos)
        return out;
    // a path into an instance whose internals are not modelled: the port of
    // the deepest instance on the path, named by the last segment
    std::vector<std::string_view> segs;
    for (std::size_t from = 0;;) {
        const auto to = pattern.find('|', from);
        segs.push_back(pattern.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from));
        if (to == std::string_view::npos)
            break;
        from = to + 1;
    }
    if (segs.size() < 3)
        return out;
    std::string prefix;
    std::size_t deepest = static_cast<std::size_t>(-1);
    std::size_t depth = 0;
    for (std::size_t k = 0; k + 1 < segs.size(); ++k) {
        prefix += (k ? "|" : "") + std::string(segs[k]);
        const auto it = std::ranges::find(qinsts, prefix);
        if (it == qinsts.end())
            break;
        deepest = static_cast<std::size_t>(it - qinsts.begin());
        depth = k + 1;
    }
    if (deepest == static_cast<std::size_t>(-1) || depth + 1 >= segs.size())
        return out; // nothing skipped: an ordinary miss
    std::string port = qinsts[deepest] + "|" + std::string(segs.back());
    return match(qpins, ObjectKind::Pin, port, false, nocase);
}

bool DesignObjectResolver::pinPropertyKnown(std::string_view prop) {
    static const std::set<std::string, std::less<>> known = {
        "NAME", "REF_PIN_NAME", "DIRECTION", "IS_LEAF", "IS_CLOCK", "IS_ENABLE", "IS_PRESET", "IS_CLEAR", "IS_SET",
        "IS_RESET"};
    return known.contains(upper(prop));
}

std::optional<std::string> DesignObjectResolver::pinProperty(const Object& pin, std::string_view prop) {
    if (pin.kind != ObjectKind::Pin)
        return std::nullopt;
    const std::string p = upper(prop);
    const bool leaf = !pin.pin.empty();
    if (p == "NAME")
        return pin.name;
    if (p == "REF_PIN_NAME")
        return leaf ? pin.pin : pin.name.substr(pin.name.rfind('/') + 1);
    if (p == "IS_LEAF")
        return leaf ? "1" : "0";
    if (!leaf)
        return std::nullopt; // instance ports: direction and pin kinds are not recorded
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };
    if (p == "DIRECTION")
        return pin.pin == "Q" ? "OUT" : "IN";
    if (p == "IS_CLOCK")
        return flag(pin.pin == "C");
    if (p == "IS_ENABLE")
        return flag(pin.pin == "CE");
    if (p == "IS_PRESET")
        return flag(pin.pin == "PRE");
    if (p == "IS_CLEAR")
        return flag(pin.pin == "CLR");
    if (p == "IS_SET")
        return flag(pin.pin == "S");
    if (p == "IS_RESET")
        return flag(pin.pin == "R");
    return std::nullopt;
}

std::vector<Object> DesignObjectResolver::nets(std::string_view pattern, bool hierarchical, bool nocase) {
    buildNames();
    return match(nets_, ObjectKind::Net, pattern, hierarchical, nocase);
}

// ============================================================================
// Tcl subset
// ============================================================================

namespace {

struct Word {
    enum class Kind { Bare, Quoted, Braced } kind = Kind::Bare;
    std::string raw;
    std::uint32_t line = 0; ///< line where the word starts
};

struct Command {
    std::vector<Word> words;
    std::uint32_t line = 0;
};

struct Value {
    std::string text;
    std::vector<Object> objects;
    bool hasObjects = false;
};

/// Splits a script into commands and words (no substitution yet).
class Tokenizer {
public:
    Tokenizer(std::string_view text, std::uint32_t firstLine) : s_(withLf(text)), line_(firstLine) {}

    /// Returns false at end of input. `error` is set on malformed input.
    bool next(Command& cmd, std::string& error) {
        cmd = {};
        // Skip separators and comments.
        for (;;) {
            while (i_ < s_.size() && (isSpace(s_[i_]) || s_[i_] == '\n' || s_[i_] == ';')) {
                if (s_[i_] == '\n')
                    ++line_;
                ++i_;
            }
            if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == '\n') {
                i_ += 2;
                ++line_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '#') {
                while (i_ < s_.size() && s_[i_] != '\n') {
                    if (s_[i_] == '\\' && i_ + 1 < s_.size() && s_[i_ + 1] == '\n') {
                        ++line_;
                        ++i_;
                    }
                    ++i_;
                }
                continue;
            }
            break;
        }
        if (i_ >= s_.size())
            return false;
        cmd.line = line_;
        for (;;) {
            while (i_ < s_.size() && (isSpace(s_[i_]) || (s_[i_] == '\\' && i_ + 1 < s_.size() && s_[i_ + 1] == '\n'))) {
                if (s_[i_] == '\\') {
                    ++line_;
                    i_ += 2;
                }
                else {
                    ++i_;
                }
            }
            if (i_ >= s_.size() || s_[i_] == '\n' || s_[i_] == ';')
                break;
            Word w;
            w.line = line_;
            if (s_[i_] == '{') {
                w.kind = Word::Kind::Braced;
                if (!readBraced(w.raw)) {
                    error = "missing close-brace";
                    return true;
                }
            }
            else if (s_[i_] == '"') {
                w.kind = Word::Kind::Quoted;
                if (!readQuoted(w.raw)) {
                    error = "missing close-quote";
                    return true;
                }
            }
            else if (!readBare(w.raw)) {
                error = "missing close-bracket";
                return true;
            }
            cmd.words.push_back(std::move(w));
        }
        return true;
    }

private:
    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

    bool readBraced(std::string& out) {
        int depth = 1;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                out += c;
                out += s_[i_ + 1];
                if (s_[i_ + 1] == '\n')
                    ++line_;
                i_ += 2;
                continue;
            }
            if (c == '\n')
                ++line_;
            if (c == '{')
                ++depth;
            if (c == '}' && --depth == 0) {
                ++i_;
                return true;
            }
            out += c;
            ++i_;
        }
        return false;
    }

    bool readBracket(std::string& out) {
        // s_[i_] == '['; copies up to and including the matching ']'.
        int depth = 0;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                out += c;
                out += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (c == '{') {
                std::string inner;
                if (!readBraced(inner))
                    return false;
                out += "{" + inner + "}";
                continue;
            }
            if (c == '\n')
                ++line_;
            out += c;
            ++i_;
            if (c == '[')
                ++depth;
            else if (c == ']' && --depth == 0)
                return true;
        }
        return false;
    }

    bool readQuoted(std::string& out) {
        ++i_;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                out += s_[i_];
                out += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (s_[i_] == '[') {
                if (!readBracket(out))
                    return false;
                continue;
            }
            if (s_[i_] == '\n')
                ++line_;
            out += s_[i_++];
        }
        if (i_ >= s_.size())
            return false;
        ++i_;
        return true;
    }

    bool readBare(std::string& out) {
        while (i_ < s_.size() && !isSpace(s_[i_]) && s_[i_] != '\n' && s_[i_] != ';') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                if (s_[i_ + 1] == '\n')
                    break; // continuation ends the word
                out += s_[i_];
                out += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (s_[i_] == '[') {
                if (!readBracket(out))
                    return false;
                continue;
            }
            out += s_[i_++];
        }
        return true;
    }

    /// CRLF -> LF. Constraint files written on Windows end lines with CRLF;
    /// Tcl's backslash-newline continuation must still work there.
    static std::string withLf(std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (std::size_t k = 0; k < text.size(); ++k)
            if (!(text[k] == '\r' && k + 1 < text.size() && text[k + 1] == '\n'))
                out += text[k];
        return out;
    }

    std::string s_;
    std::size_t i_ = 0;
    std::uint32_t line_;
};

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i >= text.size())
            break;
        std::string item;
        if (text[i] == '{') {
            int depth = 1;
            ++i;
            while (i < text.size() && depth > 0) {
                if (text[i] == '{')
                    ++depth;
                else if (text[i] == '}' && --depth == 0)
                    break;
                item += text[i++];
            }
            ++i;
        }
        else {
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])))
                item += text[i++];
        }
        out.push_back(std::move(item));
    }
    return out;
}

/// A time with an optional unit: "8", "8.0 ns", "800 ps", "125 MHz" -> ns.
std::optional<double> toNanoseconds(std::string_view s) {
    std::string t(s);
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str())
        return std::nullopt;
    std::string unit;
    for (const char* c = end; *c; ++c)
        if (!std::isspace(static_cast<unsigned char>(*c)))
            unit += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    if (unit.empty() || unit == "ns")
        return v;
    if (unit == "ps")
        return v / 1000.0;
    if (unit == "us")
        return v * 1000.0;
    if (unit == "ms")
        return v * 1e6;
    if (v <= 0)
        return std::nullopt;
    if (unit == "mhz")
        return 1000.0 / v;
    if (unit == "ghz")
        return 1.0 / v;
    if (unit == "khz")
        return 1e6 / v;
    return std::nullopt;
}

std::optional<double> toNumber(std::string_view s) {
    double v = 0;
    std::string tmp(s);
    char* end = nullptr;
    v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str() || *end != '\0')
        return std::nullopt;
    return v;
}

const std::set<std::string, std::less<>>& knownCommands() {
    static const std::set<std::string, std::less<>> cmds = {
        "set", "create_clock", "create_generated_clock", "set_clock_groups", "set_input_delay", "get_ports", "get_pins", "get_nets",
        "get_clocks", "all_clocks", "get_cells", "all_inputs", "all_outputs", "all_registers", "expr", "if",
        "foreach", "for", "while", "break", "continue", "incr", "llength", "lindex", "lappend", "puts",
        "get_property", "list", "concat", "proc", "return", "global", "source", "get_registers", "get_keepers",
        "get_nodes", "get_collection_size", "get_node_info", "get_object_info", "query_collection",
        "add_to_collection", "foreach_in_collection"};
    return cmds;
}

/// Commands that are valid SDC/XDC but not used by clock analysis yet.
bool isTimingOrMiscCommand(std::string_view c) {
    static const std::set<std::string, std::less<>> cmds = {
        "set_output_delay", "set_false_path", "set_max_delay", "set_min_delay",
        "set_multicycle_path", "set_clock_uncertainty", "set_clock_latency", "set_input_jitter",
        "set_system_jitter", "set_clock_sense", "set_case_analysis", "set_property", "set_load",
        "set_drive", "set_driving_cell", "set_input_transition", "set_disable_timing", "set_units",
        "current_design", "current_instance", "set_max_fanout", "set_max_transition", "set_bus_skew",
        "set_max_time_borrow", "set_propagated_clock", "set_ideal_network", "set_dont_touch",
        "set_operating_conditions", "set_wire_load_model", "group_path", "set_clock_transition",
        "create_pblock", "add_cells_to_pblock", "resize_pblock", "set_clock_gating_check", "set_data_check",
        "set_external_delay", "config_timing_pessimism", "set_assert", "set_hierarchy_separator",
        // Quartus: PLL output clocks come from the built-in PLL models
        "derive_pll_clocks", "derive_clock_uncertainty", "derive_clocks", "set_time_format", "set_max_skew",
        "set_net_delay", "set_scc_mode", "set_annotated_delay"};
    return cmds.contains(c);
}

class Interp {
public:
    Interp(IObjectResolver& resolver, ConstraintSet& out, DiagnosticSink& sink, SdcOptions options)
        : resolver_(resolver), out_(out), sink_(sink), options_(std::move(options)) {}

    /// Runs one constraint file; the interpreter state (variables,
    /// procedures) carries over to the next file.
    void runFile(std::string_view text, const std::string& fileName) {
        file_ = fileName;
        runScript(text, 1);
        ctl_ = Ctl::None; // a `return` (or stray break) at file level ends the file
        reportIgnored();
    }

    Value runScript(std::string_view text, std::uint32_t firstLine) {
        Tokenizer tok(text, firstLine);
        Command cmd;
        Value last;
        for (;;) {
            std::string error;
            if (!tok.next(cmd, error))
                break;
            if (!error.empty()) {
                diag("CON-PARSE", Severity::Error, error, cmd.line);
                break;
            }
            if (!cmd.words.empty())
                last = exec(cmd);
            if (ctl_ != Ctl::None)
                break; // break / continue / return: unwind to the enclosing loop or procedure
        }
        return last;
    }

    void reportIgnored() {
        if (ignored_.empty())
            return;
        std::string list;
        for (const auto& [name, count] : ignored_) {
            out_.ignoredCommands[name] += count;
            if (!list.empty())
                list += ", ";
            list += name + " x" + std::to_string(count);
        }
        diag("CON-IGNORED", Severity::Info,
             "commands not used by clock analysis (recorded, not applied): " + list, 0);
        ignored_.clear();
    }

private:
    void diag(const std::string& code, Severity sev, const std::string& msg, std::uint32_t line) {
        // commands given -quiet ignore their errors (Vivado): warnings become notes
        if (quiet_ && sev == Severity::Warning)
            sink_.report({code, Severity::Info, msg + " (command given -quiet)", {file_, line, 0}, {}});
        else
            sink_.report({code, sev, msg, {file_, line, 0}, {}});
    }

    Value subst(const Word& w, std::uint32_t line) {
        if (w.kind == Word::Kind::Braced)
            return Value{w.raw, {}, false};
        const std::string& raw = w.raw;
        // A word that is exactly one command substitution keeps its objects.
        if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']' && matchingBracket(raw, 0) == raw.size() - 1) {
            auto inner = std::string_view(raw).substr(1, raw.size() - 2);
            if (looksLikeCommand(inner))
                return runScript(inner, line);
        }
        Value v;
        bool literal = false; // text besides substitutions: the word is a plain string
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const char c = raw[i];
            if (c == '\\' && i + 1 < raw.size()) {
                const char n = raw[++i];
                v.text += n == 'n' ? '\n' : n == 't' ? '\t' : n;
                literal = true;
                continue;
            }
            if (c == '$') {
                std::string name;
                if (i + 1 < raw.size() && raw[i + 1] == '{') {
                    auto close = raw.find('}', i + 2);
                    if (close == std::string::npos) {
                        v.text += c;
                        continue;
                    }
                    name = raw.substr(i + 2, close - i - 2);
                    i = close;
                }
                else {
                    std::size_t j = i + 1;
                    while (j < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[j])) || raw[j] == '_' ||
                                              raw[j] == ':'))
                        ++j;
                    if (j == i + 1) {
                        v.text += c;
                        continue;
                    }
                    name = raw.substr(i + 1, j - i - 1);
                    i = j - 1;
                }
                const Value* var = findVar(name);
                if (!var) {
                    diag("CON-PARSE", Severity::Warning, "undefined variable '$" + name + "'", line);
                    continue;
                }
                v.text += var->text;
                if (var->hasObjects) {
                    v.objects.insert(v.objects.end(), var->objects.begin(), var->objects.end());
                    v.hasObjects = true;
                }
                continue;
            }
            if (c == '[') {
                const auto close = matchingBracket(raw, i);
                if (close != std::string::npos) {
                    auto inner = std::string_view(raw).substr(i + 1, close - i - 1);
                    if (looksLikeCommand(inner)) {
                        Value r = runScript(inner, line);
                        v.text += r.text;
                        if (r.hasObjects) {
                            v.objects.insert(v.objects.end(), r.objects.begin(), r.objects.end());
                            v.hasObjects = true;
                        }
                        i = close;
                        continue;
                    }
                }
                // Not a command (e.g. data[3], bus[*]): keep literally.
            }
            v.text += c;
            literal = true;
        }
        if (literal && v.hasObjects) {
            // "$inst/q_reg" is a name built from an object, not the object
            v.objects.clear();
            v.hasObjects = false;
        }
        return v;
    }

    static std::size_t matchingBracket(const std::string& s, std::size_t open) {
        int depth = 0;
        for (std::size_t i = open; i < s.size(); ++i) {
            if (s[i] == '\\') {
                ++i;
                continue;
            }
            if (s[i] == '[')
                ++depth;
            else if (s[i] == ']' && --depth == 0)
                return i;
        }
        return std::string::npos;
    }

    bool looksLikeCommand(std::string_view inner) const {
        std::size_t i = 0;
        while (i < inner.size() && std::isspace(static_cast<unsigned char>(inner[i])))
            ++i;
        std::size_t j = i;
        while (j < inner.size() && !std::isspace(static_cast<unsigned char>(inner[j])))
            ++j;
        const auto name = inner.substr(i, j - i);
        return knownCommands().contains(name) || isTimingOrMiscCommand(name) ||
               procs_.contains(std::string(name));
    }

    struct Args {
        std::map<std::string, std::vector<Value>> opts;
        std::set<std::string> flags;
        std::vector<Value> positional;
    };

    /// spec: option -> takes a value.
    Args parseArgs(const Command& cmd, const std::string& name, const std::map<std::string, bool>& spec) {
        Args a;
        for (std::size_t i = 1; i < cmd.words.size(); ++i) {
            const Word& w = cmd.words[i];
            if (w.kind == Word::Kind::Bare && !w.raw.empty() && w.raw[0] == '-' && w.raw.size() > 1 &&
                !std::isdigit(static_cast<unsigned char>(w.raw[1]))) {
                std::string opt = w.raw;
                if (opt == "-hier")
                    opt = "-hierarchical";
                auto it = spec.find(opt);
                if (it == spec.end()) {
                    // Tcl commands accept any unique prefix of an option (-of for -of_objects)
                    auto match = spec.end();
                    int count = 0;
                    for (auto k = spec.begin(); k != spec.end(); ++k)
                        if (k->first.starts_with(opt)) {
                            match = k;
                            ++count;
                        }
                    if (count == 1) {
                        it = match;
                        opt = match->first;
                    }
                }
                if (it == spec.end()) {
                    // Most options take a value: skip it too unless it is another option.
                    const bool nextIsValue = i + 1 < cmd.words.size() &&
                                             !(cmd.words[i + 1].kind == Word::Kind::Bare &&
                                               cmd.words[i + 1].raw.starts_with('-'));
                    diag("CON-UNKNOWN-OPTION", Severity::Warning,
                         name + ": unknown option '" + w.raw + "' ignored" +
                             (nextIsValue ? " (together with its value '" + cmd.words[i + 1].raw + "')" : ""),
                         cmd.line);
                    if (nextIsValue)
                        ++i;
                    continue;
                }
                if (it->second) {
                    if (i + 1 >= cmd.words.size()) {
                        diag("CON-PARSE", Severity::Error, name + ": option " + opt + " requires a value", cmd.line);
                        continue;
                    }
                    a.opts[opt].push_back(subst(cmd.words[++i], cmd.line));
                }
                else {
                    a.flags.insert(opt);
                }
                continue;
            }
            a.positional.push_back(subst(w, cmd.line));
        }
        return a;
    }

    std::vector<Object> objectsOf(const Value& v, const Command& cmd, const std::string& ctx) {
        if (v.hasObjects)
            return v.objects;
        // Plain names: search ports, then pins, then nets (Synopsys behaviour).
        std::vector<Object> out;
        for (const auto& pat : splitList(v.text)) {
            auto r = resolver_.ports(pat, false);
            if (r.empty())
                r = resolver_.pins(pat, false, false);
            if (r.empty())
                r = resolver_.nets(pat, false, false);
            if (r.empty() && pat.find('|') != std::string::npos) // a Quartus name
                r = resolver_.quartus(pat, QuartusKind::Pins, false);
            if (r.empty()) {
                if (pat.starts_with("altera_reserved_"))
                    unmatched(ctx, pat, cmd.line);
                else
                    diag("CON-UNMATCHED", Severity::Warning, ctx + ": no port, pin or net matches '" + pat + "'", cmd.line);
            }
            out.insert(out.end(), r.begin(), r.end());
        }
        return out;
    }

    Value objectsValue(std::vector<Object> objs) {
        Value v;
        for (const auto& o : objs) {
            if (!v.text.empty())
                v.text += ' ';
            v.text += o.name;
        }
        v.objects = std::move(objs);
        v.hasObjects = true;
        return v;
    }

    Value getObjects(const Command& cmd, const std::string& name) {
        const bool clocks = name == "get_clocks";
        std::map<std::string, bool> spec = {{"-quiet", false}, {"-nocase", false}, {"-regexp", false},
                                            {"-filter", true},  {"-of_objects", true}, {"-hierarchical", false},
                                            {"-include_generated_clocks", false}};
        if (name == "get_nets")
            spec.emplace("-segments", false); // all segments of a net: our nets are whole

        Args a = parseArgs(cmd, name, spec);
        const bool quiet = a.flags.contains("-quiet");
        const bool nocase = a.flags.contains("-nocase");
        const bool hier = a.flags.contains("-hierarchical");
        if (a.flags.contains("-regexp"))
            diag("CON-UNSUPPORTED-OPTION", Severity::Warning, name + ": -regexp is not supported; patterns are treated as globs",
                 cmd.line);
        const bool pins = name == "get_pins";
        if (a.opts.contains("-filter") && !pins)
            diag("CON-UNSUPPORTED-OPTION", Severity::Warning, name + ": -filter is not supported and was ignored", cmd.line);
        const std::string filter = pins && a.opts.contains("-filter") ? a.opts["-filter"].back().text : std::string();
        if (a.opts.contains("-of_objects") && !clocks) {
            if (name == "get_ports") {
                diag("CON-UNSUPPORTED-OPTION", Severity::Warning,
                     name + ": -of_objects is not supported; the command returns nothing", cmd.line);
                return objectsValue({});
            }
            // netlist traversal: pins of cells or nets, nets of pins
            std::vector<Object> result;
            bool unsupportedKind = false;
            for (const auto& ov : a.opts["-of_objects"])
                for (const auto& o : objectsOf(ov, cmd, name + " -of_objects")) {
                    std::vector<Object> r;
                    if (pins && o.kind == ObjectKind::Cell)
                        r = resolver_.cellPins(o);
                    else if (pins && o.kind == ObjectKind::Net)
                        r = resolver_.netPins(o);
                    else if (!pins && o.kind == ObjectKind::Pin)
                        r = resolver_.pinNets(o);
                    else
                        unsupportedKind = true;
                    for (auto& x : r)
                        if (std::ranges::find(result, x.name, &Object::name) == result.end())
                            result.push_back(std::move(x));
                }
            if (unsupportedKind)
                diag("CON-UNSUPPORTED-OPTION", Severity::Warning,
                     name + ": -of_objects supports " + (pins ? "cells and nets" : "pins") +
                         "; other objects contribute nothing",
                     cmd.line);
            if (!filter.empty())
                applyFilter(result, filter, true, false, name, cmd);
            return objectsValue(std::move(result));
        }
        if (a.opts.contains("-of_objects")) {
            // get_clocks -of_objects <nets/pins/ports>: the clocks on these
            // objects, known only after clock analysis (kept as their bits)
            // (cells: the clocks of their flip-flops)
            std::vector<Object> result;
            for (const auto& ov : a.opts["-of_objects"])
                for (auto& o : objectsOf(ov, cmd, "get_clocks -of_objects")) {
                    auto bits = o.kind == ObjectKind::Cell ? resolver_.cellClocks(o) : std::move(o.bits);
                    if (bits.empty() || std::ranges::any_of(result, [&](const Object& r) { return r.bits == bits; }))
                        continue;
                    result.push_back({ObjectKind::Clock, "-of_objects " + o.name, std::move(bits)});
                }
            return objectsValue(std::move(result));
        }
        std::vector<Object> result;
        for (const auto& pv : a.positional) {
            std::vector<std::string> patterns;
            if (pv.hasObjects)
                for (const auto& o : pv.objects)
                    patterns.push_back(o.name);
            else
                patterns = splitList(pv.text);
            for (const auto& pat : patterns) {
                if (clocks) {
                    result.push_back({ObjectKind::Clock, pat, {}});
                    continue;
                }
                std::vector<Object> r = name == "get_ports"   ? resolver_.ports(pat, nocase)
                                        : name == "get_pins"  ? resolver_.pins(pat, hier, nocase)
                                        : name == "get_cells" ? resolver_.cells(pat, hier, nocase)
                                                              : resolver_.nets(pat, hier, nocase);
                if (r.empty() && pins && pat.find('|') != std::string::npos)
                    r = resolver_.quartus(pat, QuartusKind::Pins, nocase); // a Quartus name held in a variable
                if (!filter.empty() && !applyFilter(r, filter, true, false, name, cmd))
                    return objectsValue({});
                if (r.empty() && !quiet) {
                    if (filter.empty())
                        unmatched(name, pat, cmd.line);
                    else
                        diag("CON-UNMATCHED", Severity::Warning,
                             name + ": nothing matches '" + pat + "' with filter {" + filter + "}", cmd.line);
                }
                result.insert(result.end(), r.begin(), r.end());
            }
        }
        return objectsValue(std::move(result));
    }

    static std::vector<BitRef> bitsOf(const std::vector<Object>& objs) {
        std::vector<BitRef> out;
        for (const auto& o : objs)
            out.insert(out.end(), o.bits.begin(), o.bits.end());
        return out;
    }

    void addClock(ClockDef def, std::uint32_t line) {
        if (!def.add) {
            auto it = std::ranges::find(out_.clocks, def.name, &ClockDef::name);
            if (it != out_.clocks.end()) {
                diag("CON-DUPLICATE-CLOCK", Severity::Warning,
                     "clock '" + def.name + "' redefined; the earlier definition (" + toString(it->loc) + ") is replaced",
                     line);
                out_.clocks.erase(it);
            }
        }
        out_.clocks.push_back(std::move(def));
    }

    Value createClock(const Command& cmd) {
        Args a = parseArgs(cmd, "create_clock",
                           {{"-name", true}, {"-period", true}, {"-waveform", true}, {"-add", false}, {"-comment", true}});
        ClockDef def;
        def.loc = {file_, cmd.line, 0};
        def.add = a.flags.contains("-add");
        if (a.opts.contains("-period")) {
            auto p = toNanoseconds(a.opts["-period"].back().text);
            if (!p || *p <= 0)
                diag("CON-PARSE", Severity::Error, "create_clock: invalid -period '" + a.opts["-period"].back().text + "'",
                     cmd.line);
            else
                def.periodNs = *p;
        }
        else {
            diag("CON-PARSE", Severity::Warning, "create_clock without -period", cmd.line);
        }
        if (a.opts.contains("-waveform"))
            for (const auto& w : splitList(a.opts["-waveform"].back().text))
                if (auto n = toNumber(w))
                    def.waveform.push_back(*n);
        std::vector<Object> objs;
        for (const auto& pv : a.positional) {
            auto o = objectsOf(pv, cmd, "create_clock");
            objs.insert(objs.end(), o.begin(), o.end());
        }
        def.sources = bitsOf(objs);
        for (const auto& o : objs)
            def.sourceNames.push_back(o.name);
        if (a.opts.contains("-name"))
            def.name = a.opts["-name"].back().text;
        else if (!objs.empty())
            def.name = objs.front().name;
        if (def.name.empty()) {
            diag("CON-PARSE", Severity::Error, "create_clock: a virtual clock needs -name", cmd.line);
            return {};
        }
        const std::string name = def.name;
        addClock(std::move(def), cmd.line);
        return Value{name, {{ObjectKind::Clock, name, {}}}, true};
    }

    Value createGeneratedClock(const Command& cmd) {
        Args a = parseArgs(cmd, "create_generated_clock",
                           {{"-name", true}, {"-source", true}, {"-master_clock", true}, {"-divide_by", true},
                            {"-multiply_by", true}, {"-invert", false}, {"-edges", true}, {"-edge_shift", true},
                            {"-duty_cycle", true}, {"-add", false}, {"-combinational", false}, {"-comment", true}});
        ClockDef def;
        def.generated = true;
        def.loc = {file_, cmd.line, 0};
        def.add = a.flags.contains("-add");
        def.invert = a.flags.contains("-invert");
        if (a.opts.contains("-edges") || a.opts.contains("-edge_shift"))
            diag("CON-UNSUPPORTED-OPTION", Severity::Warning,
                 "create_generated_clock: -edges/-edge_shift are not supported; the clock is treated as synchronous "
                 "to its master without waveform",
                 cmd.line);
        auto intOpt = [&](const char* opt, int& dst) {
            if (!a.opts.contains(opt))
                return;
            auto n = toNumber(a.opts[opt].back().text);
            if (!n || *n < 1)
                diag("CON-PARSE", Severity::Error, std::string("create_generated_clock: invalid ") + opt, cmd.line);
            else
                dst = static_cast<int>(*n);
        };
        intOpt("-divide_by", def.divideBy);
        intOpt("-multiply_by", def.multiplyBy);
        if (a.opts.contains("-source"))
            def.masterSource = bitsOf(objectsOf(a.opts["-source"].back(), cmd, "create_generated_clock -source"));
        if (a.opts.contains("-master_clock")) {
            const auto& mv = a.opts["-master_clock"].back();
            def.masterClock = mv.hasObjects && !mv.objects.empty() ? mv.objects.front().name : mv.text;
        }
        if (def.masterSource.empty() && def.masterClock.empty()) {
            if (a.opts.contains("-source"))
                return {}; // the -source query matched nothing (reported by the query): no clock
            diag("CON-PARSE", Severity::Error, "create_generated_clock: -source or -master_clock is required", cmd.line);
        }
        std::vector<Object> objs;
        for (const auto& pv : a.positional) {
            auto o = objectsOf(pv, cmd, "create_generated_clock");
            objs.insert(objs.end(), o.begin(), o.end());
        }
        def.sources = bitsOf(objs);
        for (const auto& o : objs)
            def.sourceNames.push_back(o.name);
        def.name = a.opts.contains("-name") ? a.opts["-name"].back().text : (objs.empty() ? "" : objs.front().name);
        if (def.name.empty() || def.sources.empty()) {
            diag("CON-PARSE", Severity::Error, "create_generated_clock: no target object", cmd.line);
            return {};
        }
        const std::string name = def.name;
        addClock(std::move(def), cmd.line);
        return Value{name, {{ObjectKind::Clock, name, {}}}, true};
    }

    Value setInputDelay(const Command& cmd) {
        Args a = parseArgs(cmd, "set_input_delay",
                           {{"-clock", true}, {"-clock_fall", false}, {"-max", false}, {"-min", false}, {"-add_delay", false},
                            {"-rise", false}, {"-fall", false}, {"-network_latency_included", false},
                            {"-source_latency_included", false}, {"-reference_pin", true}});
        if (!a.opts.contains("-clock")) {
            // A delay without a clock does not assign a domain.
            ++ignored_["set_input_delay (without -clock)"];
            return {};
        }
        InputDelay d;
        d.loc = {file_, cmd.line, 0};
        d.clockFall = a.flags.contains("-clock_fall");
        const auto& cv = a.opts["-clock"].back();
        d.clock = cv.hasObjects && !cv.objects.empty() ? cv.objects.front().name : cv.text;
        if (cv.hasObjects && !cv.objects.empty() && cv.objects.front().kind == ObjectKind::Clock)
            d.clockBits = cv.objects.front().bits; // get_clocks -of_objects
        // Positional arguments: <delay_value> <port_pin_list> (the number is skipped).
        bool portsGiven = false;
        for (const auto& pv : a.positional) {
            if (!pv.hasObjects && toNumber(pv.text))
                continue;
            portsGiven = true;
            auto objs = objectsOf(pv, cmd, "set_input_delay");
            for (const auto& o : objs) {
                d.portNames.push_back(o.name);
                d.bits.insert(d.bits.end(), o.bits.begin(), o.bits.end());
            }
        }
        if (d.bits.empty()) {
            if (!portsGiven) // ports that match nothing are reported by their query
                diag("CON-PARSE", Severity::Warning, "set_input_delay: no port given", cmd.line);
            return {};
        }
        out_.inputDelays.push_back(std::move(d));
        return {};
    }

    /// set_false_path / set_max_delay -datapath_only. Evaluated forms:
    ///  - clocks on both sides: a clock exception (ClockException);
    ///  - registers, pins or ports on at least one side (the other side may be
    ///    clocks or missing): a path exception (PathException).
    /// Anything else (-through, -hold, plain max delays, queries we cannot
    /// answer) is recorded and not used.
    bool clockException(const Command& cmd, const std::string& name) {
        // look at the words before evaluating anything, so that exceptions
        // we do not use cause no queries (and no warnings)
        enum class Side : std::uint8_t { None, Clocks, Cells };
        Side from = Side::None, to = Side::None;
        bool datapathOnly = false, quiet = false, quartusQuery = false;
        for (std::size_t i = 1; i < cmd.words.size(); ++i) {
            const std::string& w = cmd.words[i].raw;
            if (w == "-through" || w == "-hold" || w == "-rise_from" || w == "-fall_from" || w == "-rise_to" ||
                w == "-fall_to" || w == "-rise_through" || w == "-fall_through")
                return false;
            if (w == "-datapath_only")
                datapathOnly = true;
            if (w == "-quiet")
                quiet = true;
            if ((w != "-from" && w != "-to") || i + 1 >= cmd.words.size())
                continue;
            std::string v = cmd.words[i + 1].raw;
            // property filters on nets and ports are not supported
            if (v.find("-filter") != std::string::npos &&
                (v.find("get_nets") != std::string::npos || v.find("get_ports") != std::string::npos))
                return false;
            v.erase(0, v.find_first_not_of(" \t["));
            Side side = Side::None;
            if (cmd.words[i + 1].raw == "*")
                continue; // everything: as if the side were not given
            if (v.starts_with("get_registers") || v.starts_with("get_keepers") || v.starts_with("get_nodes"))
                quartusQuery = true;
            if (v.starts_with('$')) {
                // a variable holding the result of an earlier query
                std::string var = v.substr(1);
                if (var.size() >= 2 && var.front() == '{' && var.back() == '}')
                    var = var.substr(1, var.size() - 2);
                const Value* held = findVar(var);
                if (!held || !held->hasObjects || held->objects.empty())
                    return false;
                const auto& objs = held->objects;
                if (std::ranges::all_of(objs, [](const Object& o) { return o.kind == ObjectKind::Clock; }))
                    side = Side::Clocks;
                else if (std::ranges::none_of(objs, [](const Object& o) {
                             return o.kind == ObjectKind::Clock || o.kind == ObjectKind::Net;
                         }))
                    side = Side::Cells;
                else
                    return false;
            }
            else if (v.starts_with("get_clocks") || v.starts_with("all_clocks"))
                side = Side::Clocks;
            else if (v.starts_with("get_cells") || v.starts_with("get_pins") || v.starts_with("get_ports") ||
                     v.starts_with("get_registers") || v.starts_with("get_keepers") || v.starts_with("get_nodes"))
                side = Side::Cells;
            else
                return false;
            (w == "-from" ? from : to) = side;
        }
        // Quartus has no -datapath_only: there a max delay between registers
        // of unrelated clocks is the way crossings are constrained
        if (name == "set_max_delay" && !datapathOnly && !quartusQuery)
            return false;
        if (from == Side::Cells || to == Side::Cells) {
            quiet_ = quiet;
            pathException(cmd, name);
            quiet_ = false;
            return true;
        }
        if (from != Side::Clocks || to != Side::Clocks)
            return false;
        Args a = parseArgs(cmd, name,
                           {{"-from", true}, {"-to", true}, {"-through", true}, {"-datapath_only", false},
                            {"-setup", false}, {"-hold", false}, {"-rise", false}, {"-fall", false},
                            {"-reset_path", false}, {"-quiet", false}, {"-comment", true}, {"-ignore_clock_latency", false},
                            {"-rise_from", true}, {"-fall_from", true}, {"-rise_to", true}, {"-fall_to", true}});
        if (!a.opts.contains("-from") || !a.opts.contains("-to"))
            return false;
        ClockException e;
        e.command = name == "set_false_path" ? name : name + " -datapath_only";
        e.loc = {file_, cmd.line, 0};
        auto collect = [&](const std::vector<Value>& vs, std::vector<std::string>& names,
                           std::vector<BitRef>& bits) {
            for (const auto& v : vs) {
                if (!v.hasObjects)
                    return false; // plain names could be cells: not a clock exception
                for (const auto& o : v.objects) {
                    if (o.kind != ObjectKind::Clock)
                        return false;
                    if (o.bits.empty())
                        names.push_back(o.name);
                    else
                        bits.insert(bits.end(), o.bits.begin(), o.bits.end());
                }
            }
            return true;
        };
        if (!collect(a.opts["-from"], e.from, e.fromBits) || !collect(a.opts["-to"], e.to, e.toBits))
            return false;
        out_.clockExceptions.push_back(std::move(e));
        return true;
    }

    /// Q bits of the flip-flop of a pin (or of a flip-flop cell).
    std::vector<BitRef> flipFlopBits(const Object& o) {
        if (o.kind == ObjectKind::Cell)
            return o.instance ? std::vector<BitRef>{} : o.bits;
        std::vector<BitRef> out;
        for (const auto& c : resolver_.pinCells(o))
            if (!c.instance)
                out.insert(out.end(), c.bits.begin(), c.bits.end());
        return out;
    }

    static EndpointRole roleOf(const std::string& pin) {
        if (pin == "CLR" || pin == "PRE")
            return EndpointRole::AsyncReset;
        if (pin == "D" || pin == "CE" || pin == "R" || pin == "S")
            return EndpointRole::Data;
        return EndpointRole::Any;
    }

    /// set_false_path / set_max_delay -datapath_only between registers, pins,
    /// ports and clocks (see clockException).
    void pathException(const Command& cmd, const std::string& name) {
        Args a = parseArgs(cmd, name,
                           {{"-from", true}, {"-to", true}, {"-through", true}, {"-datapath_only", false},
                            {"-setup", false}, {"-hold", false}, {"-rise", false}, {"-fall", false},
                            {"-reset_path", false}, {"-quiet", false}, {"-comment", true}, {"-ignore_clock_latency", false}});
        PathException e;
        e.command = name == "set_false_path" ? name : name + " -datapath_only";
        e.loc = {file_, cmd.line, 0};
        bool fromNamed = false, toNamed = false; // registers, pins or ports named on the side
        // "-from *" / "-to *": everything, as if the side were not given
        auto given = [&](const char* opt) {
            const auto it = a.opts.find(opt);
            return it != a.opts.end() && !std::ranges::all_of(it->second, [](const Value& v) {
                return !v.hasObjects && v.text == "*";
            });
        };
        const bool hasFrom = given("-from"), hasTo = given("-to");
        for (const auto& v : a.opts["-from"])
            for (const auto& o : v.objects) {
                if (o.kind == ObjectKind::Clock) {
                    if (o.bits.empty())
                        e.fromClocks.push_back(o.name);
                    else
                        e.fromClockBits.insert(e.fromClockBits.end(), o.bits.begin(), o.bits.end());
                    continue;
                }
                fromNamed = true;
                // startpoints: flip-flops (by any of their pins) and ports;
                // a port of an instance: whatever drives it
                const auto bits = o.kind == ObjectKind::Cell || !o.pin.empty() ? flipFlopBits(o) : o.bits;
                e.from.insert(e.from.end(), bits.begin(), bits.end());
            }
        for (const auto& v : a.opts["-to"])
            for (const auto& o : v.objects) {
                if (o.kind == ObjectKind::Clock) {
                    if (o.bits.empty())
                        e.toClocks.push_back(o.name);
                    else
                        e.toClockBits.insert(e.toClockBits.end(), o.bits.begin(), o.bits.end());
                    continue;
                }
                toNamed = true;
                if (o.kind == ObjectKind::Cell || !o.pin.empty()) {
                    const EndpointRole role = o.kind == ObjectKind::Cell ? EndpointRole::Any : roleOf(o.pin);
                    for (const auto& b : flipFlopBits(o))
                        e.to.push_back({b, role});
                }
                else if (o.kind == ObjectKind::Pin) {
                    // a port of an instance (a modelled primitive such as
                    // FDPE): the flip-flops inside it that the port feeds
                    const std::string inside = o.name.substr(0, o.name.rfind('/') + 1);
                    for (const auto& net : resolver_.pinNets(o))
                        for (const auto& p : resolver_.netPins(net))
                            if (!p.pin.empty() && p.pin != "Q" && p.name.starts_with(inside))
                                for (const auto& b : flipFlopBits(p))
                                    e.to.push_back({b, roleOf(p.pin)});
                }
                else {
                    for (const auto& b : o.bits)
                        e.to.push_back({b, EndpointRole::Any});
                }
            }
        const bool fromEmpty = hasFrom && e.from.empty() && e.fromClocks.empty() && e.fromClockBits.empty();
        const bool toEmpty = hasTo && e.to.empty() && e.toClocks.empty() && e.toClockBits.empty();
        if ((fromNamed && e.from.empty()) || (toNamed && e.to.empty()) || fromEmpty || toEmpty || (!hasFrom && !hasTo)) {
            // a query that matched nothing has reported that already
            const auto& side = a.opts[fromEmpty || (fromNamed && e.from.empty()) ? "-from" : "-to"];
            const bool reported = !side.empty() && std::ranges::all_of(side, [](const Value& v) { return v.hasObjects; });
            diag("CON-UNMATCHED", reported ? Severity::Info : Severity::Warning,
                 name + ": " + (fromEmpty || (fromNamed && e.from.empty()) ? "-from" : "-to") +
                     " matches no register, pin or port; the exception is not used",
                 cmd.line);
            return;
        }
        out_.pathExceptions.push_back(std::move(e));
    }

    Value setClockGroups(const Command& cmd) {
        Args a = parseArgs(cmd, "set_clock_groups",
                           {{"-name", true}, {"-group", true}, {"-asynchronous", false}, {"-logically_exclusive", false},
                            {"-physically_exclusive", false}, {"-allow_paths", false}, {"-comment", true}});
        ClockGroups g;
        g.loc = {file_, cmd.line, 0};
        if (a.opts.contains("-name"))
            g.name = a.opts["-name"].back().text;
        const int kinds = static_cast<int>(a.flags.contains("-asynchronous")) +
                          static_cast<int>(a.flags.contains("-logically_exclusive")) +
                          static_cast<int>(a.flags.contains("-physically_exclusive"));
        if (kinds != 1) {
            diag("CON-PARSE", Severity::Error,
                 "set_clock_groups needs exactly one of -asynchronous, -logically_exclusive, -physically_exclusive",
                 cmd.line);
            return {};
        }
        g.kind = a.flags.contains("-asynchronous")          ? GroupKind::Asynchronous
                 : a.flags.contains("-logically_exclusive") ? GroupKind::LogicallyExclusive
                                                            : GroupKind::PhysicallyExclusive;
        for (const auto& gv : a.opts["-group"]) {
            std::vector<std::string> names;
            std::vector<BitRef> bits;
            if (gv.hasObjects) {
                for (const auto& o : gv.objects) {
                    if (o.kind == ObjectKind::Clock && !o.bits.empty())
                        bits.insert(bits.end(), o.bits.begin(), o.bits.end()); // get_clocks -of_objects
                    else
                        names.push_back(o.name);
                }
            }
            else {
                names = splitList(gv.text);
            }
            g.groups.push_back(std::move(names));
            if (!bits.empty()) {
                g.memberBits.resize(g.groups.size());
                g.memberBits.back() = std::move(bits);
            }
        }
        if (g.groups.empty()) {
            diag("CON-PARSE", Severity::Error, "set_clock_groups without -group", cmd.line);
            return {};
        }
        out_.groups.push_back(std::move(g));
        return {};
    }

    // ------------------------------------------------------------------
    // expr
    // ------------------------------------------------------------------

    struct ExprError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    struct ExprValue {
        bool num = false;
        bool integer = false;
        double d = 0;
        std::string s;
    };

    static ExprValue exprValue(std::string text) {
        ExprValue v;
        std::string t = text;
        t.erase(0, t.find_first_not_of(" \t\n"));
        t.erase(t.find_last_not_of(" \t\n") + 1);
        if (auto n = toNumber(t)) {
            v.num = true;
            v.d = *n;
            v.integer = t.find_first_of(".eEnNiI") == std::string::npos ||
                        (t.starts_with("0x") || t.starts_with("0X"));
        }
        v.s = std::move(text);
        return v;
    }

    static ExprValue numberValue(double d, bool integer) {
        ExprValue v;
        v.num = true;
        v.integer = integer;
        v.d = d;
        return v;
    }

    static std::string formatNumber(const ExprValue& v) {
        if (v.integer && std::isfinite(v.d))
            return std::to_string(static_cast<long long>(v.d));
        if (std::isnan(v.d))
            return "nan";
        char buf[64];
        auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v.d);
        std::string out(buf, ec == std::errc{} ? end : buf);
        if (out.find_first_of(".eni") == std::string::npos)
            out += ".0";
        return out;
    }

    static bool truth(const ExprValue& v) {
        if (v.num)
            return v.d != 0;
        std::string t;
        for (char c : v.s)
            if (!std::isspace(static_cast<unsigned char>(c)))
                t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t == "true" || t == "yes" || t == "on")
            return true;
        if (t == "false" || t == "no" || t == "off")
            return false;
        throw ExprError("expected a boolean value but got \"" + v.s + "\"");
    }

    /// Recursive-descent evaluator of Tcl expressions. Substitutes $var and
    /// [command] itself (braced expressions arrive unsubstituted).
    class ExprParser {
    public:
        ExprParser(Interp& in, std::string_view s, std::uint32_t line) : in_(in), s_(s), line_(line) {}

        ExprValue parse() {
            ExprValue v = ternary();
            skip();
            if (i_ < s_.size())
                throw ExprError("unexpected '" + std::string(s_.substr(i_)) + "'");
            return v;
        }

    private:
        void skip() {
            while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])))
                ++i_;
        }
        bool eat(std::string_view op) {
            skip();
            if (s_.substr(i_).starts_with(op)) {
                // do not split '<=' into '<' '=' or '&&' into '&'
                i_ += op.size();
                return true;
            }
            return false;
        }
        static double need(const ExprValue& v, const char* op) {
            if (!v.num)
                throw ExprError(std::string("can't use non-numeric string \"") + v.s + "\" as operand of \"" + op + "\"");
            return v.d;
        }

        ExprValue ternary() {
            ExprValue c = lor();
            if (!eat("?"))
                return c;
            ExprValue a = ternary();
            if (!eat(":"))
                throw ExprError("missing ':' in ?: expression");
            ExprValue b = ternary();
            return truth(c) ? a : b;
        }
        ExprValue lor() {
            ExprValue a = land();
            while (eat("||")) {
                ExprValue b = land();
                a = numberValue(truth(a) || truth(b) ? 1 : 0, true);
            }
            return a;
        }
        ExprValue land() {
            ExprValue a = equality();
            while (eat("&&")) {
                ExprValue b = equality();
                a = numberValue(truth(a) && truth(b) ? 1 : 0, true);
            }
            return a;
        }
        ExprValue equality() {
            ExprValue a = relational();
            for (;;) {
                skip();
                std::string op;
                for (const char* o : {"==", "!=", "eq ", "ne "})
                    if (s_.substr(i_).starts_with(o)) {
                        op = o;
                        break;
                    }
                if (op.empty())
                    return a;
                i_ += op.size();
                ExprValue b = relational();
                bool eq = false;
                if ((op == "==" || op == "!=") && a.num && b.num)
                    eq = a.d == b.d;
                else
                    eq = (a.num ? formatNumber(a) : a.s) == (b.num ? formatNumber(b) : b.s);
                const bool negate = op == "!=" || op == "ne ";
                a = numberValue(eq != negate ? 1 : 0, true);
            }
        }
        ExprValue relational() {
            ExprValue a = additive();
            for (;;) {
                skip();
                std::string op;
                for (const char* o : {"<=", ">=", "<", ">"})
                    if (s_.substr(i_).starts_with(o) && !s_.substr(i_).starts_with("<<") &&
                        !s_.substr(i_).starts_with(">>")) {
                        op = o;
                        break;
                    }
                if (op.empty())
                    return a;
                i_ += op.size();
                ExprValue b = additive();
                int cmp = 0;
                if (a.num && b.num)
                    cmp = a.d < b.d ? -1 : a.d > b.d ? 1 : 0;
                else
                    cmp = a.s.compare(b.s);
                const bool r = op == "<" ? cmp < 0 : op == ">" ? cmp > 0 : op == "<=" ? cmp <= 0 : cmp >= 0;
                a = numberValue(r ? 1 : 0, true);
            }
        }
        ExprValue additive() {
            ExprValue a = multiplicative();
            for (;;) {
                skip();
                if (i_ >= s_.size() || (s_[i_] != '+' && s_[i_] != '-'))
                    return a;
                const char op = s_[i_++];
                ExprValue b = multiplicative();
                const double x = need(a, op == '+' ? "+" : "-"), y = need(b, op == '+' ? "+" : "-");
                a = numberValue(op == '+' ? x + y : x - y, a.integer && b.integer);
            }
        }
        ExprValue multiplicative() {
            ExprValue a = unary();
            for (;;) {
                skip();
                if (i_ >= s_.size() || (s_[i_] != '*' && s_[i_] != '/' && s_[i_] != '%') ||
                    s_.substr(i_).starts_with("**"))
                    return a;
                const char op = s_[i_++];
                ExprValue b = unary();
                const char name[2] = {op, 0};
                const double x = need(a, name), y = need(b, name);
                const bool ints = a.integer && b.integer;
                if ((op == '/' || op == '%') && ints && y == 0)
                    throw ExprError("divide by zero");
                if (op == '*')
                    a = numberValue(x * y, ints);
                else if (op == '/')
                    a = numberValue(ints ? std::floor(x / y) : x / y, ints);
                else
                    a = numberValue(x - y * std::floor(x / y), ints);
            }
        }
        ExprValue unary() {
            skip();
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+' || s_[i_] == '!')) {
                const char op = s_[i_++];
                ExprValue v = unary();
                if (op == '!')
                    return numberValue(truth(v) ? 0 : 1, true);
                const double x = need(v, op == '-' ? "-" : "+");
                return numberValue(op == '-' ? -x : x, v.integer);
            }
            return primary();
        }
        ExprValue primary() {
            skip();
            if (i_ >= s_.size())
                throw ExprError("missing operand");
            const char c = s_[i_];
            if (c == '(') {
                ++i_;
                ExprValue v = ternary();
                if (!eat(")"))
                    throw ExprError("missing ')'");
                return v;
            }
            if (c == '$') {
                std::size_t j = i_ + 1;
                std::string name;
                if (j < s_.size() && s_[j] == '{') {
                    const auto close = s_.find('}', j);
                    if (close == std::string_view::npos)
                        throw ExprError("missing '}' after '${'");
                    name = std::string(s_.substr(j + 1, close - j - 1));
                    i_ = close + 1;
                }
                else {
                    while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_' ||
                                             s_[j] == ':'))
                        ++j;
                    name = std::string(s_.substr(i_ + 1, j - i_ - 1));
                    i_ = j;
                }
                const Value* var = in_.findVar(name);
                if (!var)
                    throw ExprError("can't read \"" + name + "\": no such variable");
                return exprValue(var->text);
            }
            if (c == '[') {
                const std::string str(s_);
                const auto close = matchingBracket(str, i_);
                if (close == std::string::npos)
                    throw ExprError("missing ']'");
                Value r = in_.runScript(s_.substr(i_ + 1, close - i_ - 1), line_);
                i_ = close + 1;
                return exprValue(r.text);
            }
            if (c == '"') {
                std::size_t j = i_ + 1;
                while (j < s_.size() && s_[j] != '"')
                    j += s_[j] == '\\' ? 2U : 1U;
                if (j >= s_.size())
                    throw ExprError("missing '\"'");
                Word w{Word::Kind::Quoted, std::string(s_.substr(i_ + 1, j - i_ - 1)), line_};
                i_ = j + 1;
                return exprValue(in_.subst(w, line_).text);
            }
            if (c == '{') {
                int depth = 0;
                std::size_t j = i_;
                for (; j < s_.size(); ++j) {
                    if (s_[j] == '{')
                        ++depth;
                    else if (s_[j] == '}' && --depth == 0)
                        break;
                }
                if (j >= s_.size())
                    throw ExprError("missing '}'");
                ExprValue v = exprValue(std::string(s_.substr(i_ + 1, j - i_ - 1)));
                i_ = j + 1;
                return v;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
                std::size_t j = i_;
                while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '.' ||
                                         ((s_[j] == '-' || s_[j] == '+') && (s_[j - 1] == 'e' || s_[j - 1] == 'E'))))
                    ++j;
                ExprValue v = exprValue(std::string(s_.substr(i_, j - i_)));
                if (!v.num)
                    throw ExprError("invalid number \"" + v.s + "\"");
                i_ = j;
                return v;
            }
            if (std::isalpha(static_cast<unsigned char>(c))) {
                std::size_t j = i_;
                while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_'))
                    ++j;
                std::string word(s_.substr(i_, j - i_));
                i_ = j;
                skip();
                if (i_ < s_.size() && s_[i_] == '(') {
                    ++i_;
                    std::vector<ExprValue> args;
                    skip();
                    if (i_ < s_.size() && s_[i_] == ')')
                        ++i_;
                    else {
                        for (;;) {
                            args.push_back(ternary());
                            if (eat(")"))
                                break;
                            if (!eat(","))
                                throw ExprError("missing ')' after the arguments of " + word + "()");
                        }
                    }
                    return call(word, args);
                }
                std::string lower;
                for (char ch : word)
                    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (lower == "nan")
                    return numberValue(std::nan(""), false);
                if (lower == "inf")
                    return numberValue(HUGE_VAL, false);
                ExprValue v;
                v.s = word;
                truth(v); // only boolean words are valid barewords
                return v;
            }
            throw ExprError("unexpected '" + std::string(1, c) + "'");
        }
        static ExprValue call(const std::string& f, const std::vector<ExprValue>& a) {
            auto arg = [&](std::size_t k) { return need(a.at(k), f.c_str()); };
            auto count = [&](std::size_t n) {
                if (a.size() != n)
                    throw ExprError("wrong number of arguments to " + f + "()");
            };
            if (f == "min" || f == "max") {
                if (a.empty())
                    throw ExprError(f + "() needs arguments");
                ExprValue r = numberValue(arg(0), a[0].integer);
                for (std::size_t k = 1; k < a.size(); ++k) {
                    const double x = arg(k);
                    if (std::isnan(x) || std::isnan(r.d))
                        r = numberValue(std::nan(""), false); // an unknown value stays unknown
                    else if (f == "min" ? x < r.d : x > r.d)
                        r = numberValue(x, a[k].integer);
                }
                return r;
            }
            if (f == "abs") {
                count(1);
                return numberValue(std::fabs(arg(0)), a[0].integer);
            }
            if (f == "int" || f == "round" || f == "floor" || f == "ceil") {
                count(1);
                const double x = arg(0);
                const double r = f == "int" ? std::trunc(x) : f == "round" ? std::round(x) : f == "floor" ? std::floor(x) : std::ceil(x);
                return numberValue(r, f == "int" || f == "round");
            }
            if (f == "double") {
                count(1);
                return numberValue(arg(0), false);
            }
            if (f == "sqrt") {
                count(1);
                return numberValue(std::sqrt(arg(0)), false);
            }
            if (f == "pow") {
                count(2);
                return numberValue(std::pow(arg(0), arg(1)), false);
            }
            throw ExprError("unknown math function \"" + f + "\"");
        }

        Interp& in_;
        std::string_view s_;
        std::uint32_t line_;
        std::size_t i_ = 0;
    };

    /// Evaluates an expression; reports CON-PARSE and returns nullopt on error.
    std::optional<ExprValue> evalExpr(std::string_view text, std::uint32_t line, const std::string& ctx) {
        try {
            return ExprParser(*this, text, line).parse();
        }
        catch (const ExprError& e) {
            diag("CON-PARSE", Severity::Warning, ctx + ": " + e.what(), line);
            return std::nullopt;
        }
    }

    /// Text of a word as an expression or script: braced words verbatim.
    std::string wordText(const Word& w, std::uint32_t line) {
        return w.kind == Word::Kind::Braced ? w.raw : subst(w, line).text;
    }

    Value runBody(const Word& w, std::uint32_t line) { return runScript(wordText(w, line), w.line ? w.line : line); }

    Value exprCmd(const Command& cmd) {
        std::string text;
        for (std::size_t i = 1; i < cmd.words.size(); ++i) {
            if (i > 1)
                text += ' ';
            text += wordText(cmd.words[i], cmd.line);
        }
        auto v = evalExpr(text, cmd.line, "expr");
        if (!v)
            return {};
        return Value{v->num ? formatNumber(*v) : v->s, {}, false};
    }

    std::optional<bool> condition(const Word& w, std::uint32_t line, const char* ctx) {
        auto v = evalExpr(wordText(w, line), w.line ? w.line : line, ctx);
        if (!v)
            return std::nullopt;
        try {
            return truth(*v);
        }
        catch (const ExprError& e) {
            diag("CON-PARSE", Severity::Warning, std::string(ctx) + ": " + e.what(), line);
            return std::nullopt;
        }
    }

    /// if expr ?then? body ?elseif expr ?then? body ...? ?else? ?body?
    Value ifCmd(const Command& cmd) {
        const auto& w = cmd.words;
        std::size_t i = 1;
        for (;;) {
            if (i >= w.size()) {
                diag("CON-PARSE", Severity::Error, "if: missing condition", cmd.line);
                return {};
            }
            auto c = condition(w[i], cmd.line, "if");
            ++i;
            if (i < w.size() && w[i].kind == Word::Kind::Bare && w[i].raw == "then")
                ++i;
            if (i >= w.size()) {
                diag("CON-PARSE", Severity::Error, "if: missing body", cmd.line);
                return {};
            }
            if (!c)
                return {}; // condition could not be evaluated: nothing runs
            if (*c)
                return runBody(w[i], cmd.line);
            ++i;
            if (i >= w.size())
                return {};
            if (w[i].kind == Word::Kind::Bare && w[i].raw == "elseif") {
                ++i;
                continue;
            }
            if (w[i].kind == Word::Kind::Bare && w[i].raw == "else")
                ++i;
            if (i >= w.size()) {
                diag("CON-PARSE", Severity::Error, "if: missing else body", cmd.line);
                return {};
            }
            return runBody(w[i], cmd.line);
        }
    }

    static constexpr std::size_t kMaxIterations = 100000;

    /// foreach varList list body (one list; several variables per step)
    Value foreachCmd(const Command& cmd) {
        if (cmd.words.size() != 4) {
            diag("CON-UNSUPPORTED-COMMAND", Severity::Warning,
                 "foreach: only the form 'foreach varList list body' is supported; the command was skipped", cmd.line);
            return {};
        }
        const auto names = splitList(subst(cmd.words[1], cmd.line).text);
        if (names.empty()) {
            diag("CON-PARSE", Severity::Error, "foreach: no loop variable", cmd.line);
            return {};
        }
        const Value list = subst(cmd.words[2], cmd.line);
        std::vector<Value> items;
        if (list.hasObjects)
            for (const auto& o : list.objects)
                items.push_back(Value{o.name, {o}, true});
        else
            for (auto& t : splitList(list.text))
                items.push_back(Value{std::move(t), {}, false});
        for (std::size_t k = 0; k < items.size(); k += names.size()) {
            for (std::size_t n = 0; n < names.size(); ++n)
                varRef(names[n]) = k + n < items.size() ? items[k + n] : Value{};
            runBody(cmd.words[3], cmd.line);
            if (ctl_ == Ctl::Return)
                break; // leaves the enclosing procedure
            if (ctl_ == Ctl::Break) {
                ctl_ = Ctl::None;
                break;
            }
            ctl_ = Ctl::None;
        }
        return {};
    }

    /// for start test next body / while test body
    Value loopCmd(const Command& cmd, const std::string& name) {
        const bool isFor = name == "for";
        if (cmd.words.size() != (isFor ? 5U : 3U)) {
            diag("CON-PARSE", Severity::Error, name + ": wrong number of arguments", cmd.line);
            return {};
        }
        if (isFor)
            runBody(cmd.words[1], cmd.line);
        const Word& test = cmd.words[isFor ? 2 : 1];
        const Word& body = cmd.words[isFor ? 4 : 2];
        for (std::size_t n = 0;; ++n) {
            if (n == kMaxIterations) {
                diag("CON-PARSE", Severity::Warning,
                     name + ": stopped after " + std::to_string(kMaxIterations) + " iterations", cmd.line);
                break;
            }
            auto c = condition(test, cmd.line, name.c_str());
            if (!c || !*c)
                break;
            runBody(body, cmd.line);
            if (ctl_ == Ctl::Return)
                break;
            if (ctl_ == Ctl::Break) {
                ctl_ = Ctl::None;
                break;
            }
            ctl_ = Ctl::None;
            if (isFor)
                runBody(cmd.words[3], cmd.line);
        }
        return {};
    }

    Value listCmd(const Command& cmd, const std::string& name) {
        const auto& w = cmd.words;
        if (w.size() < 2) {
            diag("CON-PARSE", Severity::Error, name + ": wrong number of arguments", cmd.line);
            return {};
        }
        if (name == "llength") {
            const Value v = subst(w[1], cmd.line);
            const std::size_t n = v.hasObjects ? v.objects.size() : splitList(v.text).size();
            return Value{std::to_string(n), {}, false};
        }
        if (name == "lindex") {
            const Value v = subst(w[1], cmd.line);
            if (w.size() < 3)
                return v;
            const std::string idx = subst(w[2], cmd.line).text;
            const std::size_t size = v.hasObjects ? v.objects.size() : splitList(v.text).size();
            long long k = -1;
            if (idx == "end")
                k = static_cast<long long>(size) - 1;
            else if (idx.starts_with("end-"))
                k = static_cast<long long>(size) - 1 - std::atoll(idx.c_str() + 4);
            else if (auto n = toNumber(idx))
                k = static_cast<long long>(*n);
            if (k < 0 || static_cast<std::size_t>(k) >= size)
                return {};
            if (v.hasObjects)
                return Value{v.objects[static_cast<std::size_t>(k)].name, {v.objects[static_cast<std::size_t>(k)]}, true};
            return Value{splitList(v.text)[static_cast<std::size_t>(k)], {}, false};
        }
        const std::string var = subst(w[1], cmd.line).text;
        Value& cur = varRef(var);
        if (name == "incr") {
            long long by = 1;
            if (w.size() > 2)
                if (auto n = toNumber(subst(w[2], cmd.line).text))
                    by = static_cast<long long>(*n);
            const auto n = toNumber(cur.text.empty() ? "0" : cur.text);
            if (!n) {
                diag("CON-PARSE", Severity::Warning, "incr: '" + var + "' is not an integer", cmd.line);
                return cur;
            }
            cur = Value{std::to_string(static_cast<long long>(*n) + by), {}, false};
            return cur;
        }
        // lappend
        for (std::size_t i = 2; i < w.size(); ++i) {
            Value v = subst(w[i], cmd.line);
            if (!cur.text.empty())
                cur.text += ' ';
            cur.text += v.text.find_first_of(" \t") != std::string::npos && !v.hasObjects ? "{" + v.text + "}" : v.text;
            if (v.hasObjects) {
                cur.objects.insert(cur.objects.end(), v.objects.begin(), v.objects.end());
                cur.hasObjects = true;
            }
        }
        return cur;
    }

    /// Period of the clocks matching a name pattern (generated clocks through
    /// their master); nullopt when unknown.
    std::optional<double> clockPeriod(const ClockDef& c, int depth = 0) const {
        if (!c.generated)
            return c.periodNs;
        if (depth > 16)
            return std::nullopt;
        const ClockDef* master = nullptr;
        for (const auto& m : out_.clocks) {
            if (&m == &c)
                continue;
            if (!c.masterClock.empty() ? m.name == c.masterClock
                                       : !c.masterSource.empty() &&
                                             std::ranges::find(m.sources, c.masterSource.front()) != m.sources.end()) {
                master = &m;
                break;
            }
        }
        if (!master)
            return std::nullopt;
        auto p = clockPeriod(*master, depth + 1);
        if (!p)
            return std::nullopt;
        return *p * c.divideBy / c.multiplyBy;
    }

    /// get_property ?-min|-max? ?-quiet? name objects
    Value getProperty(const Command& cmd) {
        Args a = parseArgs(cmd, "get_property", {{"-min", false}, {"-max", false}, {"-quiet", false}});
        if (a.positional.size() != 2) {
            diag("CON-PARSE", Severity::Error, "get_property: expected a property name and objects", cmd.line);
            return {};
        }
        const std::string prop = upper(a.positional[0].text);
        const Value& objs = a.positional[1];
        if (!objs.hasObjects) {
            diag("CON-UNSUPPORTED-COMMAND", Severity::Warning,
                 "get_property: '" + objs.text + "' is not the result of an object query", cmd.line);
            return {};
        }
        std::vector<std::string> values;
        bool unknown = false;
        for (const auto& o : objs.objects) {
            if (o.kind == ObjectKind::Clock && prop == "PERIOD") {
                if (!o.bits.empty()) {
                    // get_clocks -of_objects: the clock is known only after
                    // clock analysis; the period is unknown here
                    unknown = true;
                    continue;
                }
                bool found = false;
                for (const auto& c : out_.clocks)
                    if (globMatch(o.name, c.name)) {
                        found = true;
                        if (auto p = clockPeriod(c))
                            values.push_back(formatNumber(numberValue(*p, false)));
                        else
                            unknown = true;
                    }
                if (!found)
                    unknown = true;
                continue;
            }
            if (o.kind == ObjectKind::Cell && resolver_.cellPropertyKnown(prop)) {
                if (auto v = resolver_.cellProperty(o, prop))
                    values.push_back(*v);
                continue;
            }
            diag("CON-UNSUPPORTED-COMMAND", Severity::Warning,
                 "get_property: property " + prop + " of '" + o.name + "' is not available", cmd.line);
            return {};
        }
        if (prop == "PERIOD" && unknown)
            return Value{"nan", {}, false}; // unknown period (only feeds delay values, which are not used)
        if ((a.flags.contains("-min") || a.flags.contains("-max")) && !values.empty()) {
            const bool mn = a.flags.contains("-min");
            std::optional<double> best;
            for (const auto& v : values)
                if (auto n = toNumber(v))
                    best = !best ? *n : mn ? std::min(*best, *n) : std::max(*best, *n);
            if (best)
                return Value{formatNumber(numberValue(*best, false)), {}, false};
        }
        std::string text;
        for (const auto& v : values) {
            if (!text.empty())
                text += ' ';
            text += v;
        }
        return Value{text, {}, false};
    }

    // ------------------------------------------------------------------
    // get_cells
    // ------------------------------------------------------------------

    /// Vivado -filter expressions: PROP op value (==, !=, =~, !~), bare PROP
    /// (boolean), !, &&, || and parentheses. A missing property never matches.
    class FilterParser {
    public:
        using Lookup = std::function<std::optional<std::string>(const std::string&)>;
        using RegexCache = std::unordered_map<std::string, std::regex>;
        /// regexp: =~ and !~ take regular expressions (get_cells -regexp)
        FilterParser(std::string_view s, Lookup lookup, bool regexp = false, RegexCache* cache = nullptr)
            : s_(s), lookup_(std::move(lookup)), regexp_(regexp), cache_(cache) {}

        bool parse() {
            const bool r = orExpr();
            skip();
            if (i_ < s_.size())
                throw ExprError("unexpected '" + std::string(s_.substr(i_)) + "' in filter");
            return r;
        }

    private:
        void skip() {
            while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])))
                ++i_;
        }
        bool eat(std::string_view t) {
            skip();
            if (s_.substr(i_).starts_with(t)) {
                i_ += t.size();
                return true;
            }
            return false;
        }
        bool orExpr() {
            bool r = andExpr();
            while (eat("||")) {
                const bool b = andExpr(); // evaluate every term (property collection)
                r = r || b;
            }
            return r;
        }
        bool andExpr() {
            bool r = notExpr();
            while (eat("&&")) {
                const bool b = notExpr();
                r = r && b;
            }
            return r;
        }
        bool notExpr() {
            skip();
            if (i_ < s_.size() && s_[i_] == '!' && !s_.substr(i_).starts_with("!=") && !s_.substr(i_).starts_with("!~")) {
                ++i_;
                return !notExpr();
            }
            return term();
        }
        static bool boolLike(const std::string& v, bool& out) {
            std::string t = upper(v);
            if (t == "1" || t == "TRUE" || t == "YES") {
                out = true;
                return true;
            }
            if (t == "0" || t == "FALSE" || t == "NO") {
                out = false;
                return true;
            }
            return false;
        }
        bool term() {
            if (eat("(")) {
                const bool r = orExpr();
                if (!eat(")"))
                    throw ExprError("missing ')' in filter");
                return r;
            }
            skip();
            std::size_t j = i_;
            while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_' || s_[j] == '.'))
                ++j;
            if (j == i_)
                throw ExprError("expected a property name in filter at '" + std::string(s_.substr(i_)) + "'");
            const std::string prop(s_.substr(i_, j - i_));
            i_ = j;
            const auto value = lookup_(prop);
            std::string op;
            for (const char* o : {"==", "!=", "=~", "!~"})
                if (eat(o)) {
                    op = o;
                    break;
                }
            if (op.empty()) {
                bool b = false;
                return value && boolLike(*value, b) && b;
            }
            skip();
            std::string rhs;
            if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '{')) {
                const char close = s_[i_] == '"' ? '"' : '}';
                const auto end = s_.find(close, i_ + 1);
                if (end == std::string_view::npos)
                    throw ExprError("unterminated value in filter");
                rhs = std::string(s_.substr(i_ + 1, end - i_ - 1));
                i_ = end + 1;
            }
            else {
                std::size_t k = i_;
                while (k < s_.size() && !std::isspace(static_cast<unsigned char>(s_[k])) && s_[k] != ')' &&
                       !s_.substr(k).starts_with("&&") && !s_.substr(k).starts_with("||"))
                    ++k;
                rhs = std::string(s_.substr(i_, k - i_));
                i_ = k;
            }
            if (op == "=~" || op == "!~") {
                bool hit = false;
                if (regexp_) {
                    const std::regex* re = nullptr;
                    std::regex local;
                    try {
                        if (cache_) {
                            auto it = cache_->find(rhs);
                            if (it == cache_->end())
                                it = cache_->emplace(rhs, std::regex(rhs)).first;
                            re = &it->second;
                        }
                        else {
                            local = std::regex(rhs);
                            re = &local;
                        }
                    }
                    catch (const std::regex_error&) {
                        throw ExprError("invalid regular expression '" + rhs + "' in filter");
                    }
                    hit = value && std::regex_match(*value, *re);
                }
                else {
                    hit = value && globMatch(rhs, *value);
                }
                return value && hit == (op == "=~");
            }
            if (!value)
                return false;
            bool lb = false, rb = false;
            const bool eq = boolLike(*value, lb) && boolLike(rhs, rb) ? lb == rb : *value == rhs;
            return eq == (op == "==");
        }

        std::string_view s_;
        Lookup lookup_;
        bool regexp_ = false;
        RegexCache* cache_ = nullptr;
        std::size_t i_ = 0;
    };

    /// Keeps the objects (cells or pins) that match a -filter expression.
    /// Returns false, with `objs` cleared and a warning, when the filter is
    /// malformed or uses a property that is not available.
    bool applyFilter(std::vector<Object>& objs, const std::string& filter, bool pins, bool regexp,
                     const std::string& ctx, const Command& cmd) {
        std::vector<std::string> props;
        try {
            FilterParser(
                filter,
                [&](const std::string& p) {
                    props.push_back(p);
                    return std::optional<std::string>{};
                },
                regexp, &regexCache_)
                .parse();
            for (const auto& p : props)
                if (!(pins ? resolver_.pinPropertyKnown(p) : resolver_.cellPropertyKnown(p))) {
                    diag("CON-UNSUPPORTED-OPTION", Severity::Warning,
                         ctx + ": -filter property '" + p + "' is not available in the design; the query returns nothing",
                         cmd.line);
                    objs.clear();
                    return false;
                }
            std::erase_if(objs, [&](const Object& o) {
                return !FilterParser(
                            filter,
                            [&](const std::string& p) {
                                return pins ? resolver_.pinProperty(o, p) : resolver_.cellProperty(o, p);
                            },
                            regexp, &regexCache_)
                            .parse();
            });
        }
        catch (const ExprError& e) {
            diag("CON-PARSE", Severity::Warning, ctx + ": " + e.what(), cmd.line);
            objs.clear();
            return false;
        }
        return true;
    }

    Value getCells(const Command& cmd) {
        Args a = parseArgs(cmd, "get_cells",
                           {{"-quiet", false}, {"-nocase", false}, {"-regexp", false}, {"-filter", true},
                            {"-of_objects", true}, {"-hierarchical", false}});
        const bool quiet = a.flags.contains("-quiet");
        const bool nocase = a.flags.contains("-nocase");
        const bool hier = a.flags.contains("-hierarchical");
        const bool regexp = a.flags.contains("-regexp");
        const std::string filter = a.opts.contains("-filter") ? a.opts["-filter"].back().text : std::string();
        std::vector<Object> result;
        if (a.opts.contains("-of_objects")) {
            // the cells of pins
            bool unsupportedKind = false;
            for (const auto& ov : a.opts["-of_objects"])
                for (const auto& o : objectsOf(ov, cmd, "get_cells -of_objects")) {
                    if (o.kind != ObjectKind::Pin) {
                        unsupportedKind = true;
                        continue;
                    }
                    for (auto& c : resolver_.pinCells(o))
                        if (std::ranges::find(result, c.name, &Object::name) == result.end())
                            result.push_back(std::move(c));
                }
            if (unsupportedKind)
                diag("CON-UNSUPPORTED-OPTION", Severity::Warning,
                     "get_cells: -of_objects supports pins; other objects contribute nothing", cmd.line);
            if (!filter.empty())
                applyFilter(result, filter, false, regexp, "get_cells", cmd);
            return objectsValue(std::move(result));
        }
        std::vector<std::string> patterns;
        for (const auto& pv : a.positional) {
            if (pv.hasObjects)
                for (const auto& o : pv.objects)
                    patterns.push_back(o.name);
            else
                for (auto& p : splitList(pv.text))
                    patterns.push_back(std::move(p));
        }
        if (patterns.empty())
            patterns.emplace_back(regexp ? ".*" : "*");
        for (const auto& pat : patterns) {
            std::vector<Object> r;
            if (regexp) {
                try {
                    r = resolver_.cellsRegexp(pat, hier, nocase);
                }
                catch (const std::regex_error&) {
                    diag("CON-PARSE", Severity::Error, "get_cells: invalid regular expression '" + pat + "'", cmd.line);
                    continue;
                }
            }
            else {
                r = resolver_.cells(pat, hier, nocase);
                if (r.empty() && pat.find('|') != std::string::npos)
                    r = resolver_.quartus(pat, QuartusKind::Cells, nocase); // a Quartus name held in a variable
            }
            if (!filter.empty() && !applyFilter(r, filter, false, regexp, "get_cells", cmd))
                return objectsValue({});
            if (r.empty() && !quiet)
                diag("CON-UNMATCHED", Severity::Warning,
                     "get_cells: nothing matches '" + pat + "'" + (filter.empty() ? "" : " with filter {" + filter + "}"),
                     cmd.line);
            result.insert(result.end(), r.begin(), r.end());
        }
        return objectsValue(std::move(result));
    }

    // ------------------------------------------------------------------
    // proc / return / global / source
    // ------------------------------------------------------------------

    struct Proc {
        std::vector<std::pair<std::string, std::optional<std::string>>> params; ///< name, default
        std::string body;
        std::uint32_t line = 0;
        std::string file;
    };

    static constexpr std::size_t kMaxCallDepth = 64;

    /// proc name params body
    Value procCmd(const Command& cmd) {
        if (cmd.words.size() != 4) {
            diag("CON-PARSE", Severity::Error, "proc: expected 'proc name params body'", cmd.line);
            return {};
        }
        Proc p;
        const std::string name = subst(cmd.words[1], cmd.line).text;
        for (const auto& param : splitList(wordText(cmd.words[2], cmd.line))) {
            const auto parts = splitList(param);
            if (parts.empty())
                continue;
            p.params.emplace_back(parts[0], parts.size() > 1 ? std::optional<std::string>(parts[1]) : std::nullopt);
        }
        p.body = wordText(cmd.words[3], cmd.line);
        p.line = cmd.words[3].line ? cmd.words[3].line : cmd.line;
        p.file = file_;
        procs_[name] = std::move(p);
        return {};
    }

    /// Calls a procedure: arguments bound in a new frame, the body run in the
    /// file that defined it; the value is that of `return` or of the last command.
    Value callProc(const Command& cmd, const std::string& name, const Proc& p) {
        if (frames_.size() > kMaxCallDepth) {
            diag("CON-PARSE", Severity::Error, name + ": too many nested procedure calls", cmd.line);
            return {};
        }
        std::vector<Value> args;
        for (std::size_t i = 1; i < cmd.words.size(); ++i)
            args.push_back(subst(cmd.words[i], cmd.line));
        Frame frame;
        std::size_t k = 0;
        for (std::size_t i = 0; i < p.params.size(); ++i) {
            const auto& [pname, def] = p.params[i];
            if (pname == "args" && i + 1 == p.params.size()) {
                Value rest;
                for (; k < args.size(); ++k) {
                    if (!rest.text.empty())
                        rest.text += ' ';
                    rest.text += args[k].text;
                    if (args[k].hasObjects) {
                        rest.objects.insert(rest.objects.end(), args[k].objects.begin(), args[k].objects.end());
                        rest.hasObjects = true;
                    }
                }
                frame.vars["args"] = std::move(rest);
                continue;
            }
            if (k < args.size())
                frame.vars[pname] = args[k++];
            else if (def)
                frame.vars[pname] = Value{*def, {}, false};
            else {
                diag("CON-PARSE", Severity::Error, "wrong # args: " + name + " needs '" + pname + "'", cmd.line);
                return {};
            }
        }
        if (k < args.size()) {
            diag("CON-PARSE", Severity::Error,
                 "wrong # args: " + name + " takes " + std::to_string(p.params.size()) + " argument(s)", cmd.line);
            return {};
        }
        frames_.push_back(std::move(frame));
        const std::string caller = file_;
        file_ = p.file;
        Value result = runScript(p.body, p.line);
        file_ = caller;
        frames_.pop_back();
        if (ctl_ == Ctl::Return)
            result = std::move(returnValue_);
        ctl_ = Ctl::None; // break/continue do not leave a procedure
        return result;
    }

    Value returnCmd(const Command& cmd) {
        // return ?-code c? ?value?
        returnValue_ = {};
        for (std::size_t i = 1; i < cmd.words.size(); ++i) {
            if (cmd.words[i].kind == Word::Kind::Bare && cmd.words[i].raw == "-code" && i + 1 < cmd.words.size()) {
                ++i;
                continue;
            }
            returnValue_ = subst(cmd.words[i], cmd.line);
        }
        ctl_ = Ctl::Return;
        return returnValue_;
    }

    Value globalCmd(const Command& cmd) {
        if (frames_.size() > 1)
            for (std::size_t i = 1; i < cmd.words.size(); ++i)
                frames_.back().globals.insert(subst(cmd.words[i], cmd.line).text);
        return {};
    }

    static constexpr std::size_t kMaxSourceDepth = 16;

    /// source ?-encoding name? file: relative paths are searched in the
    /// directory of the current file, then in SdcOptions::sourceDirs.
    Value sourceCmd(const Command& cmd) {
        std::string name;
        for (std::size_t i = 1; i < cmd.words.size(); ++i) {
            if (cmd.words[i].kind == Word::Kind::Bare && cmd.words[i].raw == "-encoding") {
                ++i;
                continue;
            }
            name = subst(cmd.words[i], cmd.line).text;
        }
        if (name.empty()) {
            diag("CON-PARSE", Severity::Error, "source: no file given", cmd.line);
            return {};
        }
        if (sourceDepth_ >= kMaxSourceDepth) {
            diag("CON-PARSE", Severity::Error, "source: files source each other too deeply", cmd.line);
            return {};
        }
        const std::filesystem::path rel = fromUtf8(name);
        std::vector<std::filesystem::path> candidates;
        if (rel.is_absolute())
            candidates.push_back(rel);
        else {
            candidates.push_back(fromUtf8(file_).parent_path() / rel);
            for (const auto& d : options_.sourceDirs)
                candidates.push_back(d / rel);
        }
        std::error_code ec;
        for (const auto& c : candidates) {
            if (!std::filesystem::is_regular_file(c, ec))
                continue;
            std::ifstream in(c, std::ios::binary);
            if (!in)
                continue;
            std::stringstream text;
            text << in.rdbuf();
            const auto path = std::filesystem::weakly_canonical(c, ec);
            out_.files.push_back(ec ? c : path);
            const std::string caller = file_;
            file_ = toUtf8(ec ? c : path);
            ++sourceDepth_;
            Value v = runScript(text.str(), 1);
            --sourceDepth_;
            file_ = caller;
            if (ctl_ == Ctl::Return) {
                v = std::move(returnValue_);
                ctl_ = Ctl::None; // `return` at file level ends the sourced file
            }
            return v;
        }
        std::string searched;
        for (const auto& c : candidates)
            searched += (searched.empty() ? "" : ", ") + toUtf8(c.lexically_normal());
        diag("CON-OPEN", Severity::Warning,
             "source: cannot find '" + name + "' (searched " + searched +
                 "); add the directory the scripts are run from to the project's tclSourceDirs",
             cmd.line);
        return {};
    }

    // ------------------------------------------------------------------
    // Quartus collections
    // ------------------------------------------------------------------

    /// get_cells / get_pins written with Quartus names: a pattern containing
    /// '|' (never part of a Vivado name; -regexp patterns and filters excluded)
    static bool quartusNames(const Command& cmd) {
        bool skipNext = false;
        for (std::size_t i = 1; i < cmd.words.size(); ++i) {
            const auto& w = cmd.words[i];
            if (skipNext) {
                skipNext = false;
                continue;
            }
            if (w.kind == Word::Kind::Bare && w.raw.starts_with('-')) {
                if (w.raw == "-regexp")
                    return false;
                skipNext = w.raw == "-filter" || w.raw.starts_with("-of");
                continue;
            }
            if (w.raw.find('|') != std::string::npos)
                return true;
        }
        return false;
    }

    /// get_registers / get_keepers / get_nodes / get_cells|get_pins with
    /// Quartus names ('|' separated).
    Value quartusQuery(const Command& cmd, const std::string& name) {
        Args a = parseArgs(cmd, name,
                           {{"-nowarn", false}, {"-nocase", false}, {"-no_duplicates", false}, {"-compatibility_mode", false},
                            {"-hierarchical", false}, {"-quiet", false}});
        const bool nowarn = a.flags.contains("-nowarn") || a.flags.contains("-quiet");
        const bool nocase = a.flags.contains("-nocase");
        QuartusKind kind = name == "get_registers" ? QuartusKind::Registers
                           : name == "get_keepers" ? QuartusKind::Keepers
                           : name == "get_nodes"   ? QuartusKind::Nodes
                           : name == "get_pins"    ? QuartusKind::Pins
                                                   : QuartusKind::Cells;
        std::vector<std::string> patterns;
        for (const auto& pv : a.positional) {
            if (pv.hasObjects)
                for (const auto& o : pv.objects)
                    patterns.push_back(o.name);
            else
                for (auto& p : splitList(pv.text))
                    patterns.push_back(std::move(p));
        }
        if (patterns.empty())
            patterns.emplace_back("*");
        std::vector<Object> result;
        for (const auto& pat : patterns) {
            auto r = resolver_.quartus(pat, kind, nocase);
            if (r.empty() && !nowarn)
                unmatched(name, pat, cmd.line);
            for (auto& o : r)
                if (std::ranges::find(result, o.name, &Object::name) == result.end())
                    result.push_back(std::move(o));
        }
        return objectsValue(std::move(result));
    }

    /// foreach_in_collection var collection body
    Value foreachInCollection(const Command& cmd) {
        if (cmd.words.size() != 4) {
            diag("CON-PARSE", Severity::Error, "foreach_in_collection: expected 'var collection body'", cmd.line);
            return {};
        }
        const std::string var = subst(cmd.words[1], cmd.line).text;
        const Value coll = subst(cmd.words[2], cmd.line);
        if (!coll.hasObjects) {
            diag("CON-PARSE", Severity::Warning, "foreach_in_collection: '" + coll.text + "' is not a collection", cmd.line);
            return {};
        }
        for (const auto& o : coll.objects) {
            varRef(var) = Value{o.name, {o}, true};
            runBody(cmd.words[3], cmd.line);
            if (ctl_ == Ctl::Break) {
                ctl_ = Ctl::None;
                break;
            }
            if (ctl_ == Ctl::Continue)
                ctl_ = Ctl::None;
            if (ctl_ == Ctl::Return)
                break;
        }
        return {};
    }

    /// get_collection_size, get_node_info/get_object_info -name,
    /// query_collection, add_to_collection
    Value collectionCmd(const Command& cmd, const std::string& name) {
        if (name == "get_collection_size") {
            if (cmd.words.size() != 2) {
                diag("CON-PARSE", Severity::Error, "get_collection_size: expected one collection", cmd.line);
                return {};
            }
            const Value v = subst(cmd.words[1], cmd.line);
            return Value{std::to_string(v.hasObjects ? v.objects.size() : splitList(v.text).size()), {}, false};
        }
        if (name == "get_node_info" || name == "get_object_info") {
            Args a = parseArgs(cmd, name, {{"-name", false}, {"-type", false}});
            if (!a.flags.contains("-name") || a.positional.size() != 1) {
                diag("CON-UNSUPPORTED-COMMAND", Severity::Warning, name + ": only '-name <object>' is supported", cmd.line);
                return {};
            }
            const auto& v = a.positional[0];
            return Value{v.hasObjects && !v.objects.empty() ? v.objects.front().name : v.text, {}, false};
        }
        if (name == "query_collection") {
            Args a = parseArgs(cmd, name, {{"-all", false}, {"-list_format", false}, {"-report_format", false}, {"-limit", true}});
            std::string text;
            for (const auto& v : a.positional)
                for (const auto& o : v.objects)
                    text += (text.empty() ? "" : " ") + o.name;
            return Value{text, {}, false};
        }
        // add_to_collection a b
        Value out;
        for (std::size_t i = 1; i < cmd.words.size(); ++i) {
            Value v = subst(cmd.words[i], cmd.line);
            for (auto& o : v.objects)
                if (std::ranges::find(out.objects, o.name, &Object::name) == out.objects.end())
                    out.objects.push_back(std::move(o));
        }
        return objectsValue(std::move(out.objects));
    }

    /// An object query that matched nothing. Quartus's reserved JTAG ports
    /// (altera_reserved_*) exist only after fitting: noted, not warned.
    void unmatched(const std::string& ctx, const std::string& pattern, std::uint32_t line) {
        if (pattern.starts_with("altera_reserved_"))
            diag("CON-UNMATCHED", Severity::Info,
                 ctx + ": '" + pattern + "' is a Quartus reserved JTAG port added during fitting, not part of the RTL",
                 line);
        else
            diag("CON-UNMATCHED", Severity::Warning, ctx + ": nothing matches '" + pattern + "'", line);
    }

    Value exec(const Command& cmd) {
        const Value head = subst(cmd.words[0], cmd.line);
        const std::string& name = head.text;
        if (name == "set") {
            if (cmd.words.size() == 3) {
                Value v = subst(cmd.words[2], cmd.line);
                varRef(subst(cmd.words[1], cmd.line).text) = v;
                return v;
            }
            if (cmd.words.size() == 2) {
                const Value* var = findVar(subst(cmd.words[1], cmd.line).text);
                return var ? *var : Value{};
            }
            diag("CON-PARSE", Severity::Error, "set: wrong number of arguments", cmd.line);
            return {};
        }
        if (name == "proc")
            return procCmd(cmd);
        if (name == "return")
            return returnCmd(cmd);
        if (name == "global")
            return globalCmd(cmd);
        if (name == "source")
            return sourceCmd(cmd);
        if (name == "get_registers" || name == "get_keepers" || name == "get_nodes" ||
            ((name == "get_cells" || name == "get_pins") && quartusNames(cmd)))
            return quartusQuery(cmd, name);
        if (name == "foreach_in_collection")
            return foreachInCollection(cmd);
        if (name == "get_collection_size" || name == "get_node_info" || name == "get_object_info" ||
            name == "query_collection" || name == "add_to_collection")
            return collectionCmd(cmd, name);
        if (name == "create_clock")
            return createClock(cmd);
        if (name == "create_generated_clock")
            return createGeneratedClock(cmd);
        if (name == "set_input_delay")
            return setInputDelay(cmd);
        if (name == "set_clock_groups")
            return setClockGroups(cmd);
        if (name == "get_ports" || name == "get_pins" || name == "get_nets" || name == "get_clocks")
            return getObjects(cmd, name);
        if (name == "all_clocks")
            return objectsValue({{ObjectKind::Clock, "*", {}}});
        if (name == "list" || name == "concat") {
            Value v;
            for (std::size_t i = 1; i < cmd.words.size(); ++i) {
                Value w = subst(cmd.words[i], cmd.line);
                if (!v.text.empty())
                    v.text += ' ';
                v.text += cmd.words[i].kind == Word::Kind::Braced && name == "list" ? "{" + w.text + "}" : w.text;
                if (w.hasObjects) {
                    v.objects.insert(v.objects.end(), w.objects.begin(), w.objects.end());
                    v.hasObjects = true;
                }
            }
            return v;
        }
        if (name == "get_cells")
            return getCells(cmd);
        if (name == "expr")
            return exprCmd(cmd);
        if (name == "if")
            return ifCmd(cmd);
        if (name == "foreach")
            return foreachCmd(cmd);
        if (name == "for" || name == "while")
            return loopCmd(cmd, name);
        if (name == "break" || name == "continue") {
            ctl_ = name == "break" ? Ctl::Break : Ctl::Continue;
            return {};
        }
        if (name == "incr" || name == "llength" || name == "lindex" || name == "lappend")
            return listCmd(cmd, name);
        if (name == "puts")
            return {}; // progress messages of vendor scripts
        if (name == "get_property")
            return getProperty(cmd);
        if (name == "all_inputs" || name == "all_outputs" || name == "all_registers") {
            diag("CON-UNSUPPORTED-COMMAND", Severity::Warning, name + " is not supported; it returns nothing", cmd.line);
            return objectsValue({});
        }
        if (auto p = procs_.find(name); p != procs_.end()) {
            const Proc proc = p->second; // the body may redefine the procedure
            return callProc(cmd, name, proc);
        }
        if (name == "switch" || name == "eval" || name == "upvar" || name == "uplevel" || name == "namespace") {
            diag("CON-UNSUPPORTED-COMMAND", Severity::Warning,
                 "Tcl command '" + name + "' is not supported; the command was skipped", cmd.line);
            return {};
        }
        if (name == "set_false_path" || name == "set_max_delay") {
            if (clockException(cmd, name))
                return {};
        }
        if (isTimingOrMiscCommand(name)) {
            ++ignored_[name];
            return {};
        }
        diag("CON-UNKNOWN-COMMAND", Severity::Warning, "unknown command '" + name + "' skipped", cmd.line);
        return {};
    }

    std::string file_; ///< the file being run (diagnostics; base of relative `source` paths)
    IObjectResolver& resolver_;
    ConstraintSet& out_;
    DiagnosticSink& sink_;
    /// Tcl variable frames: the global frame and one per procedure call
    struct Frame {
        std::unordered_map<std::string, Value> vars;
        std::set<std::string, std::less<>> globals; ///< names linked to the global frame (`global`)
    };
    std::vector<Frame> frames_ = std::vector<Frame>(1);

    std::unordered_map<std::string, Value>& frameOf(const std::string& name) {
        Frame& f = frames_.back();
        return frames_.size() > 1 && f.globals.contains(name) ? frames_.front().vars : f.vars;
    }
    const Value* findVar(const std::string& name) {
        auto& vars = frameOf(name);
        auto it = vars.find(name);
        return it == vars.end() ? nullptr : &it->second;
    }
    Value& varRef(const std::string& name) { return frameOf(name)[name]; }
    std::map<std::string, int> ignored_;
    enum class Ctl : std::uint8_t { None, Break, Continue, Return };
    Ctl ctl_ = Ctl::None;
    Value returnValue_;                        ///< value of the pending `return`
    std::unordered_map<std::string, Proc> procs_;
    std::size_t sourceDepth_ = 0;
    SdcOptions options_;
    bool quiet_ = false; ///< evaluating a command given -quiet
    std::unordered_map<std::string, std::regex> regexCache_; ///< -regexp filters
};

} // namespace

struct SdcReader::Impl {
    Interp interp;
    ConstraintSet& out;
    DiagnosticSink& sink;
    Impl(IObjectResolver& resolver, ConstraintSet& o, DiagnosticSink& s, SdcOptions options)
        : interp(resolver, o, s, std::move(options)), out(o), sink(s) {}
};

SdcReader::SdcReader(IObjectResolver& resolver, ConstraintSet& out, DiagnosticSink& sink, SdcOptions options)
    : impl_(std::make_unique<Impl>(resolver, out, sink, std::move(options))) {}

SdcReader::~SdcReader() = default;

void SdcReader::parse(std::string_view text, const std::string& fileName) { impl_->interp.runFile(text, fileName); }

bool SdcReader::readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        impl_->sink.report({"CON-OPEN", Severity::Error, "cannot open constraint file", {toUtf8(file), 0, 0}, {}});
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    impl_->out.files.push_back(file);
    parse(text.str(), toUtf8(file));
    return true;
}

void parseSdc(std::string_view text, const std::string& fileName, IObjectResolver& resolver, ConstraintSet& out,
              DiagnosticSink& sink) {
    SdcReader(resolver, out, sink).parse(text, fileName);
}

bool readSdcFile(const std::filesystem::path& file, IObjectResolver& resolver, ConstraintSet& out,
                 DiagnosticSink& sink) {
    return SdcReader(resolver, out, sink).readFile(file);
}

} // namespace opencdc::constraints

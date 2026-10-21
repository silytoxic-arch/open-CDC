#include "engine/ResultJson.h"

#include "core/PathUtil.h"
#include "core/Version.h"
#include "ir/Dump.h"
#include "rules/RuleEngine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <ostream>
#include <sstream>
#include <vector>

namespace opencdc::engine {

namespace {

using nlohmann::json;

/// Streams JSON with the same layout as `json::dump(indent)`: members are
/// written by callbacks so that large arrays need not exist as one DOM.
class JsonWriter {
public:
    using Body = std::function<void(int level)>;
    using Emit = std::function<void(const json&)>;
    struct Member {
        std::string key;
        Body body;
    };

    JsonWriter(std::ostream& os, int indent) : os_(os), indent_(indent) {}

    void value(int level, const json& v) {
        // Invalid UTF-8 is replaced instead of throwing (source text may be Latin-1/GBK).
        const std::string text = v.dump(indent_, ' ', false, json::error_handler_t::replace);
        if (indent_ < 0 || level == 0) {
            os_ << text;
            return;
        }
        const std::string pad = "\n" + std::string(static_cast<std::size_t>(indent_ * level), ' ');
        // Raw newlines only occur as layout (strings escape them).
        std::size_t from = 0;
        for (auto nl = text.find('\n'); nl != std::string::npos; nl = text.find('\n', from)) {
            os_.write(text.data() + from, static_cast<std::streamsize>(nl - from));
            os_ << pad;
            from = nl + 1;
        }
        os_.write(text.data() + from, static_cast<std::streamsize>(text.size() - from));
    }

    void object(int level, const std::vector<Member>& members) {
        if (members.empty()) {
            os_ << "{}";
            return;
        }
        os_ << '{';
        bool first = true;
        for (const auto& m : members) {
            open(level, first);
            os_ << json(m.key).dump() << (indent_ < 0 ? ":" : ": ");
            m.body(level + 1);
        }
        close(level, '}');
    }

    void array(int level, const std::function<void(const Emit&)>& produce) {
        bool first = true;
        produce([&](const json& v) {
            if (first)
                os_ << '['; // written lazily, so that an empty array stays "[]"
            open(level, first);
            value(level + 1, v);
        });
        if (first)
            os_ << "[]";
        else
            close(level, ']');
    }

private:
    void open(int level, bool& first) {
        if (!first)
            os_ << ',';
        first = false;
        if (indent_ >= 0)
            os_ << '\n' << std::string(static_cast<std::size_t>(indent_ * (level + 1)), ' ');
    }
    void close(int level, char bracket) {
        if (indent_ >= 0)
            os_ << '\n' << std::string(static_cast<std::size_t>(indent_ * level), ' ');
        os_ << bracket;
    }

    std::ostream& os_;
    int indent_;
};

json locationJson(const SourceLocation& loc) {
    if (loc.file.empty())
        return nullptr;
    return {{"file", loc.file}, {"line", loc.line}, {"column", loc.column}};
}

json instanceJson(const frontend::InstanceInfo& inst) {
    json children = json::array();
    for (const auto& c : inst.children)
        children.push_back(instanceJson(c));
    return {{"name", inst.name},
            {"definition", inst.definition},
            {"hierPath", inst.hierPath},
            {"location", locationJson(inst.location)},
            {"children", std::move(children)}};
}

json registerJson(const ir::Design& d, const ir::InstanceNode& inst, const ir::Register& r,
                  const clock::ClockModel* clocks) {
    const auto& b = d.body(inst.body);
    json resets = json::array();
    for (const auto& a : r.asyncResets)
        resets.push_back({{"signal", ir::formatSig(b, a.signal)},
                          {"activeHigh", a.activeHigh},
                          {"value", ir::formatSig(b, a.value)}});
    json reg = {{"instance", inst.hierPath},
                {"q", ir::formatSig(b, r.q)},
                {"width", r.q.width()},
                {"clock", ir::formatSig(b, r.clock.signal)},
                {"edge", r.clock.edge == ir::Edge::Pos ? "posedge" : "negedge"},
                {"asyncResets", std::move(resets)},
                {"location", locationJson(r.origin)}};
    reg["syncReset"] = r.syncReset ? json{{"signal", ir::formatSig(b, r.syncReset->signal)},
                                          {"activeHigh", r.syncReset->activeHigh},
                                          {"value", ir::formatSig(b, r.syncReset->value)}}
                                   : json(nullptr);
    reg["enable"] = r.enable ? json(ir::formatSig(b, *r.enable)) : json(nullptr);
    if (clocks) {
        const auto global =
            static_cast<std::size_t>(inst.registerBase) + static_cast<std::size_t>(&r - b.registers.data());
        const auto& rc = clocks->registers.at(global);
        json cl = json::array();
        for (const auto& ce : rc.clocks)
            cl.push_back({{"clock", clocks->clock(ce.clock).name},
                          {"edge", ce.edge == ir::Edge::Pos ? "posedge" : "negedge"}});
        reg["domain"] = {{"status", rc.status == clock::ClockStatus::Resolved   ? "resolved"
                                    : rc.status == clock::ClockStatus::Constant ? "constant"
                                                                               : "unresolved"},
                         {"clocks", std::move(cl)},
                         {"gated", rc.gated},
                         {"muxed", rc.muxed},
                         {"reason", rc.reason}};
    }
    return reg;
}

/// Flattened register list (one entry per register per instance) is capped.
constexpr std::size_t kMaxRegisters = 100000;

void writeRtl(JsonWriter& w, int level, const ir::Design& d, const clock::ClockModel* clocks) {
    const auto st = d.stats();
    bool truncated = false;
    w.object(level, {
        {"registers",
         [&](int l) {
             std::size_t n = 0;
             w.array(l, [&](const JsonWriter::Emit& emit) {
                 for (const auto& inst : d.instances)
                     for (const auto& r : d.body(inst.body).registers) {
                         if (n >= kMaxRegisters) {
                             truncated = true;
                             return;
                         }
                         ++n;
                         emit(registerJson(d, inst, r, clocks));
                     }
             });
         }},
        {"registersTruncated", [&](int l) { w.value(l, truncated); }},
        {"stats",
         [&](int l) {
             w.value(l, json{{"uniqueBodies", st.uniqueBodies}, {"instances", st.instances},
                             {"nets", st.nets},                 {"cells", st.cells},
                             {"registers", st.registers},       {"registerBits", st.registerBits},
                             {"latches", st.latches},           {"unsupported", st.unsupported}});
         }},
        {"unsupported",
         [&](int l) {
             json unsupported = json::array();
             for (const auto& u : d.unsupported)
                 unsupported.push_back(
                     {{"code", u.code}, {"module", u.module}, {"what", u.what}, {"location", locationJson(u.loc)}});
             w.value(l, unsupported);
         }},
    });
}

json clocksJson(const clock::ClockModel& m, const constraints::ConstraintSet* cons, UnknownClockPolicy policy) {
    json j;
    json list = json::array();
    for (const auto& c : m.clocks) {
        json e = {{"name", c.name},
                  {"kind", clock::toString(c.kind)},
                  {"source", c.sourceName},
                  {"master", c.master.valid() ? json(m.clock(c.master).name) : json(nullptr)},
                  {"periodNs", c.periodNs ? json(*c.periodNs) : json(nullptr)},
                  {"frequencyMHz", c.periodNs && *c.periodNs > 0 ? json(1000.0 / *c.periodNs) : json(nullptr)},
                  {"waveform", c.waveform},
                  {"definedAt", locationJson(c.definedAt)},
                  {"posedgeRegisters", c.posedgeRegisters},
                  {"negedgeRegisters", c.negedgeRegisters},
                  {"gatedRegisters", c.gatedRegisters}};
        list.push_back(std::move(e));
    }
    j["clocks"] = std::move(list);
    json rel = json::array();
    for (std::size_t i = 0; i < m.clocks.size(); ++i)
        for (std::size_t k = i + 1; k < m.clocks.size(); ++k) {
            const clock::ClockId a{static_cast<std::uint32_t>(i)};
            const clock::ClockId b{static_cast<std::uint32_t>(k)};
            rel.push_back({{"a", m.clocks[i].name},
                           {"b", m.clocks[k].name},
                           {"relation", clock::toString(m.relation(a, b))},
                           {"reason", m.reason(a, b)}});
        }
    j["relations"] = std::move(rel);
    j["unknownRelationPolicy"] = toString(policy);
    j["registersResolved"] = m.resolvedRegisters();
    j["registersUnresolved"] = m.unresolvedRegisters();
    j["latches"] = m.latchCount;
    if (cons) {
        json files = json::array();
        for (const auto& f : cons->files)
            files.push_back(toUtf8(f));
        j["constraints"] = {{"files", std::move(files)},
                            {"clockDefinitions", cons->clocks.size()},
                            {"clockGroups", cons->groups.size()},
                            {"inputDelays", cons->inputDelays.size()},
                            {"ignoredCommands", cons->ignoredCommands}};
    }
    return j;
}

json pathJson(const cdc::CdcResult& c, const cdc::CdcPath& p) {
    json conds = json::array();
    for (const auto& cd : p.conditions)
        conds.push_back({{"text", cd.text}, {"satisfied", cd.satisfied}});
    json steps = json::array();
    for (const auto& s : p.steps)
        steps.push_back({{"name", s.name}, {"kind", s.kind}, {"location", locationJson(s.loc)}});
    json hits = json::array();
    for (const auto& h : p.hits)
        hits.push_back({{"rule", h.ruleId},
                        {"severity", toString(h.severity)},
                        {"status", cdc::toString(h.status)},
                        {"message", h.message},
                        {"waiver", h.waiver.empty() ? nlohmann::json(nullptr) : nlohmann::json(h.waiver)}});
    json pj = {{"id", p.id},
                     {"fingerprint", p.fingerprint},
                     {"sourceClock", p.srcClockName},
                     {"destinationClock", p.dstClockName},
                     {"clockRelation", clock::toString(p.relation)},
                     {"sourceRegister", p.srcName},
                     {"sourceKind", cdc::toString(c.nodes.at(p.srcNode).kind)},
                     {"destinationRegister", p.dstName},
                     {"crossingSignal", p.crossingSignal},
                     {"pin", cdc::toString(p.pin)},
                     {"width", p.width},
                     {"class", cdc::toString(p.cls)},
                     {"throughLogic", p.throughLogic},
                     {"synchronizerType", cdc::toString(p.syncType)},
                     {"synchronizerStages", p.syncStages},
                     {"synchronizerStatus", cdc::toString(p.syncStatus)},
                     {"synchronizerRegisters", p.syncRegisters},
                     {"conditions", std::move(conds)},
                     {"unverifiedConditions", p.unverified},
                     {"notes", p.notes},
                     {"hierarchicalPath", std::move(steps)},
                     {"sourceLocation", locationJson(p.srcLoc)},
                     {"destinationLocation", locationJson(p.dstLoc)},
                     {"ruleHits", std::move(hits)},
                     {"status", cdc::toString(p.status)}};
    pj["waived"] = p.waived;
    pj["sourceUnclocked"] = p.srcUnclocked;
    pj["groupSize"] = p.groupSize;
    pj["groupMembers"] = p.groupMembers;
    pj["sourceMayPulse"] = p.sourceMayPulse;
    pj["bitsReadTogether"] = p.bitsCombined;
    pj["splitWith"] = p.splitWith;
    pj["splitBits"] = p.splitBits;
    pj["mixedClocks"] = p.mixedClocks;
    json rv = json::array();
    for (const auto& r : p.reconvergences)
        rv.push_back({{"at", r.at},
                      {"location", locationJson(r.loc)},
                      {"depth", r.depth},
                      {"paths", r.paths},
                      {"partners", r.partners}});
    pj["reconvergences"] = std::move(rv);
    pj["structure"] = p.structure < c.structures.size() ? json(p.structure) : json(nullptr);
    return pj;
}

json cdcSmall(const cdc::CdcResult& c) {
    json j;
    const auto& st = c.stats;
    j["stats"] = {{"paths", st.paths},
                  {"synchronized", st.synchronized},
                  {"unsynchronized", st.unsynchronized},
                  {"synchronizers", st.synchronizers},
                  {"unknownRelation", st.unknownRelation},
                  {"excludedNoDomainSources", st.excludedNoDomainSources},
                  {"excludedUnclockedDestinations", st.excludedUnclockedDestinations},
                  {"asyncResetCrossings", st.asyncResetCrossings},
                  {"resetSynchronizers", st.resetSynchronizers},
                  {"reconvergencePoints", st.reconvergencePoints},
                  {"waivedHits", st.waivedHits},
                  {"combLoops", st.combLoops}};
    json byRule = json::object();
    for (const auto& [id, n] : rules::countHits(c))
        byRule[id] = n;
    j["hitsByRule"] = std::move(byRule);

    json rulesMeta = json::array();
    const auto registry = rules::RuleRegistry::standard();
    for (const auto& r : registry.rules()) {
        const auto& m = r->meta();
        rulesMeta.push_back({{"id", m.id},
                             {"title", m.title},
                             {"defaultSeverity", toString(m.defaultSeverity)},
                             {"fixSuggestion", m.fixSuggestion}});
    }
    j["rules"] = std::move(rulesMeta);


    json structures = json::array();
    for (const auto& s : c.structures)
        structures.push_back({{"id", s.id},
                              {"type", cdc::toString(s.type)},
                              {"name", s.name},
                              {"sourceClock", s.srcClock},
                              {"destinationClock", s.dstClock},
                              {"status", cdc::toString(s.status)},
                              {"paths", s.paths},
                              {"registers", s.registers},
                              {"notes", s.notes}});
    j["structures"] = std::move(structures);

    json waivers = json::array();
    for (const auto& w : c.waivers)
        waivers.push_back({{"id", w.id},
                           {"rule", w.rule},
                           {"fingerprint", w.fingerprint},
                           {"source", w.source},
                           {"destination", w.destination},
                           {"reason", w.reason},
                           {"author", w.author},
                           {"expires", w.expires},
                           {"location", locationJson(w.loc)},
                           {"expired", w.expired},
                           {"uses", w.uses}});
    j["waivers"] = std::move(waivers);
    return j;
}

void writeCdc(JsonWriter& w, int level, const cdc::CdcResult& c) {
    const json small = cdcSmall(c);
    auto member = [&](const char* key) { return [&w, &small, key](int l) { w.value(l, small.at(key)); }; };
    w.object(level, {
        {"hitsByRule", member("hitsByRule")},
        {"paths",
         [&](int l) {
             w.array(l, [&](const JsonWriter::Emit& emit) {
                 for (const auto& p : c.paths)
                     emit(pathJson(c, p));
             });
         }},
        {"rules", member("rules")},
        {"stats", member("stats")},
        {"structures", member("structures")},
        {"waivers", member("waivers")},
    });
}

} // namespace

void writeJson(const AnalysisResult& r, std::ostream& os, int indent) {
    // Small members are built as one DOM; the large lists (registers, CDC
    // paths) are streamed element by element, so memory stays bounded.
    // Keys are written in sorted order, as nlohmann::json would.
    json small;
    small["format"] = kResultFormatName;
    small["formatVersion"] = kResultFormatVersion;
    small["tool"] = {{"name", "OpenCDC Analyzer"}, {"version", kVersionString}};
    small["project"] = {{"name", r.project.name},
                        {"file", toUtf8(r.project.projectFile)},
                        {"top", r.project.top},
                        {"language", frontend::toString(r.project.language)},
                        {"hdl", r.project.languageSummary()}};
    small["startedAtUtc"] = r.startedAtUtc;
    small["elapsedSeconds"] = r.elapsedSeconds;

    json stages = json::array();
    for (const auto& s : r.stages)
        stages.push_back(
            {{"name", s.name}, {"status", toString(s.status)}, {"detail", s.detail}, {"seconds", s.seconds}});
    small["stages"] = std::move(stages);

    if (r.elaboration) {
        const auto& e = *r.elaboration;
        json files = json::array();
        for (const auto& f : e.parsedFiles)
            files.push_back(toUtf8(f));
        json defs = json::array();
        for (const auto& d : e.definitions)
            defs.push_back({{"name", d.name}, {"kind", d.kind}, {"location", locationJson(d.location)}});
        json tops = json::array();
        for (const auto& t : e.topInstances)
            tops.push_back(instanceJson(t));
        small["elaboration"] = {{"parsedFiles", std::move(files)},
                                {"definitions", std::move(defs)},
                                {"instanceCount", e.instanceCount},
                                {"hierarchy", std::move(tops)}};
    }
    else {
        small["elaboration"] = nullptr;
    }
    small["clockAnalysis"] =
        r.clocks ? clocksJson(*r.clocks, r.constraints.get(), r.project.options.unknownClockRelation) : json(nullptr);

    json diags = json::array();
    for (const auto& d : r.diagnostics) {
        diags.push_back({{"code", d.code},
                         {"severity", toString(d.severity)},
                         {"message", d.message},
                         {"location", locationJson(d.location)},
                         {"notes", d.notes}});
    }
    small["diagnostics"] = std::move(diags);
    small["summary"] = {{"errors", r.count(Severity::Error) + r.count(Severity::Fatal)},
                        {"warnings", r.count(Severity::Warning)},
                        {"infos", r.count(Severity::Info)}};
    // Explicit marker so that no consumer mistakes this for a CDC report.
    small["cdcAnalysisAvailable"] = r.cdc != nullptr;
    small["summary"]["violationsError"] = r.violations(Severity::Error);
    small["summary"]["violationsWarning"] = r.violations(Severity::Warning);
    small["summary"]["pathsPotentialRisk"] = r.pathsWithStatus(cdc::AnalysisStatus::PotentialRisk);
    small["summary"]["pathsNeedReview"] = r.pathsWithStatus(cdc::AnalysisStatus::NeedsReview);
    small["summary"]["waivedHits"] = r.waivedHits();

    JsonWriter w(os, indent);
    auto member = [&](const char* key) { return [&w, &small, key](int l) { w.value(l, small.at(key)); }; };
    auto null = [&](int l) { w.value(l, json(nullptr)); };
    std::vector<JsonWriter::Member> members;
    for (auto it = small.begin(); it != small.end(); ++it)
        members.push_back({it.key(), member(it.key().c_str())});
    members.push_back({"cdc", r.cdc ? JsonWriter::Body([&](int l) { writeCdc(w, l, *r.cdc); }) : null});
    members.push_back({"rtl", r.design ? JsonWriter::Body([&](int l) { writeRtl(w, l, *r.design, r.clocks.get()); })
                                       : null});
    std::ranges::sort(members, {}, &JsonWriter::Member::key);
    w.object(0, members);
}

std::string toJson(const AnalysisResult& r, int indent) {
    std::ostringstream os;
    writeJson(r, os, indent);
    return std::move(os).str();
}

} // namespace opencdc::engine

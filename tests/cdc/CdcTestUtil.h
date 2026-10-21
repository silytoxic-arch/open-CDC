#pragma once

// Shared helpers of the CDC tests (test_cdc.cpp, test_cdc_advanced.cpp).

#include "cdc/CdcAnalyzer.h"
#include "cdc/SeqGraph.h"
#include "clock/ClockAnalyzer.h"
#include "frontend/Frontend.h"
#include "rules/RuleEngine.h"

#include "../DesignUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::test::cdc {

using namespace opencdc;
using namespace opencdc::cdc;

struct CdcRun {
    test::BuiltDesign built;
    std::unique_ptr<graph::DesignView> view;
    constraints::ConstraintSet cons;
    std::shared_ptr<const clock::ClockModel> clocks;
    std::shared_ptr<CdcResult> cdc;
    std::vector<Diagnostic> diags;

    /// The unique path from `src` to `dst` (register names as "top.x").
    [[nodiscard]] const CdcPath& path(std::string_view src, std::string_view dst) const {
        const CdcPath* found = nullptr;
        for (const auto& p : cdc->paths) {
            if (p.srcName == src && p.dstName == dst) {
                REQUIRE_FALSE(found);
                found = &p;
            }
        }
        if (!found) {
            for (const auto& p : cdc->paths)
                UNSCOPED_INFO("path: " << p.srcName << " -> " << p.dstName);
            FAIL("no path " << src << " -> " << dst);
        }
        return *found;
    }
    [[nodiscard]] bool hasPath(std::string_view src, std::string_view dst) const {
        return std::ranges::any_of(cdc->paths, [&](const CdcPath& p) { return p.srcName == src && p.dstName == dst; });
    }
    [[nodiscard]] std::size_t hits(std::string_view rule) const {
        std::size_t n = 0;
        for (const auto& p : cdc->paths)
            for (const auto& h : p.hits)
                n += h.ruleId == rule ? 1U : 0U;
        return n;
    }
    [[nodiscard]] std::size_t countCode(std::string_view c) const {
        return static_cast<std::size_t>(std::ranges::count_if(diags, [&](const Diagnostic& d) { return d.code == c; }));
    }
};

inline bool hasHit(const CdcPath& p, std::string_view rule) {
    return std::ranges::any_of(p.hits, [&](const RuleHit& h) { return h.ruleId == rule; });
}

inline constexpr const char* kAsync = "set_clock_groups -asynchronous -group clk_a -group clk_b\n";

inline CdcRun analyze(std::string_view verilog, std::string_view sdc = kAsync, CdcOptions opt = {}) {
    CdcRun run;
    run.built = test::buildDesign(verilog, "top");
    run.view = std::make_unique<graph::DesignView>(*run.built.design);
    CollectingDiagnosticSink sink;
    constraints::DesignObjectResolver resolver(*run.view);
    if (!sdc.empty())
        constraints::parseSdc(sdc, "t.sdc", resolver, run.cons, sink);
    run.clocks = clock::analyzeClocks(*run.view, run.cons, sink);
    run.cdc = analyzeCdc(*run.view, *run.clocks, run.cons, opt, sink);
    rules::applyRules(rules::RuleRegistry::standard(), *run.cdc, *run.clocks, opt);
    run.diags = sink.take();
    for (const auto& d : run.diags)
        UNSCOPED_INFO(format(d));
    for (const auto& p : run.cdc->paths) {
        UNSCOPED_INFO("path " << p.srcName << " -> " << p.dstName << " sync=" << toString(p.syncType) << "/"
                              << toString(p.syncStatus) << " status=" << toString(p.status));
    }
    return run;
}

inline CdcRun analyzeExample(const char* dirName, const std::vector<const char*>& files, const char* top,
                      const char* constraintFile, std::vector<std::filesystem::path> includes = {}) {
    CdcRun run;
    const auto dir = test::examplesDir() / dirName;
    frontend::FrontendInput in;
    for (const char* f : files)
        in.sourceFiles.push_back(dir / f);
    in.includeDirs = std::move(includes);
    in.top = top;
    CollectingDiagnosticSink sink;
    auto out = frontend::makeSlangFrontend()->elaborate(in, sink);
    REQUIRE(out.ok());
    run.built.design = out.value().design;
    REQUIRE(run.built.design);
    run.view = std::make_unique<graph::DesignView>(*run.built.design);
    constraints::DesignObjectResolver resolver(*run.view);
    REQUIRE(constraints::readSdcFile(dir / constraintFile, resolver, run.cons, sink));
    run.clocks = clock::analyzeClocks(*run.view, run.cons, sink);
    const CdcOptions opt;
    run.cdc = analyzeCdc(*run.view, *run.clocks, run.cons, opt, sink);
    rules::applyRules(rules::RuleRegistry::standard(), *run.cdc, *run.clocks, opt);
    run.diags = sink.take();
    for (const auto& p : run.cdc->paths)
        UNSCOPED_INFO("path " << p.srcName << " -> " << p.dstName << " sync=" << toString(p.syncType));
    return run;
}


} // namespace opencdc::test::cdc

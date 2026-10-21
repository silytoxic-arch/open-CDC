#pragma once
// Helpers to build a Design IR from Verilog text in tests.

#include "TestUtil.h"
#include "core/Diagnostic.h"
#include "frontend/Frontend.h"
#include "ir/Dump.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace opencdc::test {

struct BuiltDesign {
    std::shared_ptr<const ir::Design> design;
    std::vector<Diagnostic> diags;

    [[nodiscard]] std::size_t countCode(std::string_view code) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(diags, [&](const Diagnostic& d) { return d.code == code; }));
    }
};

inline BuiltDesign buildDesign(std::string_view verilog, std::string top = "",
                               frontend::LanguageStd lang = frontend::LanguageStd::Verilog2001) {
    TempDir dir;
    frontend::FrontendInput in;
    in.sourceFiles = {dir.write("d.v", verilog)};
    in.top = std::move(top);
    in.language = lang;
    CollectingDiagnosticSink sink;
    auto out = frontend::makeSlangFrontend()->elaborate(in, sink);
    REQUIRE(out.ok());
    BuiltDesign b;
    b.design = out.value().design;
    b.diags = sink.take();
    REQUIRE(b.design);
    return b;
}

/// (instance, register index) of the register whose Q is printed as `hierQ`,
/// e.g. "top.u_sync.meta" or "top.cnt[3:0]".
inline std::pair<ir::InstanceId, std::size_t> findRegister(const ir::Design& d, std::string_view hierQ) {
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const auto& node = d.instances[i];
        const auto& body = d.body(node.body);
        for (std::size_t r = 0; r < body.registers.size(); ++r)
            if (node.hierPath + "." + ir::formatSig(body, body.registers[r].q) == hierQ)
                return {ir::InstanceId{static_cast<std::uint32_t>(i)}, r};
    }
    FAIL("no register " << hierQ);
    return {};
}

} // namespace opencdc::test

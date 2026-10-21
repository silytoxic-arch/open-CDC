#include "core/Diagnostic.h"

#include <catch2/catch_test_macros.hpp>

using namespace opencdc;

TEST_CASE("format() renders location, severity, code and notes", "[core][diagnostic]") {
    Diagnostic d{"FE-TEST", Severity::Error, "something failed", {"rtl/top.v", 12, 5}, {"see here"}};
    CHECK(format(d) == "rtl/top.v:12:5: error[FE-TEST]: something failed\n    note: see here");
}

TEST_CASE("format() omits unknown location parts", "[core][diagnostic]") {
    CHECK(format({"X", Severity::Warning, "msg", {}, {}}) == "warning[X]: msg");
    CHECK(format({"X", Severity::Info, "msg", {"a.v", 3, 0}, {}}) == "a.v:3: info[X]: msg");
    CHECK(format({"", Severity::Fatal, "msg", {}, {}}) == "fatal: msg");
}

TEST_CASE("SourceLocation validity", "[core][diagnostic]") {
    CHECK_FALSE(SourceLocation{}.valid());
    CHECK_FALSE(SourceLocation{"a.v", 0, 0}.valid());
    CHECK(SourceLocation{"a.v", 1, 0}.valid());
}

TEST_CASE("CollectingDiagnosticSink counts and reports errors", "[core][diagnostic]") {
    CollectingDiagnosticSink sink;
    CHECK_FALSE(sink.hasErrors());

    sink.report({"A", Severity::Warning, "w", {}, {}});
    sink.report({"B", Severity::Info, "i", {}, {}});
    CHECK_FALSE(sink.hasErrors());

    sink.report({"C", Severity::Fatal, "f", {}, {}});
    CHECK(sink.hasErrors());
    CHECK(sink.count(Severity::Warning) == 1);
    CHECK(sink.count(Severity::Fatal) == 1);
    CHECK(sink.count(Severity::Error) == 0);

    auto taken = sink.take();
    REQUIRE(taken.size() == 3);
    CHECK(taken[0].code == "A"); // order preserved
    CHECK(sink.diagnostics().empty());
}

#include "core/Id.h"
#include "core/PathUtil.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <unordered_set>

using namespace opencdc;

namespace {
using NetId = Id<struct NetTagForTest>;
using CellId = Id<struct CellTagForTest>;
} // namespace

TEST_CASE("Id is invalid by default and comparable", "[core][id]") {
    NetId none;
    CHECK_FALSE(none.valid());
    NetId a{3};
    NetId b{7};
    CHECK(a.valid());
    CHECK(a.index() == 3);
    CHECK(a < b);
    CHECK(a == NetId{3});
    // Different tags are different types: no accidental mixing.
    STATIC_CHECK_FALSE(std::is_convertible_v<NetId, CellId>);
    STATIC_CHECK_FALSE(std::is_convertible_v<std::uint32_t, NetId>);
}

TEST_CASE("Id is hashable", "[core][id]") {
    std::unordered_set<NetId> set{NetId{1}, NetId{2}, NetId{1}};
    CHECK(set.size() == 2);
}

TEST_CASE("Result holds either a value or a diagnostic", "[core][result]") {
    Result<int> ok = 42;
    REQUIRE(ok.ok());
    CHECK(ok.value() == 42);

    Result<int> bad = Diagnostic{"E1", Severity::Error, "boom", {}, {}};
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == "E1");

    Result<std::string> moved = std::string("abc");
    std::string s = std::move(moved).value();
    CHECK(s == "abc");
}

TEST_CASE("UTF-8 path round trip", "[core][path]") {
    const std::string text = "rtl/\xE6\x97\xB6\xE9\x92\x9F/top.v"; // "rtl/时钟/top.v"
    CHECK(toUtf8(fromUtf8(text)) == text);
}

TEST_CASE("CancellationToken", "[core][progress]") {
    CancellationToken token;
    CHECK_FALSE(token.cancelled());
    token.cancel();
    CHECK(token.cancelled());
}

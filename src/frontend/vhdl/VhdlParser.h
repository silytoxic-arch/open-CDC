#pragma once
// Recursive-descent VHDL parser (VHDL-93 / VHDL-2008). Produces the AST of
// one design file. Syntax errors are reported with their location; the
// parser recovers at statement and design-unit boundaries so that one error
// does not hide the rest of the file.

#include "frontend/vhdl/VhdlAst.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::vhdl {

struct ParseMessage {
    std::string message;
    Loc loc;
    bool error = true; ///< false: warning
};

struct ParseResult {
    DesignFile file;
    std::vector<ParseMessage> messages;
    [[nodiscard]] bool hasErrors() const noexcept;
};

/// Parses one VHDL design file. `fileIndex` is stored in every location.
[[nodiscard]] ParseResult parseVhdl(std::string_view text, std::uint32_t fileIndex);

/// Parses a single expression (used by tests and for generic overrides from
/// the project file).
[[nodiscard]] ExprPtr parseVhdlExpression(std::string_view text, std::vector<ParseMessage>& messages);

} // namespace opencdc::vhdl

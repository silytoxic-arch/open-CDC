#include "core/Diagnostic.h"

#include <algorithm>
#include <utility>

namespace opencdc {

std::string toString(const SourceLocation& loc) {
    if (loc.file.empty())
        return {};
    std::string out = loc.file;
    if (loc.line > 0) {
        out += ':';
        out += std::to_string(loc.line);
        if (loc.column > 0) {
            out += ':';
            out += std::to_string(loc.column);
        }
    }
    return out;
}

std::string_view toString(Severity severity) noexcept {
    switch (severity) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
        case Severity::Fatal: return "fatal";
    }
    return "unknown";
}

std::string format(const Diagnostic& diag) {
    std::string out;
    if (auto loc = toString(diag.location); !loc.empty()) {
        out += loc;
        out += ": ";
    }
    out += toString(diag.severity);
    if (!diag.code.empty()) {
        out += '[';
        out += diag.code;
        out += ']';
    }
    out += ": ";
    out += diag.message;
    for (const auto& note : diag.notes) {
        out += "\n    note: ";
        out += note;
    }
    return out;
}

void CollectingDiagnosticSink::report(Diagnostic diag) {
    diags_.push_back(std::move(diag));
}

std::vector<Diagnostic> CollectingDiagnosticSink::take() noexcept {
    return std::exchange(diags_, {});
}

std::size_t CollectingDiagnosticSink::count(Severity severity) const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        diags_, [severity](const Diagnostic& d) { return d.severity == severity; }));
}

bool CollectingDiagnosticSink::hasErrors() const noexcept {
    return std::ranges::any_of(diags_, [](const Diagnostic& d) { return d.isError(); });
}

} // namespace opencdc

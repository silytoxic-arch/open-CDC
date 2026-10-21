// SVG output of a laid-out CDC schematic (no Qt).

#include "schematic/Schematic.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <vector>

namespace opencdc::schematic {

namespace {

std::string esc(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

const char* statusColor(cdc::AnalysisStatus s) {
    switch (s) {
        case cdc::AnalysisStatus::Safe: return "#2e8b57";
        case cdc::AnalysisStatus::NeedsReview: return "#c9a200";
        case cdc::AnalysisStatus::PotentialRisk: return "#e07b00";
        case cdc::AnalysisStatus::Violation: return "#d02020";
    }
    return "#000000";
}

constexpr const char* kPalette[] = {"#cfe2ff", "#d8f0d0", "#ffe0c0", "#ecd8f5",
                                    "#fff3b8", "#c8eeee", "#f8d0dc", "#e2e2c8"};

std::string baseClock(const std::string& name) {
    const auto sp = name.find(' ');
    return sp == std::string::npos ? name : name.substr(0, sp);
}

} // namespace

std::string toSvg(const Schematic& s, const cdc::CdcResult& result) {
    std::map<std::string, std::string> fills;
    auto fill = [&](const Node& n) -> std::string {
        if (n.kind == NodeKind::Logic || n.clock.empty())
            return "#f0f0f0";
        const auto base = baseClock(n.clock);
        auto it = fills.find(base);
        if (it == fills.end())
            it = fills.emplace(base, kPalette[fills.size() % std::size(kPalette)]).first;
        return it->second;
    };
    // Colors by clock name in alphabetical order over the whole result, so
    // every schematic of one analysis (and the GUI) uses the same colors.
    {
        std::vector<std::string> names;
        for (const auto& p : result.paths)
            for (const auto* c : {&p.srcClockName, &p.dstClockName})
                if (!c->empty() && c->front() != '(')
                    names.push_back(baseClock(*c));
        std::ranges::sort(names);
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (const auto& n : names)
            fills.emplace(n, kPalette[fills.size() % std::size(kPalette)]);
    }

    const double margin = 20;
    const double titleH = 28;
    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << num(s.width + 2 * margin) << "\" height=\""
      << num(s.height + 2 * margin + titleH) << "\" font-family=\"sans-serif\" font-size=\"12\">\n";
    o << "<title>" << esc(s.title) << "</title>\n";
    o << "<defs><marker id=\"arrow\" viewBox=\"0 0 10 10\" refX=\"10\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"10\" "
         "markerHeight=\"10\" orient=\"auto-start-reverse\"><path d=\"M0,0 L10,5 L0,10 z\" fill=\"context-stroke\"/>"
         "</marker></defs>\n";
    o << "<rect width=\"100%\" height=\"100%\" fill=\"#fbfbfb\"/>\n";
    o << "<text x=\"" << num(margin) << "\" y=\"" << num(margin) << "\" font-weight=\"bold\">" << esc(s.title)
      << "</text>\n";
    o << "<g transform=\"translate(" << num(margin) << "," << num(margin + titleH) << ")\">\n";

    for (const auto& e : s.edges) {
        cdc::AnalysisStatus worst = cdc::AnalysisStatus::Safe;
        for (const auto pid : e.paths)
            if (pid < result.paths.size())
                worst = std::max(worst, result.paths[pid].status);
        std::string color = "#444444";
        std::string dash;
        double width = 1.5;
        switch (e.kind) {
            case EdgeKind::Path:
                color = statusColor(worst);
                width = e.crossesClocks ? 3.0 : 2.2;
                break;
            case EdgeKind::Chain: break;
            case EdgeKind::Qualifier:
                color = "#1f5fbf";
                dash = "6,4";
                break;
            case EdgeKind::Reconvergence:
                color = "#8e3fb0";
                dash = "8,3,2,3";
                break;
        }
        std::string d = "M" + num(e.points.front().x) + "," + num(e.points.front().y);
        for (std::size_t i = 1; i < e.points.size(); ++i) {
            const auto& a = e.points[i - 1];
            const auto& b = e.points[i];
            const double dx = (b.x - a.x) * 0.5;
            d += " C" + num(a.x + dx) + "," + num(a.y) + " " + num(b.x - dx) + "," + num(b.y) + " " + num(b.x) + "," +
                 num(b.y);
        }
        o << "<path class=\"edge\" d=\"" << d << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\""
          << num(width) << "\"" << (dash.empty() ? "" : " stroke-dasharray=\"" + dash + "\"")
          << " marker-end=\"url(#arrow)\"/>\n";
        if (!e.label.empty())
            o << "<text x=\"" << num(e.points.front().x + 6) << "\" y=\"" << num(e.points.front().y - 5)
              << "\" font-size=\"10\" fill=\"#555555\">" << esc(e.label) << "</text>\n";
    }

    for (const auto& n : s.nodes) {
        if (n.kind == NodeKind::Dummy)
            continue;
        std::string stroke = "#505050";
        double sw = 1.2;
        std::string dash;
        if (n.roles & kDestination) {
            stroke = statusColor(n.status);
            sw = 3.0;
        }
        else if (n.roles & kReconvergence) {
            stroke = "#8e3fb0";
            sw = 2.5;
            dash = " stroke-dasharray=\"6,3\"";
        }
        o << "<g class=\"node\"><title>" << esc(n.detail) << "</title>\n";
        const std::string common = "fill=\"" + fill(n) + "\" stroke=\"" + stroke + "\" stroke-width=\"" + num(sw) +
                                   "\"" + dash;
        switch (n.kind) {
            case NodeKind::Logic:
                o << "<rect x=\"" << num(n.x) << "\" y=\"" << num(n.y) << "\" width=\"" << num(n.w) << "\" height=\""
                  << num(n.h) << "\" rx=\"" << num(n.h / 2.5) << "\" " << common << "/>\n";
                break;
            case NodeKind::Input:
            case NodeKind::BlackBox:
                o << "<polygon points=\"" << num(n.x) << "," << num(n.y) << " " << num(n.x + n.w - 12) << ","
                  << num(n.y) << " " << num(n.x + n.w) << "," << num(n.y + n.h / 2) << " " << num(n.x + n.w - 12)
                  << "," << num(n.y + n.h) << " " << num(n.x) << "," << num(n.y + n.h) << "\" " << common << "/>\n";
                break;
            case NodeKind::Memory:
                o << "<rect x=\"" << num(n.x + 5) << "\" y=\"" << num(n.y - 5) << "\" width=\"" << num(n.w)
                  << "\" height=\"" << num(n.h) << "\" fill=\"" << fill(n) << "\" stroke=\"#505050\"/>\n";
                [[fallthrough]];
            default:
                o << "<rect x=\"" << num(n.x) << "\" y=\"" << num(n.y) << "\" width=\"" << num(n.w) << "\" height=\""
                  << num(n.h) << "\" " << common << "/>\n";
                if (n.kind != NodeKind::Memory)
                    o << "<polyline points=\"" << num(n.x) << "," << num(n.y + n.h - 13) << " " << num(n.x + 8) << ","
                      << num(n.y + n.h - 7) << " " << num(n.x) << "," << num(n.y + n.h - 1)
                      << "\" fill=\"none\" stroke=\"#505050\"/>\n";
                break;
        }
        const double cx = n.x + n.w / 2;
        const bool clocked = !n.clock.empty() && n.kind != NodeKind::Logic;
        o << "<text x=\"" << num(cx) << "\" y=\"" << num(n.y + n.h / 2 + (clocked ? -1 : 4))
          << "\" text-anchor=\"middle\">" << esc(n.label) << "</text>\n";
        if (clocked)
            o << "<text x=\"" << num(cx) << "\" y=\"" << num(n.y + n.h - 5)
              << "\" text-anchor=\"middle\" font-size=\"10\" fill=\"#404040\">" << esc(n.clock) << "</text>\n";
        o << "</g>\n";
    }
    o << "</g>\n</svg>\n";
    return o.str();
}

} // namespace opencdc::schematic

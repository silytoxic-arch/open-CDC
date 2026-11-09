#include "engine/FileList.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>

namespace opencdc::engine {

namespace {

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::filesystem::path canonicalOf(const std::filesystem::path& p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

std::vector<std::string> plusArgs(const std::string& s, std::size_t from) {
    std::vector<std::string> out;
    std::stringstream ss(s.substr(from));
    std::string item;
    while (std::getline(ss, item, '+'))
        if (!item.empty())
            out.push_back(item);
    return out;
}

} // namespace

bool isFileList(const std::filesystem::path& file) {
    auto ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".f";
}

FileListResult readFileList(const std::filesystem::path& list, std::set<std::filesystem::path>& seen) {
    FileListResult r;
    std::set<std::filesystem::path> lists; // lists being read (a list naming itself)
    std::function<bool(const std::filesystem::path&, int)> read = [&](const std::filesystem::path& f, int depth) {
        const auto canon = canonicalOf(f);
        if (depth > 32 || !lists.insert(canon).second)
            return true; // a cycle: read once
        std::ifstream in(f);
        if (!in) {
            r.error = "cannot read file list '" + f.string() + "'";
            return false;
        }
        const auto dir = f.parent_path();
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            if (const auto c = line.find("//"); c != std::string::npos)
                line.erase(c);
            line = trim(line);
            if (line.empty() || line.front() == '#')
                continue;
            // several entries on one line, separated by blanks
            std::stringstream ss(line);
            std::vector<std::string> words;
            for (std::string w; ss >> w;) {
                if (w.front() == '#')
                    break; // a comment to the end of the line
                words.push_back(w);
            }
            for (std::size_t i = 0; i < words.size(); ++i) {
                const std::string& w = words[i];
                if ((w == "-f" || w == "-F") && i + 1 < words.size()) {
                    if (!read(dir / words[++i], depth + 1))
                        return false;
                    continue;
                }
                if (w.starts_with("+incdir+")) {
                    for (const auto& d : plusArgs(w, 8))
                        if (const auto p = (dir / d).lexically_normal();
                            std::ranges::find(r.includeDirs, p) == r.includeDirs.end())
                            r.includeDirs.push_back(p);
                    continue;
                }
                if (w.starts_with("+define+")) {
                    for (const auto& d : plusArgs(w, 8)) {
                        const auto eq = d.find('=');
                        r.defines.push_back(eq == std::string::npos
                                                ? frontend::MacroDefine{d, std::nullopt}
                                                : frontend::MacroDefine{d.substr(0, eq), d.substr(eq + 1)});
                    }
                    continue;
                }
                if (w.front() == '-' || w.front() == '+') {
                    // -v lib.v / -y dir take an argument
                    std::string text = w;
                    if ((w == "-v" || w == "-y") && i + 1 < words.size())
                        text += " " + words[++i];
                    r.ignored.push_back(f.filename().string() + ":" + std::to_string(lineNo) + ": " + text);
                    continue;
                }
                const auto p = (dir / w).lexically_normal();
                if (isFileList(p)) {
                    if (!read(p, depth + 1))
                        return false;
                    continue;
                }
                if (seen.insert(canonicalOf(p)).second)
                    r.sources.push_back(p);
            }
        }
        return true;
    };
    read(list, 0);
    return r;
}

} // namespace opencdc::engine

#include "coverage.h"

#include <cxxabi.h>
#include <array>
#include <cstdio>
#include <cstdlib>

#include "sha256.h"

namespace afzh {

std::uint32_t edge_id(const std::string& key) {
    std::array<std::uint8_t, 32> digest{};
    Sha256 h;
    h.update(key);
    h.finish(digest.data());
    return (static_cast<std::uint32_t>(digest[0]) << 24) |
           (static_cast<std::uint32_t>(digest[1]) << 16) |
           (static_cast<std::uint32_t>(digest[2]) << 8) | static_cast<std::uint32_t>(digest[3]);
}

std::string demangle_cxx(const std::string& symbol) {
    if (symbol.size() < 3 || symbol[0] != '_' || symbol[1] != 'Z') return symbol;
    int status = 0;
    char* dem = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, &status);
    if (!dem || status != 0) {
        std::free(dem);
        return symbol;
    }
    std::string out(dem);
    std::free(dem);
    return out;
}

namespace {

// "rad::unorm_utf8(std::string const&)" -> "rad::unorm_utf8".
std::string cut_params(const std::string& demangled) {
    std::string head = demangled;
    // The parameter list is the trailing parenthesized group; an earlier
    // `(` belongs to an anonymous-namespace marker, not to the arguments.
    if (!head.empty() && head.back() == ')') {
        int depth = 0;
        std::size_t i = head.size();
        for (; i-- > 0;) {
            if (head[i] == ')') ++depth;
            else if (head[i] == '(' && --depth == 0) break;
        }
        if (i > 0) head = head.substr(0, i);
    }
    // Trim a return-type prefix: the last space at paren depth zero
    // separates it from the qualified name.
    int depth = 0;
    std::size_t cut = std::string::npos;
    for (std::size_t i = 0; i < head.size(); ++i) {
        if (head[i] == '(') ++depth;
        else if (head[i] == ')') --depth;
        else if (head[i] == ' ' && depth == 0) cut = i;
    }
    if (cut != std::string::npos) head = head.substr(cut + 1);
    return head;
}

// "rad::foo<int, 2>" -> "rad::foo".
std::string cut_templates(const std::string& name) {
    auto lt = name.find('<');
    return lt == std::string::npos ? name : name.substr(0, lt);
}

} // namespace

std::vector<std::vector<std::string>> Coverage::hits(const std::string& profraw,
                                                     std::string& error) {
    std::string cmd = profdata + " show -all-functions -text " + profraw + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        error = profdata + " did not start";
        return {};
    }
    std::string text;
    char buf[4096];
    while (fgets(buf, sizeof buf, p)) text += buf;
    int st = pclose(p);
    if (st != 0) {
        error = "llvm-profdata failed: " + text.substr(0, 400);
        return {};
    }

    // `llvm-profdata show -text -all-functions` prints, per function: a name
    // line, then "# Func Hash:" plus a bare hash, "# Num Counters:" plus a
    // count, then "# Counter Values:" and the counters. The first counter is
    // the entry count; region counters describe branches and never decide a
    // hit.
    enum class Field { Name, Hash, Count, Values } field = Field::Name;
    std::vector<std::vector<std::string>> out;
    std::string current;
    for (std::size_t start = 0; start <= text.size();) {
        std::size_t eol = text.find('\n', start);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(start, eol - start);
        start = eol + 1;
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (line.empty()) {
            field = Field::Name; // a blank line ends one function's block
            continue;
        }
        if (line == "# Func Hash:") { field = Field::Hash; continue; }
        if (line == "# Num Counters:") { field = Field::Count; continue; }
        if (line == "# Counter Values:") { field = Field::Values; continue; }
        if (line.front() == '#') continue;
        if (field == Field::Name) {
            current = line;
        } else if (field == Field::Values && !current.empty()) {
            try {
                if (std::stoull(line) > 0) {
                    // Some entries print as `file.cpp:_ZN...`: the mangled
                    // name starts at the last colon-delimited `_Z`.
                    std::string raw = current;
                    auto colon = raw.rfind(':');
                    if (colon != std::string::npos && raw.compare(colon + 1, 2, "_Z") == 0)
                        raw = raw.substr(colon + 1);
                    std::string dem = demangle_cxx(raw);
                    std::string key = cut_params(dem);
                    std::vector<std::string> candidates{key};
                    std::string tmpl = cut_templates(key);
                    if (tmpl != key) candidates.push_back(tmpl);
                    // Anonymous-namespace functions demangle with
                    // `(anonymous namespace)::`; the universe keys them bare.
                    auto qual = key.rfind("::");
                    if (qual != std::string::npos) candidates.push_back(key.substr(qual + 2));
                    if (raw != key) candidates.push_back(raw);
                    out.push_back(std::move(candidates));
                    current.clear(); // only the entry counter decides
                }
            } catch (...) {
            }
            field = Field::Hash; // the remaining counters never re-fire
        }
    }
    return out;
}

json coverage_map(const std::vector<UniverseEntry>& universe) {
    json entries = json::array();
    for (const UniverseEntry& u : universe)
        entries.push_back(json{{"edge", edge_id(u.key)},
                               {"url", u.file},
                               {"start", u.start},
                               {"end", u.end}});
    return json{{"entries", entries}};
}

} // namespace afzh

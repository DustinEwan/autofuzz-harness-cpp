#include "wire.h"

#include <cstdio>
#include <unistd.h>

namespace afzh {

bool read_line_stdin(std::string& line) {
    line.clear();
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        // A blank line is EOF by the transport rule (authoring guide section 12).
        if (c == '\n') return !line.empty();
        if (c != '\r') line += char(c);
    }
    return !line.empty();
}

bool write_line_stdout(std::string_view line) {
    // Write directly to fd 1: a stdio buffer that a failed flush hides would
    // drop the frame the engine is waiting on.
    std::string data(line);
    data += '\n';
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(1, data.data() + off, data.size() - off);
        if (n <= 0) {
            std::fprintf(stderr, "afz-cpp: stdout write failed; the engine will restart this process\n");
            std::fflush(stderr);
            return false;
        }
        off += std::size_t(n);
    }
    return true;
}

json ok_response(std::uint64_t id, json result) {
    return json{{"jsonrpc", kJsonRpc}, {"id", id}, {"result", std::move(result)}};
}

json error_response(std::uint64_t id, int code, std::string_view message) {
    return json{{"jsonrpc", kJsonRpc},
                {"id", id},
                {"error", {{"code", code}, {"message", std::string(message)}}}};
}

namespace {

// Segment-aware match: `*` within one path segment, `**` across segments,
// `?` one character.
bool glob_here(std::string_view pat, std::string_view str) {
    while (!pat.empty()) {
        if (pat[0] == '*') {
            if (pat.size() > 1 && pat[1] == '*') {
                std::string_view rest = pat.substr(2);
                if (!rest.empty() && rest[0] == '/') rest = rest.substr(1);
                for (std::size_t i = 0;; ++i) {
                    if (glob_here(rest, str.substr(i))) return true;
                    if (i >= str.size()) return false;
                }
            }
            std::string_view rest = pat.substr(1);
            for (std::size_t i = 0;; ++i) {
                if (glob_here(rest, str.substr(i))) return true;
                if (i >= str.size() || str[i] == '/') return false;
            }
        }
        if (str.empty()) return false;
        if (pat[0] == '?' || pat[0] == str[0]) {
            pat.remove_prefix(1);
            str.remove_prefix(1);
            continue;
        }
        return false;
    }
    return str.empty();
}

} // namespace

bool glob_match(std::string_view pattern, std::string_view path) {
    // docs/harness-authoring.md section 3: a pattern with no '/' is matched
    // against the file name alone.
    if (pattern.find('/') == std::string_view::npos) {
        auto slash = path.rfind('/');
        if (slash != std::string_view::npos) path = path.substr(slash + 1);
    }
    return glob_here(pattern, path);
}

} // namespace afzh

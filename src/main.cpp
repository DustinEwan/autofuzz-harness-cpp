// The afz-cpp harness plugin: a line-delimited JSON-RPC loop over stdin and
// stdout speaking the afz/1 protocol. The engine spawns this binary as a
// child process; one process serves one scan.
//
// The seven answered methods: handshake, discover, prepare, invoke,
// reproducer, coverageMap, ready. `minimize` is not a declared capability and
// answers -32601, like the Rust harness.
// Discovery parses with libclang and never
// runs target code; the driver binary is built lazily on the first `prepare`
// because building is the honest way to answer whether a call can be made.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "coverage.h"
#include "discover.h"
#include "sha256.h"
#include "driver.h"
#include "lower.h"
#include "options.h"
#include "wire.h"

namespace afzh {

namespace {

class Harness {
public:
    struct RpcError {
        int code;
        std::string message;
    };

    json handshake() {
        // The engine reads the coverage capability from the handshake, which
        // precedes discover; resolve llvm-profdata from PATH now and let a
        // later llvmBin option re-resolve it in discover.
        if (coverage_.profdata.empty())
            coverage_.profdata = find_llvm_tool("llvm-profdata", std::string());
        haveCoverage_ = !coverage_.profdata.empty();
        json caps = json::array();
        if (haveCoverage_) caps.push_back("coverage");
        caps.push_back("reproducer");
        caps.push_back("stateful");
        return json{{"protocolVersion", kProtocolVersion},
                    {"name", "cpp"},
                    {"version", "1.0.0"},
                    {"targetClass", "design-time"},
                    {"capabilities", caps},
                    {"languages", json::array({"cpp"})},
                    {"discoverRequiresExecution", false}};
    }

    json discover(const json& req) {
        std::string root = req.value("root", std::string());
        std::vector<std::string> include = str_array(req, "include");
        std::vector<std::string> exclude = str_array(req, "exclude");
        std::string config = req.contains("configPath") && req["configPath"].is_string()
                                 ? req["configPath"].get<std::string>()
                                 : root + "/autofuzz.json";
        Inventory inv;
        std::string error;
        if (!afzh::discover(root, include, exclude, config, inv, error))
            throw RpcError{kPluginError, error};
        options_ = load_options(config);
        haveOptions_ = true;
        // The handshake decided the coverage capability before options were
        // readable; re-resolve here so a later llvmBin still drives the tool
        // itself.
        coverage_.profdata = find_llvm_tool("llvm-profdata", options_.llvmBin);
        haveCoverage_ = !coverage_.profdata.empty();
        addr2line_ = find_llvm_tool("llvm-addr2line", options_.llvmBin);
        universe_.clear();
        for (const UniverseEntry& u : inv.universe)
            universe_.try_emplace(u.key, edge_id(u.key));
        discovery_ = inv.discovery;
        inventory_ = std::move(inv);
        haveInventory_ = true;
        haveDriver_ = false;
        return discovery_;
    }

    json prepare(const json& req) {
        const std::string id = req.value("targetId", std::string());
        if (!haveInventory_) throw RpcError{kInvalidRequest, "discover has not run"};
        const Tgt* tgt = find_target(id);
        if (!tgt) {
            // Unknown ids are a wire error; ids the repair loop dropped are a
            // discovery error the engine can report.
            if (std::find(driver_.dropped.begin(), driver_.dropped.end(), id) ==
                driver_.dropped.end())
                throw RpcError{kInvalidRequest, "no target with id " + id};
            return json{{"ok", false}, {"reason", kSkipDiscoveryError}};
        }
        ensure_driver();
        if (std::find(driver_.dropped.begin(), driver_.dropped.end(), id) !=
            driver_.dropped.end())
            return json{{"ok", false}, {"reason", kSkipDiscoveryError}};
        json values = req.contains("values") && req["values"].is_array() ? req["values"]
                                                                         : json::array();
        return json{{"ok", true},
                    {"materialized", {{"handle", id}, {"summary", summarize(*tgt, values)}}}};
    }

    json invoke(const json& req) {
        const std::string id = req.value("targetId", std::string());
        const Tgt* tgt = require_target(id);
        json values = req.contains("values") && req["values"].is_array() ? req["values"]
                                                                         : json::array();
        double deadline = req.value("deadlineMs", 1000.0);
        bool collect = req.value("collectCoverage", false);
        ensure_driver();
        std::string profraw;
        if (haveCoverage_ && collect)
            profraw = driver_.profiles + "/inv" +
                      std::to_string(req.value<std::uint64_t>("invocationId", 0)) + ".profraw";
        RunResult run = run_call(driver_, *tgt, values, deadline, profraw);
        json outcome = classify(*tgt, run, deadline);
        if (collect) outcome["coverage"] = coverage_edges(profraw);
        return outcome;
    }

    json reproducer(const json& req) {
        const std::string id = req.value("targetId", std::string());
        const Tgt* tgt = require_target(id);
        json values = req.contains("values") && req["values"].is_array() ? req["values"]
                                                                         : json::array();
        std::string script = render_repro(inventory_, *tgt, values);
        // The script is only worth storing if it builds; the check uses the
        // project's own flags, in the driver's build directory.
        std::string stem = "repro-" + sha256_hex(script).substr(0, 16);
        std::string binary, error;
        if (!compile_repro(inventory_, driver_, options_, stem, script, binary, error))
            throw RpcError{kPluginError, error};
        return json{{"extension", "cpp"}, {"script", script}};
    }

    json coverage_map_method() {
        if (!haveInventory_) return json{{"entries", json::array()}};
        return coverage_map(inventory_.universe);
    }

    json ready() { return json{{"ready", true}, {"detail", nullptr}}; }

private:
    static std::vector<std::string> str_array(const json& j, const char* key) {
        std::vector<std::string> out;
        if (j.contains(key) && j[key].is_array())
            for (const auto& s : j[key])
                if (s.is_string()) out.push_back(s.get<std::string>());
        return out;
    }

    const Tgt* find_target(const std::string& id) {
        for (const Tgt& t : inventory_.targets)
            if (t.id == id) return &t;
        return nullptr;
    }

    const Tgt* require_target(const std::string& id) {
        if (!haveInventory_) throw RpcError{kInvalidRequest, "discover has not run"};
        const Tgt* t = find_target(id);
        if (!t) throw RpcError{kInvalidRequest, "no target with id " + id};
        return t;
    }

    void ensure_driver() {
        if (haveDriver_) return;
        std::string error;
        if (!build_driver(inventory_, options_, driver_, error))
            throw RpcError{kPluginError, error};
        haveDriver_ = true;
    }

    static const char* signal_name(int sig) {
        switch (sig) {
        case 4: return "SIGILL";
        case 6: return "SIGABRT";
        case 7: return "SIGBUS";
        case 8: return "SIGFPE";
        case 11: return "SIGSEGV";
        default: return "signal";
        }
    }

    // Map the driver's answer, or its absence, onto an outcome.
    json classify(const Tgt& tgt, const RunResult& run, double deadline) {
        json outcome;
        std::string kind;
        if (run.kind == RunKind::Deadline) {
            kind = kKindFailed;
            outcome["via"] = kViaDeadlineCut;
            outcome["fault"] = {{"class", "DeadlineCut"},
                                {"message", tgt.name + " did not answer within " +
                                                std::to_string(static_cast<long long>(deadline)) +
                                                " ms"},
                                {"frames", json::array()}};
        } else if (run.kind == RunKind::Silent) {
            kind = kKindFailed;
            std::string san = sanitizer_report(run.stderrText);
            if (!san.empty()) {
                outcome["via"] = kViaSanitizer;
                std::string cls = san.rfind("runtime error:", 0) == 0
                                      ? "UndefinedBehaviorSanitizer"
                                      : "AddressSanitizer";
                outcome["fault"] = {{"class", cls},
                                    {"message", san},
                                    {"frames", parse_sanitizer_frames(run.stderrText)}};
            } else {
                outcome["via"] = kViaDriverDied;
                outcome["fault"] = {{"class", "DriverDied"},
                                    {"message", run.detail},
                                    {"frames", json::array()}};
            }
        } else {
            const std::string status = run.answer.value("status", std::string());
            if (status == "returned") {
                kind = kKindOk;
                outcome["value"] =
                    run.answer.contains("value") ? run.answer["value"] : nullptr;
            } else if (status == "empty") {
                kind = kKindRejected;
                outcome["via"] = kViaGuardSite;
                outcome["detail"] = tgt.name + " returned an empty std::optional";
            } else if (status == "throw") {
                std::string type = run.answer.value("type", std::string("exception"));
                std::string what = run.answer.value("what", std::string());
                if (is_reject_class(type)) {
                    kind = kKindRejected;
                    outcome["via"] = kViaRejectClass;
                    outcome["detail"] = type + ": " + what;
                } else {
                    kind = kKindFailed;
                    outcome["via"] = kViaThrow;
                    outcome["fault"] = {{"class", type}, {"message", what}, {"frames", json::array()}};
                }
            } else if (status == "error") {
                kind = kKindFailed;
                outcome["via"] = kViaDriverError;
                outcome["fault"] = {{"class", "DriverError"},
                                    {"message", run.answer.value("message", std::string())},
                                    {"frames", json::array()}};
            } else if (status == "crash") {
                int sig = run.answer.value("signal", 0);
                kind = kKindFailed;
                outcome["via"] = kViaCrash;
                json frames = run.answer.contains("frames") && run.answer["frames"].is_array()
                                  ? resolve_frames(run.answer["frames"])
                                  : json::array();
                outcome["fault"] = {{"class", signal_name(sig)},
                                    {"message", std::string(tgt.name) + " crashed on " +
                                                   signal_name(sig)},
                                    {"frames", std::move(frames)}};
            } else {
                kind = kKindFailed;
                outcome["via"] = kViaDriverDied;
                outcome["fault"] = {{"class", "DriverDied"},
                                    {"message", "the driver answered " + status},
                                    {"frames", json::array()}};
            }
        }
        outcome["kind"] = kind;
        outcome["durationMs"] = run.durationMs;
        return outcome;
    }

    // std::logic_error and its family are the reject classes: they are how
    // well-behaved C++ code says "that input is not valid" and are the only
    // throw sites that count as a refusal rather than a fault.
    static bool is_reject_class(const std::string& type) {
        static const char* names[] = {"logic_error", "out_of_range", "length_error",
                                      "invalid_argument", "domain_error", "future_error",
                                      "bad_optional_access", "bad_variant_access",
                                      "bad_any_cast"};
        for (const char* n : names)
            if (type.find(n) != std::string::npos) return true;
        return false;
    }

    static std::string sanitizer_report(const std::string& err) {
        std::size_t pos = err.find("ERROR: AddressSanitizer:");
        if (pos == std::string::npos) pos = err.find("ERROR: LeakSanitizer:");
        if (pos == std::string::npos) pos = err.find("runtime error:");
        if (pos == std::string::npos) return "";
        std::size_t eol = err.find('\n', pos);
        return err.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    }

    // Fault frames are reported relative to the project root, so a frame
    // inside the target's own code names the same file as the descriptor's
    // `origin`.
    std::string rel_path(const std::string& file) const {
        if (!haveInventory_) return file;
        const std::string& root = inventory_.root;
        if (file.compare(0, root.size() + 1, root + "/") == 0)
            return file.substr(root.size() + 1);
        return file;
    }

    // ASan writes `    #N 0xADDR in <fn> <file>:<line>:<col>` lines; the
    // suite's crash property needs at least one frame naming a fixture file.
    json parse_sanitizer_frames(const std::string& err) {
        json out = json::array();
        std::size_t start = 0;
        while (start < err.size()) {
            std::size_t eol = err.find('\n', start);
            if (eol == std::string::npos) eol = err.size();
            std::string line = err.substr(start, eol - start);
            start = eol + 1;
            std::size_t body = line.find_first_not_of(" \t");
            if (body == std::string::npos || line[body] != '#') continue;
            std::size_t in = line.find(" in ", body);
            if (in == std::string::npos) continue;
            std::string tail = line.substr(in + 4);
            std::size_t sp = tail.rfind(' ');
            std::string file, fn = tail;
            unsigned lineNo = 0, col = 0;
            if (sp != std::string::npos) {
                std::string cand = tail.substr(sp + 1);
                std::size_t c1 = cand.rfind(':');
                std::size_t c2 = cand.rfind(':', c1 == 0 ? 0 : c1 - 1);
                if (c1 != std::string::npos && c2 != std::string::npos &&
                    cand.find_first_not_of("0123456789", c1 + 1) == std::string::npos) {
                    file = cand.substr(0, c2);
                    try {
                        lineNo = static_cast<unsigned>(std::stoul(cand.substr(c2 + 1, c1 - c2 - 1)));
                        col = static_cast<unsigned>(std::stoul(cand.substr(c1 + 1)));
                    } catch (...) {}
                    fn = tail.substr(0, sp);
                }
            }
            out.push_back(json{{"file", rel_path(file)},
                               {"line", lineNo},
                               {"column", col},
                               {"fnName", fn.empty() ? nullptr : json(fn)}});
        }
        return out;
    }

    // Crash frame addresses become source locations through the same LLVM
    // family that built the driver. Unresolvable addresses keep their hex.
    json resolve_frames(const json& frames) {
        json out = json::array();
        if (!haveOptions_ || addr2line_.empty() || frames.empty()) return out;
        std::string cmd = addr2line_ + " -e " + driver_.bin + " -f -C";
        for (const auto& f : frames)
            if (f.is_string()) cmd += " " + f.get<std::string>();
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return out;
        std::string text;
        char buf[4096];
        while (fgets(buf, sizeof buf, p)) text += buf;
        pclose(p);
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t eol = text.find('\n', start);
            if (eol == std::string::npos) eol = text.size();
            lines.push_back(text.substr(start, eol - start));
            start = eol + 1;
        }
        std::size_t frameIndex = 0;
        for (std::size_t i = 0; i + 1 < lines.size() && frameIndex < frames.size(); i += 2) {
            std::string file = lines[i + 1];
            unsigned line = 0;
            std::size_t colon = file.rfind(':');
            if (colon != std::string::npos) {
                try { line = static_cast<unsigned>(std::stoul(file.substr(colon + 1))); }
                catch (...) {}
                file = file.substr(0, colon);
            }
            json entry{{"file", file == "??" ? "" : rel_path(file)},
                       {"line", line},
                       {"fnName", lines[i] == "??" ? nullptr : json(lines[i])},
                       {"column", 0}};
            out.push_back(std::move(entry));
            ++frameIndex;
        }
        return out;
    }

    // The universe keys the raw profile attributes to this call, as edge ids.
    json coverage_edges(const std::string& profraw) {
        json edges = json::array();
        if (profraw.empty() || !haveCoverage_) return edges;
        struct stat st{};
        if (::stat(profraw.c_str(), &st) != 0 || st.st_size == 0) return edges;
        std::string error;
        auto hits = coverage_.hits(profraw, error);
        for (const auto& candidates : hits) {
            for (const std::string& key : candidates) {
                auto it = universe_.find(key);
                if (it == universe_.end()) continue;
                edges.push_back(it->second);
                break;
            }
        }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        ::unlink(profraw.c_str());
        return edges;
    }

    Inventory inventory_;
    json discovery_;
    Driver driver_;
    Options options_;
    Coverage coverage_;
    std::string addr2line_;
    std::unordered_map<std::string, std::uint32_t> universe_;
    bool haveInventory_ = false;
    bool haveDriver_ = false;
    bool haveOptions_ = false;
    bool haveCoverage_ = false;
};

} // namespace

} // namespace afzh

int main() {
    using namespace afzh;
    Harness harness;
    std::string line;
    while (read_line_stdin(line)) {
        json req = json::parse(line, nullptr, false);
        std::uint64_t id = req.is_object() && req.contains("id") && req["id"].is_number()
                               ? req["id"].get<std::uint64_t>()
                               : 0;
        if (req.is_discarded() || !req.is_object() || !req.contains("method") ||
            !req["method"].is_string()) {
            write_line_stdout(afz_dump(error_response(id, kParseError, "the request line is not a request")));
            continue;
        }
        const std::string method = req["method"].get<std::string>();
        json params = req.contains("params") && req["params"].is_object() ? req["params"]
                                                                          : json::object();
        try {
            json result;
            if (method == "handshake") result = harness.handshake();
            else if (method == "discover") result = harness.discover(params);
            else if (method == "prepare") result = harness.prepare(params);
            else if (method == "invoke") result = harness.invoke(params);
            else if (method == "reproducer") result = harness.reproducer(params);
            else if (method == "coverageMap") result = harness.coverage_map_method();
            else if (method == "ready") result = harness.ready();
            else
                throw Harness::RpcError{kMethodNotFound, "the cpp harness does not answer " + method};
            write_line_stdout(afz_dump(ok_response(id, std::move(result))));
        } catch (const Harness::RpcError& e) {
            write_line_stdout(afz_dump(error_response(id, e.code, e.message)));
        } catch (const json::exception& e) {
            write_line_stdout(afz_dump(error_response(id, kInvalidParams, e.what())));
        }
    }
    return 0;
}

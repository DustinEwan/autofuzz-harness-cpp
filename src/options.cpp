#include "options.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "wire.h"

namespace afzh {
namespace {

template <typename T>
void get(const json& o, const char* key, T& out) {
    if (o.contains(key) && !o.at(key).is_null()) out = o.at(key).get<T>();
}

void get_strings(const json& o, const char* key, std::vector<std::string>& out) {
    if (!o.contains(key) || o.at(key).is_null()) return;
    out = o.at(key).get<std::vector<std::string>>();
}

json read_file(const std::string& path, bool& ok) {
    ok = false;
    std::ifstream in(path);
    if (!in) return json();
    std::stringstream ss;
    ss << in.rdbuf();
    json parsed = json::parse(ss.str(), nullptr, false);
    if (parsed.is_discarded()) return json();
    ok = true;
    return parsed;
}

} // namespace

Options load_options(const std::string& configPath) {
    Options opt;
    if (configPath.empty()) return opt;
    // A project without autofuzz.json is normal: defaults, no error.
    // Only an unreadable or malformed file that exists is an error.
    std::error_code ec;
    if (!std::filesystem::exists(configPath, ec)) return opt;
    bool ok = false;
    json config = read_file(configPath, ok);
    if (!ok) {
        opt.error = "the harness could not read " + configPath;
        return opt;
    }
    if (!config.contains("harness") || !config["harness"].is_object() ||
        !config["harness"].contains("options") || config["harness"]["options"].is_null())
        return opt;
    const json& o = config["harness"]["options"];
    if (!o.is_object()) {
        opt.error = "harness.options must be a JSON object";
        return opt;
    }
    static const char* known[] = {"cxx", "std", "cxxFlags", "sources", "libraries",
                                  "linkFlags", "sanitize", "coverage", "llvmBin", "jobs"};
    for (auto& [key, value] : o.items()) {
        bool found = false;
        for (const char* k : known)
            if (key == k) found = true;
        if (!found) opt.unknown.push_back(key);
    }
    try {
        get(o, "cxx", opt.cxx);
        get(o, "std", opt.std);
        get(o, "coverage", opt.coverage);
        get(o, "jobs", opt.jobs);
        get_strings(o, "cxxFlags", opt.cxxFlags);
        get_strings(o, "sources", opt.sources);
        get_strings(o, "libraries", opt.libraries);
        get_strings(o, "linkFlags", opt.linkFlags);
        get_strings(o, "sanitize", opt.sanitize);
        if (o.contains("llvmBin") && !o["llvmBin"].is_null()) opt.llvmBin = o["llvmBin"].get<std::string>();
    } catch (const json::exception& e) {
        opt.error = std::string("harness.options: ") + e.what();
    }
    return opt;
}

std::string load_output_dir(const std::string& configPath) {
    if (configPath.empty()) return ".autofuzz";
    bool ok = false;
    json config = read_file(configPath, ok);
    if (!ok || !config.contains("output") || !config["output"].is_object()) return ".autofuzz";
    const json& out = config["output"];
    if (out.contains("dir") && out["dir"].is_string()) return out["dir"].get<std::string>();
    return ".autofuzz";
}

} // namespace afzh

// harness.options parsing: defaults for a missing file, the documented keys,
// and a typo'd key reported instead of silently dropped.
#include "options.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void write_file(const fs::path& p, const char* body) {
    std::ofstream out(p);
    out << body;
}

} // namespace

int main() {
    fs::path dir = fs::temp_directory_path() / "afz-cpp-options-test";
    fs::create_directories(dir);

    // Missing file: defaults, no error.
    {
        afzh::Options o = afzh::load_options((dir / "absent.json").string());
        check(o.error.empty(), "missing file is not an error");
        check(o.unknown.empty(), "missing file has no unknown keys");
        check(o.cxx == "clang++" && o.std == "c++20" && o.coverage, "defaults");
    }

    // Documented keys.
    {
        write_file(dir / "good.json", R"({
  "harness": { "options": {
    "cxx": "clang++-21", "std": "c++23",
    "cxxFlags": ["-DFOO=1"], "sources": ["src/core.cpp"],
    "libraries": ["libcore.a"], "linkFlags": ["-lpthread"],
    "sanitize": ["address", "undefined"], "coverage": false,
    "llvmBin": "/usr/lib/llvm-21/bin", "jobs": 3
  }},
  "output": { "dir": ".work" }
})");
        std::string p = (dir / "good.json").string();
        afzh::Options o = afzh::load_options(p);
        check(o.error.empty(), "good file parses");
        check(o.cxx == "clang++-21" && o.std == "c++23", "cxx and std");
        check(o.cxxFlags == std::vector<std::string>{"-DFOO=1"}, "cxxFlags");
        check(o.sources == std::vector<std::string>{"src/core.cpp"}, "sources");
        check(o.libraries == std::vector<std::string>{"libcore.a"}, "libraries");
        check(o.sanitize == std::vector<std::string>{"address", "undefined"}, "sanitize");
        check(!o.coverage, "coverage off");
        check(o.llvmBin == "/usr/lib/llvm-21/bin" && o.jobs == 3, "llvmBin and jobs");
        check(afzh::load_output_dir(p) == ".work", "output.dir");
    }

    // A typo is reported by name, never silently ignored.
    {
        write_file(dir / "typo.json",
                   R"({"harness": {"options": {"libraires": ["x"], "jobs": 2}}})");
        afzh::Options o = afzh::load_options((dir / "typo.json").string());
        check(o.unknown.size() == 1 && o.unknown[0] == "libraires", "typo reported by name");
        check(o.jobs == 2, "valid keys still apply");
    }

    // Malformed JSON: an error, not a crash.
    {
        write_file(dir / "bad.json", "{ not json");
        afzh::Options o = afzh::load_options((dir / "bad.json").string());
        check(!o.error.empty(), "malformed json reports an error");
    }

    fs::remove_all(dir);
    return failures == 0 ? 0 : 1;
}

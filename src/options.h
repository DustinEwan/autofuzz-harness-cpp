// `harness.options` from the scan project file. The engine never reads this
// object (crates/afz-engine/src/config.rs:68); the plugin owns its schema and
// reports unknown keys as a discovery error rather than silently ignoring a
// typo like `libraires`.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace afzh {

struct Options {
    std::string cxx = "clang++";
    std::string std = "c++20";
    std::vector<std::string> cxxFlags;
    std::vector<std::string> sources;
    std::vector<std::string> libraries;
    std::vector<std::string> linkFlags;
    std::vector<std::string> sanitize; // subset of address, undefined
    bool coverage = true;
    std::string llvmBin; // empty: fall back to $AFZ_LLVM_BIN, then PATH
    int jobs = 0;        // 0: std::thread::hardware_concurrency()

    // Non-empty when a key outside the schema appeared; the names are exactly
    // what the discovery-error skip must report.
    std::vector<std::string> unknown;
    // Non-empty when the file or the object could not be read at all.
    std::string error;
};

// Read `harness.options` from the JSON at `configPath`. A missing file or a
// missing options object yields the defaults with no error.
Options load_options(const std::string& configPath);

// `output.dir` from the same file, defaulting to `.autofuzz`.
std::string load_output_dir(const std::string& configPath);

} // namespace afzh

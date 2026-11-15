// Render the conformance fixture project for the C++ harness.
//
// `afz validate-harness` validates a plugin against the template
// (`crates/afz-conformance/template/v1.json` in the engine repository):
// twelve semantic properties stated as wire-level expectations. This emitter
// writes a real C++ project - one header, one source file, one symbol per
// property - the `autofuzz.json` that points the plugin at it, and
// `afz-conformance-manifest.json` naming the symbol and inputs behind each
// property. The suite, not this file, decides pass/fail.
//
// C++ realizes every property: internal linkage, signature blockers,
// unconstructible receivers, method receivers, and two refusal channels all
// exist in the language, and the driver child keeps the protocol channel
// clean while target code shouts at its own stdout.
//
// Usage:
//   afz-cpp-emit --out /tmp/conformance-cpp

#include <cstdio>
#include <string>

#include <unistd.h>

#include "wire.h"

namespace {

constexpr const char* kHeader = R"CPP(// Conformance fixture for the afz-cpp harness: one symbol per template property.
#pragma once

#include <optional>
#include <string>

// crash-unguarded: indexing past the end of a fixed array reads out of
// bounds; AddressSanitizer reports it inside this fixture's source.
int crash(int index);

// guard-refusal: the guard the author wrote; out-of-range input returns an
// empty optional, the language's ordinary refusal channel.
std::optional<int> bounded(int amount);

// unrepresentable-symbol: a variadic signature the generator cannot drive.
int variadic(const char* fmt, ...);

// unconstructible-receiver: Ledger keeps a private field, so no value tree
// can build the receiver of read().
class Ledger {
    int secret_;

public:
    explicit Ledger(int secret) : secret_(secret) {}
    int read() const;
};

// method-receiver: fully public, default-constructible, one plain method.
struct Counter {
    int value;
    int bump(int by);
};

// coverage-digest-live: the two arms call two distinct functions, so two
// inputs execute two different function sets.
int branch(int mode);

// stdout-purity: target code shouting at stdout must not corrupt the
// protocol channel.
void noise(int lines);

// deadline-sleep: sleeps `ms` milliseconds; the suite's deadline is shorter.
void sleeper(int ms);

// opaque-return: a void* carries no shape the wire vocabulary can express.
void* reading(int index);

// unbuildable-symbol: discovery accepts NoCopy (default constructor, no
// fields), but the generated call must copy it and the copy constructor is
// deleted, so the driver build fails on this thunk alone.
struct NoCopy {
    NoCopy() = default;
    NoCopy(const NoCopy&) = delete;
};
int broken(NoCopy value);

// Ordinary targets so the fixture fuzzes like a real project.
int add(int a, int b);
std::string greet(std::string name);
)CPP";

constexpr const char* kSource = R"CPP(// Conformance fixture implementation.
#include "include/probe.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

// private-symbol: otherwise fuzzable-shaped, so the skip can only come from
// visibility (internal linkage).
int hidden(int value) { return value + 1; }

int left_arm(int mode) { return mode + 1; }
int right_arm(int mode) { return mode - 1; }

} // namespace

int crash(int index) {
    static int values[3] = {1, 2, 3};
    return values[index];
}

std::optional<int> bounded(int amount) {
    if (amount > 100) return std::nullopt;
    return amount;
}

int variadic(const char* fmt, ...) {
    (void)fmt;
    return 0;
}

int Ledger::read() const { return secret_; }

int Counter::bump(int by) {
    value += by;
    return value;
}

int branch(int mode) {
    if (mode > 0) return right_arm(mode);
    return left_arm(mode);
}

void noise(int lines) {
    for (int i = 0; i < lines; ++i) std::printf("noise %d\n", i);
    std::fflush(stdout);
}

void sleeper(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void* reading(int index) {
    static int storage[4] = {1, 2, 3, 4};
    return &storage[index & 3];
}

int broken(NoCopy value) {
    (void)value;
    return hidden(6);
}

int add(int a, int b) { return a + b; }

std::string greet(std::string name) { return "hello, " + name; }
)CPP";

// The scan project file. AddressSanitizer rides so the crash property faults
// with a report and frames inside the fixture; coverage rides so the digest
// property has a toolchain to read.
std::string project_file() {
    afzh::json j{{"harness", {{"options", {{"cxx", "clang++"},
                                           {"std", "c++20"},
                                           {"cxxFlags", afzh::json::array()},
                                           {"sources", afzh::json::array({"src/probe.cpp"})},
                                           {"sanitize", afzh::json::array({"address"})},
                                           {"coverage", true}}}}}};
    return j.dump(2) + "\n";
}

afzh::json user(afzh::json node) {
    node["provenance"] = "user";
    return node;
}

std::string manifest(const std::string& checker) {
    using afzh::json;
    auto num = [](double v) { return json{{"node", "number"}, {"value", v}}; };
    json counterReceiver = {{"node", "object"},
                            {"fields", json::array({json::array(
                                            {"value", user(num(1))})})}};
    json props{
        {"crash-unguarded", {{"symbol", "crash"}, {"input", json::array({user(num(7))})}}},
        {"guard-refusal", {{"symbol", "bounded"}, {"input", json::array({user(num(500))})}}},
        {"private-symbol", {{"symbol", "hidden"}}},
        {"unrepresentable-symbol", {{"symbol", "variadic"}}},
        {"unconstructible-receiver",
         {{"symbol", "Ledger::read"}, {"blockedByType", "Ledger"}}},
        {"method-receiver",
         {{"symbol", "Counter::bump"},
          {"input", json::array({counterReceiver, user(num(2))})}}},
        {"coverage-digest-live",
         {{"symbol", "branch"},
          {"inputs", json::array({json::array({user(num(0))}), json::array({user(num(1))})})}}},
        {"stdout-purity", {{"symbol", "noise"}, {"input", json::array({user(num(8))})}}},
        {"reproducer-script", {{"symbol", "crash"}, {"input", json::array({user(num(7))})}}},
        {"deadline-sleep", {{"symbol", "sleeper"}, {"input", json::array({user(num(3000))})}}},
        {"opaque-return", {{"symbol", "reading"}, {"input", json::array({user(num(1))})}}},
        {"unbuildable-symbol", {{"symbol", "broken"},
                                {"input", json::array({json{{"node", "object"}, {"fields", json::array()}}})}}},
    };
    json m{{"templateVersion", 1},
           {"properties", props},
           {"validator", json::array({checker, "src/probe.cpp"})}};
    return m.dump(2) + "\n";
}

bool write_file(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return true;
}

std::string self_dir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string exe(buf);
    auto slash = exe.rfind('/');
    return slash == std::string::npos ? std::string(".") : exe.substr(0, slash);
}

} // namespace

int main(int argc, char** argv) {
    std::string out;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--out" && i + 1 < argc) out = argv[++i];
    if (out.empty()) {
        std::fprintf(stderr, "usage: afz-cpp-emit --out DIR\n");
        return 2;
    }
    std::string cmd = "mkdir -p '" + out + "/include' '" + out + "/src'";
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "cannot create %s\n", out.c_str());
        return 1;
    }
    bool ok = write_file(out + "/include/probe.h", kHeader) &&
              write_file(out + "/src/probe.cpp", kSource) &&
              write_file(out + "/autofuzz.json", project_file()) &&
              write_file(out + "/afz-conformance-manifest.json",
                         manifest(self_dir() + "/afz-cpp-check"));
    if (!ok) {
        std::fprintf(stderr, "cannot write the fixture under %s\n", out.c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote the conformance fixture to %s\n", out.c_str());
    return 0;
}

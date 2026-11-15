// The generated driver: render `afz_calls.cpp`, build it with a repair loop,
// and run one call in a fresh child per invocation.
//
// Discovery cannot compile, so a symbol the parser accepted can still fail to
// compile in the driver. The repair loop attributes every compiler error that
// points into the generated file to the target whose marker range holds the
// line, drops that target, and rebuilds. A dropped target is answered
// `discovery-error` by `prepare`: the signature was acceptable to discovery
// and the failure is the harness's, not the input's.
#pragma once

#include <string>
#include <vector>

#include "discover.h"
#include "options.h"
#include "wire.h"

namespace afzh {

struct Driver {
    std::string dir;      // the scratch directory
    std::string bin;      // <dir>/build/afz-cpp-driver
    std::string profiles; // <dir>/profiles
    std::vector<std::string> dropped;
    bool binExists = false;
};

// Render and build. On success `dropped` names the targets the repair loop
// removed. A false return means the build never converged or a tool is
// missing; `error` carries the compiler tail.
bool build_driver(const Inventory& inv, const Options& opt, Driver& out, std::string& error);

enum class RunKind { Answered, Silent, Deadline };

struct RunResult {
    RunKind kind = RunKind::Silent;
    json answer;          // Answered: the driver's status object
    std::string detail;   // Silent: exit description plus the stderr tail
    std::string stderrText; // raw stderr, for sanitizer classification
    double durationMs = 0;
};

// Run one call. `profraw` empty means coverage is off for this call.
RunResult run_call(const Driver& drv, const Tgt& tgt, const json& args, double deadlineMs,
                   const std::string& profraw);

// Compile a reproducer script in the driver's build directory. A successful
// binary is kept; a failed compile deletes the script.
bool compile_repro(const Inventory& inv, const Driver& drv, const Options& opt,
                   const std::string& stem, const std::string& script, std::string& binaryOut,
                   std::string& error);

// Resolve an llvm-* tool: options.llvmBin, then $AFZ_LLVM_BIN, then PATH.
std::string find_llvm_tool(const std::string& tool, const std::string& llvmBin);

// One line describing the call for `MaterializedCall.summary`.
std::string summarize(const Tgt& tgt, const json& values);

// Render `afz_calls.cpp` for the inventory minus `dropped`. Exposed for the
// unit tests; the build path calls it internally.
std::string render_calls(const Inventory& inv, const std::vector<std::string>& dropped,
                         std::vector<std::pair<std::string, std::pair<int, int>>>& spans);

// Render the standalone reproducer script: the runtime, the object builders,
// one thunk, and a main that replays the embedded values. `values` are the
// ValueTree arguments as they arrived on the wire.
std::string render_repro(const Inventory& inv, const Tgt& tgt, const json& values);

} // namespace afzh

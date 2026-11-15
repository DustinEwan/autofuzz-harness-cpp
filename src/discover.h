// Discovery: walk the root, parse each candidate file with libclang, and
// produce the target inventory, the skipped list, and the coverage universe.
// Nothing here executes target code; the handshake says
// `discoverRequiresExecution: false`.
#pragma once

#include <string>
#include <vector>

#include "lower.h"
#include "options.h"
#include "wire.h"

namespace afzh {

// How the generated thunk passes one built local to the call.
enum class Pass {
    Value,      // pass the local directly
    Ref,        // pass the local as an lvalue (reference parameter)
    Ptr,        // pass &local
    CStrConst,  // pass local.c_str()
    CStrMut,    // pass local.data()
    Buffer,     // pass local.data() plus the folded length argument
    FnPtr,      // pass the stub; converts to a function pointer
};

struct Param {
    std::string name;
    json node;      // TypeNode
    BuildPtr build; // construction tree
    Pass pass = Pass::Value;
    bool mutableOut = false; // appears in the `ok` value object when mutated
    std::string lengthType;  // Buffer: C++ type of the folded length parameter
};

struct Tgt {
    std::string id;
    std::string name;     // qualified C++ name, `(types)` suffix when overloaded
    std::string callPath; // the same name as written in a call
    std::string file;     // root-relative, '/'-separated
    unsigned line = 0;
    std::string module;   // namespace path, "" at global scope
    bool isMethod = false;
    std::vector<Param> params; // params[0] is the receiver for methods
    json returnsNode;          // TypeNode, null for void
    BuildPtr returnBuild;      // null when the return is not encodable
    bool returnsVoid = false;
    bool returnsOptional = false; // std::optional return: empty -> `rejected`
    json descriptor;              // the full descriptor, ready to serialize
};

struct UniverseEntry {
    std::string key; // qualified name, matching the demangled profile name
    std::string file;
    unsigned start = 0;
    unsigned end = 0;
};

struct Inventory {
    std::string root;
    std::string scratch; // <root>/<output.dir>/cpp
    json discovery;      // {"targets": [...], "skipped": [...]}
    std::vector<Tgt> targets;
    std::vector<UniverseEntry> universe;
    std::vector<std::string> headers; // afz_calls.cpp includes these once
    LowerCtx lower;                   // object builders live here
    std::vector<std::string> dropped; // filled by the driver's repair loop
};

// Run discovery. On a structural failure (the root cannot be read) returns
// false with `error` set; a file that fails to parse is a skip entry, not an
// error. `configPath` names the scan project file whose `harness.options`
// carries the build configuration.
bool discover(const std::string& root, const std::vector<std::string>& include,
              const std::vector<std::string>& exclude, const std::string& configPath,
              Inventory& out, std::string& error);

// The stable id for a symbol: "afz:t:" plus the first 6 bytes of
// SHA-256(relFile + NUL + USR + NUL + signatureText).
std::string target_id(const std::string& relFile, const std::string& usr,
                      const std::string& signatureText);

} // namespace afzh

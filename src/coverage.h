// Coverage: find `llvm-profdata`, read one call's raw profile, and attribute
// the executed functions to the discovery universe.
//
// Parsing the raw profile by hand would tie the harness to one LLVM
// raw-profile revision, so the plugin reads it with the `llvm-profdata` from
// the same toolchain family that compiled the driver. When the component is
// absent the handshake drops `coverage` and every other method still works.
//
// A hit is a function whose first counter - the function-entry counter - is
// non-zero. The name `llvm-profdata` prints is the mangled symbol; the plugin
// demangles it and cuts the parameter list, which yields the qualified-name
// spelling the universe is keyed by. Static functions that ran inside a call
// are attributed too.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "discover.h"
#include "wire.h"
namespace afzh {

// The edge id a universe key gets, in the outcome coverage list and in the
// coverage map alike: the first four bytes of SHA-256(key), big-endian.
std::uint32_t edge_id(const std::string& key);

struct Coverage {
    std::string profdata; // resolved llvm-profdata path

    // One candidate key list per executed function, best first: the demangled
    // name with the parameter list cut, the same name with template
    // arguments cut, and the raw symbol. The caller attributes a function by
    // its first candidate that names a universe entry.
    std::vector<std::vector<std::string>> hits(const std::string& profraw, std::string& error);
};

// The coverage map: every universe function, public or private, as a stable
// edge id plus its source range.
json coverage_map(const std::vector<UniverseEntry>& universe);

// Demangle a C++ symbol; returns the input when the symbol is not mangled.
std::string demangle_cxx(const std::string& symbol);

} // namespace afzh

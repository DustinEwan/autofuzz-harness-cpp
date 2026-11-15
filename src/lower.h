// C++ type -> TypeNode + Build. The TypeNode goes on the wire in the target
// descriptor; the Build tree is what `src/driver.cpp` renders into the
// generated thunk: how to build a local from a JSON argument, how to pass it
// to the call, and how to encode a value of it back into a ValueTree.
#pragma once

#include <clang-c/Index.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "wire.h"

namespace afzh {

struct Build;
using BuildPtr = std::shared_ptr<Build>;

// One node of the construction tree. `localType` is the C++ type of the local
// the thunk declares; the driver codegen switches on `kind` for everything
// else. Width bounds match the Rust harness: strings <= 4096 characters,
// collections <= 64 items.
struct Build {
    enum class Kind {
        Bool, Int, UInt, Float, Enum,
        String,     // std::string by value or reference
        CStr,       // const char* / char*: local is a std::string, passed as .c_str()/.data()
        StringView, // std::string_view over a backing std::string local
        BufferPtr,  // byte pointer with a folded-in length parameter
        BytesVec,   // std::vector<uint8_t>-style buffer container
        Span,       // std::span<uint8_t> over a backing buffer local
        Seq,        // std::vector<T> / std::deque<T>
        Set,        // std::set<T> / std::unordered_set<T>
        Map,        // std::map<std::string, V> / unordered
        Tuple,      // std::array<T,N> / std::pair / std::tuple
        Optional,   // std::optional<T>
        Variant,    // std::variant<...>
        Object,     // project class or struct
        Ref,        // recursive self-reference inside an Object
        Fn,         // function pointer or std::function
        PtrWrap,    // std::unique_ptr<T> / std::shared_ptr<T>
        FieldPtr,   // pointer or string_view data member: the pointee is built into a heap copy
    } kind = Kind::Bool;

    std::string localType; // C++ type of the local
    std::string spelled;   // original spelling, for messages

    // Int / UInt / Enum / Float:
    bool isUnsigned = false;
    long long minBound = 0; // integer bounds for the TypeNode
    long long maxBound = 0;
    unsigned long long uMaxBound = 0;

    // CStr: true when the parameter is `char*` (mutable, passed as .data()).
    bool mutableCharPtr = false;
    // BufferPtr: the pointee spelling and the size of one element.
    std::string pointee;
    std::size_t elemSize = 1;

    // Seq / Set / BytesVec element, Map value, Optional inner, PtrWrap inner:
    BuildPtr elem;
    std::string container; // "std::vector", "std::set", "std::map", ...

    // Map key: only std::string keys lower; the key build is always String.
    // Tuple items / Variant members:
    std::vector<BuildPtr> items;
    std::size_t fixedSize = 0; // std::array<T, N>
    bool isPair = false;
    bool isTuple = false;

    // Object / Ref:
    std::string qualifiedName; // project class name
    std::string builderFn;     // afz_build_<k>
    std::string encoderFn;     // afz_enc_<k>
    std::vector<std::pair<std::string, BuildPtr>> fields; // public data members

    // Fn:
    std::string returnType;
    std::vector<std::string> argTypes;
};

// One project object type's generated builder and encoder. Insertion order is
// generation order.
struct ObjectDef {
    std::string qualifiedName;
    std::string localType;
    std::string builderFn;
    std::string encoderFn;
    std::vector<std::pair<std::string, BuildPtr>> fields;
};

struct LowerCtx {
    CXIndex index = nullptr;
    // Qualified names currently being lowered: a cycle through a pointer
    // member becomes a `ref` node back to the object carrying `id`.
    std::vector<std::string> stack;
    std::vector<ObjectDef> objects;
    std::map<std::string, std::size_t> objectIndex;
};

struct LowerResult {
    enum class Status { Ok, Opaque, Refuse } status = Status::Ok;
    json node;            // TypeNode when Ok
    BuildPtr build;       // when Ok
    std::string opaqueTy; // type spelling for the opaque{} node
    std::string blockerTy; // deepest unbuildable type, for blockedBy.type
    std::string detail;    // human-readable reason
};

// Lower one C++ type. Const-qualification is stripped: a const parameter still
// gets a non-const local so a mutated value can be read back.
LowerResult lower_type(CXType type, LowerCtx& ctx);

// Dispose-safe spelling of a type.
std::string type_spelling(CXType t);
std::string cursor_spelling(CXCursor c);
std::string fully_qualified_name(CXCursor c);

} // namespace afzh

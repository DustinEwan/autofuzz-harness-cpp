// The afz/1 wire layer: line-delimited JSON-RPC framing, the error codes, and
// every literal vocabulary string the plugin can emit. Nothing here is shared
// with the engine; `tests/wire_literals.cpp` pins these against the schema.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace afzh {

using json = nlohmann::json;

// docs/schema/afz-1/request.json and response.json.
inline constexpr const char* kJsonRpc = "2.0";
// crates/afz-protocol/src/rpc.rs and docs/harness-authoring.md section 12.
inline constexpr const char* kProtocolVersion = "afz/1";

// crates/afz-protocol/src/rpc.rs: PARSE_ERROR, INVALID_REQUEST,
// METHOD_NOT_FOUND, INVALID_PARAMS, PLUGIN_ERROR.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kPluginError = -32000;

// docs/schema/afz-1/discovery.json SkipReason.
inline constexpr const char* kSkipUnconstructible = "unconstructible-parameter";
inline constexpr const char* kSkipDiscoveryError = "discovery-error";
inline constexpr const char* kSkipNotPublic = "not-public";
inline constexpr const char* kSkipExcluded = "excluded";
inline constexpr const char* kSkipUnsupportedSignature = "unsupported-signature";
inline constexpr const char* kSkipTargetDisabled = "target-disabled";

// docs/schema/afz-1/discovery.json Materialization.
inline constexpr const char* kMatConstructible = "constructible";
inline constexpr const char* kMatFixtureRequired = "fixture-required";
inline constexpr const char* kMatOpaque = "opaque";

// The `via` strings this plugin puts on outcomes. The engine treats them as
// free text; the conformance template names `guard-site`, `reject-class`, and
// `deadline-cut`.
inline constexpr const char* kViaGuardSite = "guard-site";
inline constexpr const char* kViaRejectClass = "reject-class";
inline constexpr const char* kViaThrow = "throw";
inline constexpr const char* kViaCrash = "crash";
inline constexpr const char* kViaSanitizer = "sanitizer";
inline constexpr const char* kViaDriverError = "driver-error";
inline constexpr const char* kViaDriverDied = "driver-died";
inline constexpr const char* kViaDeadlineCut = "deadline-cut";

// docs/schema/afz-1/outcome.json OutcomeKind.
inline constexpr const char* kKindOk = "ok";
inline constexpr const char* kKindRejected = "rejected";
inline constexpr const char* kKindFailed = "failed";

// docs/schema/afz-1/type-node.json Kind discriminators.
inline constexpr const char* kNodeScalar = "scalar";
inline constexpr const char* kNodeLiteral = "literal";
inline constexpr const char* kNodeCollection = "collection";
inline constexpr const char* kNodeObject = "object";
inline constexpr const char* kNodeTuple = "tuple";
inline constexpr const char* kNodeComposite = "composite";
inline constexpr const char* kNodeBuffer = "buffer";
inline constexpr const char* kNodeFunction = "function";
inline constexpr const char* kNodeOpaque = "opaque";
inline constexpr const char* kNodeRef = "ref";

// ScalarKind, CollectionKind and CompositeKind enum members.
inline constexpr const char* kScalarNull = "null";
inline constexpr const char* kScalarBoolean = "boolean";
inline constexpr const char* kScalarNumber = "number";
inline constexpr const char* kScalarBigInt = "bigInt";
inline constexpr const char* kScalarString = "string";
inline constexpr const char* kCollArray = "array";
inline constexpr const char* kCollSet = "set";
inline constexpr const char* kCollMap = "map";
inline constexpr const char* kCompUnion = "union";

// A parsed request line. `params` is absent for handshake, coverageMap, ready.
struct Request {
    std::uint64_t id = 0;
    std::string method;
    json params;
    bool hasParams = false;
};

// Reads one line from stdin into `line`. False at EOF. A blank line is EOF by
// the transport rule (docs/harness-authoring.md section 12).
bool read_line_stdin(std::string& line);

// Writes one protocol line to stdout and flushes. False when the pipe is gone;
// the caller then exits, because the engine reads silence either way.
bool write_line_stdout(std::string_view line);

json ok_response(std::uint64_t id, json result);
json error_response(std::uint64_t id, int code, std::string_view message);

// True when `pattern` matches `path` (root-relative, '/'-separated). A pattern
// with no '/' matches the file name alone; `**` crosses directory boundaries.
// Mirrors crates/afz-engine/src/config.rs:60-66 and docs/harness-authoring.md
// section 3.
bool glob_match(std::string_view pattern, std::string_view path);

} // namespace afzh

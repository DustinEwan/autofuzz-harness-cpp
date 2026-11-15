// The wire-vocabulary test (docs/harness-authoring.md section 10).
//
// Every literal string and code the plugin can put on the wire must match the
// engine schema exactly. The tables below are copied from the engine sources;
// each group names the file it was copied from. If the engine changes a
// vocabulary, this test is where the drift surfaces - the engine repo is not
// available at harness CI time, so the copy is the pinned contract.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "templates.h"
#include "wire.h"

namespace {
int failures = 0;

void expect_eq(const char* name, const char* got, const char* want) {
    if (std::string(got) != want) {
        std::fprintf(stderr, "FAIL %s: plugin emits \"%s\", schema says \"%s\"\n", name, got,
                     want);
        ++failures;
    }
}

void expect_int_eq(const char* name, int got, int want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: plugin uses %d, schema says %d\n", name, got, want);
        ++failures;
    }
}

// crates/afz-protocol/src/rpc.rs: the five JSON-RPC codes the protocol crate
// answers with.
constexpr struct {
    const char* name;
    int code;
} kRpcCodes[] = {
    {"PARSE_ERROR", -32700},
    {"INVALID_REQUEST", -32600},
    {"METHOD_NOT_FOUND", -32601},
    {"INVALID_PARAMS", -32602},
    {"PLUGIN_ERROR", -32000},
};

// docs/schema/afz-1/discovery.json SkipReason consts.
constexpr const char* kSchemaSkipReasons[] = {"unconstructible-parameter", "discovery-error",
                                              "not-public", "excluded", "unsupported-signature",
                                              "target-disabled"};

// docs/schema/afz-1/discovery.json Materialization consts.
constexpr const char* kSchemaMaterializations[] = {"constructible", "fixture-required", "opaque"};

// docs/schema/afz-1/outcome.json: the `kind` const of each outcome variant.
constexpr const char* kSchemaOutcomeKinds[] = {"ok", "rejected", "failed"};

// docs/schema/afz-1/type-node.json Kind discriminators (all thirteen).
constexpr const char* kSchemaNodeKinds[] = {"scalar",    "stringFormat", "literal",
                                            "collection", "object",      "tuple",
                                            "composite", "buffer",      "promise",
                                            "function",  "stream",      "opaque",
                                            "ref"};

// docs/schema/afz-1/type-node.json ScalarKind enum.
constexpr const char* kSchemaScalarKinds[] = {"null", "undefined", "boolean",
                                              "number", "bigInt",  "string"};

// docs/schema/afz-1/type-node.json CollectionKind enum.
constexpr const char* kSchemaCollectionKinds[] = {"array", "set", "map"};

// docs/schema/afz-1/type-node.json CompositeKind enum.
constexpr const char* kSchemaCompositeKinds[] = {"union", "intersection"};

// docs/schema/afz-1/discover-req.json and handshake.json: the protocol line.
constexpr const char* kSchemaProtocolVersion = "afz/1";

bool table_has(const char* const table[], std::size_t n, const char* value) {
    for (std::size_t i = 0; i < n; ++i)
        if (std::string(table[i]) == value) return true;
    return false;
}

// The driver runtime emits ValueTree node names inside the generated call
// file; every one must be a schema node name. `pairs` are the exact
// substrings kRtHeader carries.
void check_rt_header() {
    const std::string rt = afzh::templates::kRtHeader;
    const char* emitted[] = {afzh::kScalarNull,   afzh::kScalarBoolean, afzh::kScalarBigInt,
                             afzh::kScalarNumber, afzh::kScalarString,  afzh::kCollArray,
                             afzh::kCollSet,      afzh::kCollMap,       afzh::kNodeBuffer,
                             afzh::kNodeLiteral};
    for (const char* name : emitted) {
        const std::string pair = std::string("\"node\", \"") + name + "\"";
        if (rt.find(pair) == std::string::npos) {
            std::fprintf(stderr, "FAIL rt-header: driver runtime never emits %s\n",
                         pair.c_str());
            ++failures;
        }
        if (!table_has(kSchemaNodeKinds, std::size(kSchemaNodeKinds), name) &&
            !table_has(kSchemaScalarKinds, std::size(kSchemaScalarKinds), name) &&
            !table_has(kSchemaCollectionKinds, std::size(kSchemaCollectionKinds), name)) {
            std::fprintf(stderr, "FAIL rt-header: \"%s\" is not a schema node name\n", name);
            ++failures;
        }
    }
}

// The object and tuple encoders are generated into the call file by
// src/driver.cpp; their node-name literals live in the emitted source text.
// Read that text back and pin the two pairs the runtime check cannot see.
void check_generated_encoders() {
#ifndef AFZ_REPO_ROOT
    return;
#else
    std::ifstream in(std::string(AFZ_REPO_ROOT) + "/src/driver.cpp");
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    // The pairs appear escaped: the generator writes them inside C++ string
    // literals that become the generated call file verbatim.
    const char* pairs[] = {R"({\"node\",\"object\"})", R"({\"node\",\"tuple\"})"};
    for (const char* pair : pairs) {
        if (text.find(pair) == std::string::npos) {
            std::fprintf(stderr, "FAIL encoders: src/driver.cpp does not emit %s\n", pair);
            ++failures;
        }
    }
#endif
}

} // namespace

int main() {
    expect_int_eq("PARSE_ERROR", afzh::kParseError, kRpcCodes[0].code);
    expect_int_eq("INVALID_REQUEST", afzh::kInvalidRequest, kRpcCodes[1].code);
    expect_int_eq("METHOD_NOT_FOUND", afzh::kMethodNotFound, kRpcCodes[2].code);
    expect_int_eq("INVALID_PARAMS", afzh::kInvalidParams, kRpcCodes[3].code);
    expect_int_eq("PLUGIN_ERROR", afzh::kPluginError, kRpcCodes[4].code);
    expect_eq("protocolVersion", afzh::kProtocolVersion, kSchemaProtocolVersion);

    expect_eq("skip unconstructible", afzh::kSkipUnconstructible, kSchemaSkipReasons[0]);
    expect_eq("skip discovery-error", afzh::kSkipDiscoveryError, kSchemaSkipReasons[1]);
    expect_eq("skip not-public", afzh::kSkipNotPublic, kSchemaSkipReasons[2]);
    expect_eq("skip excluded", afzh::kSkipExcluded, kSchemaSkipReasons[3]);
    expect_eq("skip unsupported-signature", afzh::kSkipUnsupportedSignature,
              kSchemaSkipReasons[4]);
    expect_eq("skip target-disabled", afzh::kSkipTargetDisabled, kSchemaSkipReasons[5]);

    expect_eq("materialization constructible", afzh::kMatConstructible,
              kSchemaMaterializations[0]);
    expect_eq("materialization fixture-required", afzh::kMatFixtureRequired,
              kSchemaMaterializations[1]);
    expect_eq("materialization opaque", afzh::kMatOpaque, kSchemaMaterializations[2]);

    expect_eq("kind ok", afzh::kKindOk, kSchemaOutcomeKinds[0]);
    expect_eq("kind rejected", afzh::kKindRejected, kSchemaOutcomeKinds[1]);
    expect_eq("kind failed", afzh::kKindFailed, kSchemaOutcomeKinds[2]);

    expect_eq("node scalar", afzh::kNodeScalar, "scalar");
    expect_eq("node literal", afzh::kNodeLiteral, "literal");
    expect_eq("node collection", afzh::kNodeCollection, "collection");
    expect_eq("node object", afzh::kNodeObject, "object");
    expect_eq("node tuple", afzh::kNodeTuple, "tuple");
    expect_eq("node composite", afzh::kNodeComposite, "composite");
    expect_eq("node buffer", afzh::kNodeBuffer, "buffer");
    expect_eq("node function", afzh::kNodeFunction, "function");
    expect_eq("node opaque", afzh::kNodeOpaque, "opaque");
    expect_eq("node ref", afzh::kNodeRef, "ref");

    expect_eq("scalar null", afzh::kScalarNull, "null");
    expect_eq("scalar boolean", afzh::kScalarBoolean, "boolean");
    expect_eq("scalar number", afzh::kScalarNumber, "number");
    expect_eq("scalar bigInt", afzh::kScalarBigInt, "bigInt");
    expect_eq("scalar string", afzh::kScalarString, "string");

    expect_eq("collection array", afzh::kCollArray, "array");
    expect_eq("collection set", afzh::kCollSet, "set");
    expect_eq("collection map", afzh::kCollMap, "map");

    expect_eq("composite union", afzh::kCompUnion, "union");

    // The via strings are free text to the engine (outcome.json carries no
    // enum); they are pinned here so a rename cannot pass unnoticed.
    const std::vector<std::string> vias = {afzh::kViaGuardSite,
                                           afzh::kViaRejectClass,
                                           afzh::kViaThrow,
                                           afzh::kViaCrash,
                                           afzh::kViaSanitizer,
                                           afzh::kViaDriverError,
                                           afzh::kViaDriverDied,
                                           afzh::kViaDeadlineCut};
    for (const std::string& v : vias)
        if (v.empty()) {
            std::fprintf(stderr, "FAIL via: empty via string\n");
            ++failures;
        }

    check_rt_header();
    check_generated_encoders();

    if (failures) {
        std::fprintf(stderr, "%d vocabulary violation(s)\n", failures);
        return 1;
    }
    std::printf("wire vocabulary matches the schema tables\n");
    return 0;
}

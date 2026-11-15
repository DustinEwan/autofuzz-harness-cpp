// The conformance project's own fixture validator, for the C++ harness.
//
// The protocol judge only ever sees the plugin's statements about the
// rendered project. This binary is the second judgment: it reads
// `src/probe.cpp` with libclang - no protocol, no plugin, no driver build in
// the path - and confirms each fixture-shaped property from the source that
// is supposed to realize it. The suite requires the two verdict lists to
// agree.
//
// Contract (docs/harness-authoring.md, section 12): stdout is exactly one
// JSON `TemplateReport` and nothing else; exit 0 even when a property fails;
// non-zero only when this validator itself could not run; the seven
// fixture-shaped ids only.
//
// Usage:
//   afz-cpp-check [path/to/src/probe.cpp]

#include <clang-c/Index.h>

#include <cstdio>
#include <string>
#include <vector>

#include <filesystem>

#include "wire.h"

namespace fs = std::filesystem;
using afzh::json;

namespace {

struct Report {
    std::vector<json> properties;
    void pass(const char* id, const std::string& detail) {
        properties.push_back(json{{"id", id}, {"verdict", "pass"}, {"detail", detail}});
    }
    void fail(const char* id, const std::string& detail) {
        properties.push_back(json{{"id", id}, {"verdict", "fail"}, {"detail", detail}});
    }
};

std::string spelling(CXCursor c) {
    CXString s = clang_getCursorSpelling(c);
    std::string out = clang_getCString(s);
    clang_disposeString(s);
    return out;
}

std::string type_spelling(CXType t) {
    CXString s = clang_getTypeSpelling(t);
    std::string out = clang_getCString(s);
    clang_disposeString(s);
    return out;
}

struct Collector {
    std::vector<std::string> names;
    std::vector<std::string> callees;
    bool subscript = false;
    bool nullopt = false;
};

CXChildVisitResult collect(CXCursor c, CXCursor, CXClientData d) {
    auto* col = static_cast<Collector*>(d);
    CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_FunctionDecl) col->names.push_back(spelling(c));
    if (k == CXCursor_ArraySubscriptExpr) col->subscript = true;
    if (spelling(c) == "nullopt") col->nullopt = true;
    if (k == CXCursor_CallExpr) {
        CXCursor callee = clang_getCursorReferenced(c);
        if (!clang_Cursor_isNull(callee)) col->callees.push_back(spelling(callee));
    }
    return CXChildVisit_Recurse;
}


struct ClassInfo {
    bool found = false;
    bool allFieldsPublic = true;
    bool anyFieldPrivate = false;
    bool hasField = false;
    std::vector<std::string> publicMethods;
};

ClassInfo find_class(CXCursor tu, const std::string& name) {
    ClassInfo info;
    struct Ctx {
        const std::string* name;
        ClassInfo* info;
    } ctx{&name, &info};
    clang_visitChildren(
        tu,
        [](CXCursor c, CXCursor, CXClientData d) {
            auto* ctx = static_cast<Ctx*>(d);
            CXCursorKind k = clang_getCursorKind(c);
            if (k != CXCursor_ClassDecl && k != CXCursor_StructDecl) return CXChildVisit_Continue;
            if (spelling(c) != *ctx->name) return CXChildVisit_Continue;
            ctx->info->found = true;
            struct Inner {
                ClassInfo* info;
            } inner{ctx->info};
            clang_visitChildren(
                c,
                [](CXCursor m, CXCursor, CXClientData dd) {
                    auto* inner = static_cast<Inner*>(dd);
                    CXCursorKind mk = clang_getCursorKind(m);
                    if (mk == CXCursor_FieldDecl) {
                        inner->info->hasField = true;
                        if (clang_getCXXAccessSpecifier(m) != CX_CXXPublic) {
                            inner->info->anyFieldPrivate = true;
                            inner->info->allFieldsPublic = false;
                        }
                    } else if (mk == CXCursor_CXXMethod) {
                        if (clang_getCXXAccessSpecifier(m) == CX_CXXPublic)
                            inner->info->publicMethods.push_back(spelling(m));
                    }
                    return CXChildVisit_Continue;
                },
                &inner);
            return CXChildVisit_Break;
        },
        &ctx);
    return info;
}

void judge(CXCursor tu, Report& report) {
    auto body_of = [&](const std::string& name, Collector& out) {
        struct Ctx {
            const std::string* name;
            Collector* out;
        } ctx{&name, &out};
        clang_visitChildren(
            tu,
            [](CXCursor c, CXCursor, CXClientData d) {
                auto* ctx = static_cast<Ctx*>(d);
                if (clang_getCursorKind(c) != CXCursor_FunctionDecl ||
                    spelling(c) != *ctx->name || !clang_isCursorDefinition(c))
                    return CXChildVisit_Recurse;
                clang_visitChildren(c, collect, ctx->out);
                return CXChildVisit_Break;
            },
            &ctx);
    };

    // crash-unguarded
    {
        bool found = false, external = false;
        struct Ctx {
            bool* found;
            bool* external;
        } ctx{&found, &external};
        clang_visitChildren(
            tu,
            [](CXCursor c, CXCursor, CXClientData d) {
                auto* ctx = static_cast<Ctx*>(d);
                if (clang_getCursorKind(c) != CXCursor_FunctionDecl || spelling(c) != "crash")
                    return CXChildVisit_Recurse;
                *ctx->found = true;
                *ctx->external = clang_getCursorLinkage(c) == CXLinkage_External;
                return CXChildVisit_Break;
            },
            &ctx);
        Collector col;
        body_of("crash", col);
        if (found && external && col.subscript)
            report.pass("crash-unguarded", "crash is external and its body indexes an array");
        else
            report.fail("crash-unguarded", "crash is missing, not external, or does not index");
    }

    // guard-refusal
    {
        Collector col;
        body_of("bounded", col);
        bool returns_optional = false, external = false;
        struct Ctx {
            bool* opt;
            bool* ext;
        } ctx{&returns_optional, &external};
        clang_visitChildren(
            tu,
            [](CXCursor c, CXCursor, CXClientData d) {
                auto* ctx = static_cast<Ctx*>(d);
                if (clang_getCursorKind(c) != CXCursor_FunctionDecl || spelling(c) != "bounded")
                    return CXChildVisit_Recurse;
                *ctx->ext = clang_getCursorLinkage(c) == CXLinkage_External;
                *ctx->opt = type_spelling(clang_getResultType(clang_getCursorType(c)))
                                .find("optional") != std::string::npos;
                return CXChildVisit_Break;
            },
            &ctx);
        if (external && returns_optional && col.nullopt)
            report.pass("guard-refusal", "bounded validates and returns an empty optional");
        else
            report.fail("guard-refusal", "bounded is missing, not external, or never refuses");
    }

    // private-symbol
    {
        bool found = false, internal = false;
        struct Ctx {
            bool* found;
            bool* internal;
        } ctx{&found, &internal};
        clang_visitChildren(
            tu,
            [](CXCursor c, CXCursor, CXClientData d) {
                auto* ctx = static_cast<Ctx*>(d);
                if (clang_getCursorKind(c) != CXCursor_FunctionDecl || spelling(c) != "hidden")
                    return CXChildVisit_Recurse;
                *ctx->found = true;
                *ctx->internal = clang_getCursorLinkage(c) == CXLinkage_Internal;
                return CXChildVisit_Break;
            },
            &ctx);
        if (found && internal)
            report.pass("private-symbol", "hidden has internal linkage");
        else
            report.fail("private-symbol", "hidden is missing or externally visible");
    }

    // unrepresentable-symbol
    {
        bool found = false, external = false, variadic = false;
        struct Ctx {
            bool* found;
            bool* external;
            bool* variadic;
        } ctx{&found, &external, &variadic};
        clang_visitChildren(
            tu,
            [](CXCursor c, CXCursor, CXClientData d) {
                auto* ctx = static_cast<Ctx*>(d);
                if (clang_getCursorKind(c) != CXCursor_FunctionDecl || spelling(c) != "variadic")
                    return CXChildVisit_Recurse;
                *ctx->found = true;
                *ctx->external = clang_getCursorLinkage(c) == CXLinkage_External;
                *ctx->variadic = clang_isFunctionTypeVariadic(clang_getCursorType(c)) != 0;
                return CXChildVisit_Break;
            },
            &ctx);
        if (found && external && variadic)
            report.pass("unrepresentable-symbol", "variadic is external and variadic");
        else
            report.fail("unrepresentable-symbol", "variadic is missing or not a variadic signature");
    }

    // unconstructible-receiver
    {
        ClassInfo ledger = find_class(tu, "Ledger");
        bool reads = false;
        for (const auto& m : ledger.publicMethods)
            if (m == "read") reads = true;
        if (ledger.found && ledger.anyFieldPrivate && reads)
            report.pass("unconstructible-receiver", "Ledger keeps a private field and a public read()");
        else
            report.fail("unconstructible-receiver", "Ledger is missing, fully public, or has no public read()");
    }

    // method-receiver
    {
        ClassInfo counter = find_class(tu, "Counter");
        bool bumps = false;
        for (const auto& m : counter.publicMethods)
            if (m == "bump") bumps = true;
        if (counter.found && counter.hasField && counter.allFieldsPublic && bumps)
            report.pass("method-receiver", "Counter is fully public and has a public bump()");
        else
            report.fail("method-receiver", "Counter is missing, partly private, or has no public bump()");
    }

    // coverage-digest-live
    {
        Collector col;
        body_of("branch", col);
        std::vector<std::string> distinct;
        for (const auto& c : col.callees) {
            bool seen = false;
            for (const auto& d : distinct)
                if (d == c) seen = true;
            if (!seen) distinct.push_back(c);
        }
        if (distinct.size() >= 2)
            report.pass("coverage-digest-live",
                        "branch calls " + std::to_string(distinct.size()) +
                            " distinct functions, so two inputs execute different sets");
        else
            report.fail("coverage-digest-live", "branch does not call two distinct functions");
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "src/probe.cpp";
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return 1;
    }
    fs::path file(path);
    std::string root = file.parent_path().parent_path().string();
    if (root.empty()) root = ".";

    CXIndex index = clang_createIndex(0, 0);
    std::string inc = "-I" + root;
    std::vector<const char*> args = {"-std=c++20", "-xc++", "-Wno-everything", inc.c_str()};
    CXTranslationUnit tu = nullptr;
    CXErrorCode res = clang_parseTranslationUnit2(
        index, path.c_str(), args.data(), static_cast<int>(args.size()), nullptr, 0,
        CXTranslationUnit_None, &tu);
    Report report;
    if (res != CXError_Success || !tu) {
        // The fixture is not valid C++: every property is unjudged from
        // source, and the protocol judge will disagree, which is the point
        // of two verdicts.
        const char* ids[] = {"crash-unguarded", "guard-refusal", "private-symbol",
                             "unrepresentable-symbol", "unconstructible-receiver",
                             "method-receiver", "coverage-digest-live"};
        for (const char* id : ids)
            report.fail(id, path + " does not parse");
    } else {
        judge(clang_getTranslationUnitCursor(tu), report);
        clang_disposeTranslationUnit(tu);
    }
    clang_disposeIndex(index);
    json out{{"version", 1}, {"properties", json::array()}};
    for (const auto& p : report.properties) out["properties"].push_back(p);
    std::printf("%s\n", afzh::afz_dump(out).c_str());
    return 0;
}

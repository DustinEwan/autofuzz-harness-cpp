#include "discover.h"

#include <clang-c/Index.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>
#include <unordered_set>
#include <unordered_map>

#include "sha256.h"

#include <cstdint>

#include <map>


namespace afzh {
namespace fs = std::filesystem;

std::string target_id(const std::string& relFile, const std::string& usr,
                      const std::string& signatureText) {
    Sha256 h;
    h.update(relFile);
    h.update("\0", 1);
    h.update(usr);
    h.update("\0", 1);
    h.update(signatureText);
    std::uint8_t d[32];
    h.finish(d);
    static const char* digits = "0123456789abcdef";
    char hex[13];
    for (int i = 0; i < 6; ++i) {
        hex[i * 2] = digits[d[i] >> 4];
        hex[i * 2 + 1] = digits[d[i] & 15];
    }
    hex[12] = 0;
    return std::string("afz:t:") + hex;
}

namespace {

std::string cstr(CXString s) {
    const char* p = clang_getCString(s);
    std::string out = p ? p : "";
    clang_disposeString(s);
    return out;
}

std::string strip_qual(std::string s) {
    while (s.starts_with("const ")) s.erase(0, 6);
    while (s.starts_with("volatile ")) s.erase(0, 9);
    return s;
}

bool has_ext(const std::string& p, std::initializer_list<const char*> exts) {
    for (const char* e : exts) {
        std::size_t n = std::strlen(e);
        if (p.size() >= n && p.compare(p.size() - n, n, e) == 0) return true;
    }
    return false;
}

bool header_ext(const std::string& p) { return has_ext(p, {".h", ".hpp", ".hh", ".hxx", ".inl"}); }
bool source_ext(const std::string& p) { return has_ext(p, {".cpp", ".cc", ".cxx", ".C", ".c++"}); }

bool walk_skip(const fs::path& name) {
    std::string n = name.string();
    return n == ".autofuzz" || n == ".git" || n == "node_modules" || n.starts_with("build");
}

bool excluded_by(const std::vector<std::string>& patterns, std::string_view rel) {
    for (const auto& p : patterns)
        if (glob_match(p, rel)) return true;
    return false;
}

std::string presumed_file(CXSourceLocation loc) {
    CXFile file = nullptr;
    clang_getSpellingLocation(loc, &file, nullptr, nullptr, nullptr);
    if (!file) return "";
    return cstr(clang_getFileName(file));
}

unsigned presumed_line(CXSourceLocation loc) {
    unsigned line = 0;
    clang_getSpellingLocation(loc, nullptr, &line, nullptr, nullptr);
    return line;
}

// The line numbers of every `throw` inside a cursor's extent: C++'s ordinary
// refusal channel, reported as `guardSites`.
struct ThrowFinder {
    std::vector<unsigned> lines;
};

CXChildVisitResult find_throws(CXCursor cur, CXCursor, CXClientData d) {
    auto* t = static_cast<ThrowFinder*>(d);
    if (clang_getCursorKind(cur) == CXCursor_CXXThrowExpr && t->lines.size() < 4096)
        t->lines.push_back(presumed_line(clang_getCursorLocation(cur)));
    return CXChildVisit_Recurse;
}

struct Cand {
    CXCursor cur = clang_getNullCursor();
    std::string usr;
    std::string qualified;
    std::string bareName;
    std::string sigText;
    std::string relFile;
    unsigned line = 0;
    bool isDefinition = false;
    bool isMethod = false;
    bool isStaticMethod = false;
    bool nonPublicMember = false;
    bool internalLinkage = false;
    bool variadic = false;
    bool inClassTemplate = false;
    std::vector<unsigned> throwLines;
};

struct ParsedFile {
    std::string rel; // root-relative, '/'-separated
    std::string abs;
    bool isHeader = false;
    bool parseFailed = false;
    std::string firstError;
    CXTranslationUnit tu = nullptr;
    std::vector<Cand> cands;
    std::vector<UniverseEntry> universe;
};

void collect_throws(Cand& c) {
    ThrowFinder t;
    clang_visitChildren(c.cur, find_throws, &t);
    std::sort(t.lines.begin(), t.lines.end());
    t.lines.erase(std::unique(t.lines.begin(), t.lines.end()), t.lines.end());
    if (t.lines.size() > 64) t.lines.resize(64);
    c.throwLines = std::move(t.lines);
}

struct Collector {
    ParsedFile* file;
};

void add_candidate(Collector& col, CXCursor cur, bool isMethod, bool inClassTemplate) {
    // Reject without reporting: operators, pure virtual, deleted.
    if (isMethod) {
        std::string sp = cursor_spelling(cur);
        if (sp.starts_with("operator") || clang_CXXMethod_isPureVirtual(cur) ||
            clang_CXXMethod_isDeleted(cur))
            return;
    }
    Cand c;
    c.cur = cur;
    c.isMethod = isMethod;
    c.inClassTemplate = inClassTemplate;
    c.relFile = col.file->rel;
    c.usr = cstr(clang_getCursorUSR(cur));
    if (c.usr.empty()) return;
    c.qualified = fully_qualified_name(cur);
    c.bareName = cursor_spelling(cur);
    CXLinkageKind link = clang_getCursorLinkage(cur);
    c.internalLinkage = link == CXLinkage_Internal || link == CXLinkage_UniqueExternal;
    c.isDefinition = clang_isCursorDefinition(cur) != 0;
    if (isMethod) {
        c.isStaticMethod = clang_CXXMethod_isStatic(cur) != 0;
        c.nonPublicMember = clang_getCXXAccessSpecifier(cur) != CX_CXXPublic;
    }
    if (presumed_file(clang_getCursorLocation(cur)) != col.file->abs) return; // from an include
    c.line = presumed_line(clang_getCursorLocation(cur));
    if (c.line == 0) c.line = presumed_line(clang_getRangeStart(clang_getCursorExtent(cur)));
    CXType t = clang_getCursorType(cur);
    c.variadic = clang_isFunctionTypeVariadic(t) != 0;
    // signatureText: return spelling + parameter spellings joined by ','.
    c.sigText = type_spelling(clang_getResultType(t));
    for (unsigned i = 0; i < clang_getNumArgTypes(t); ++i) {
        c.sigText += ",";
        c.sigText += type_spelling(clang_getArgType(t, i));
    }
    collect_throws(c);
    col.file->cands.push_back(std::move(c));
}

CXChildVisitResult visit_method(CXCursor cur, CXCursor, CXClientData d) {
    auto* pack = static_cast<std::pair<Collector*, bool>*>(d);
    if (clang_getCursorKind(cur) == CXCursor_CXXMethod)
        add_candidate(*pack->first, cur, true, pack->second);
    return CXChildVisit_Continue;
}

CXChildVisitResult visit_top(CXCursor cur, CXCursor, CXClientData d) {
    auto* col = static_cast<Collector*>(d);
    switch (clang_getCursorKind(cur)) {
    case CXCursor_Namespace:
        return CXChildVisit_Recurse;
    case CXCursor_FunctionDecl: {
        CXCursorKind pk = clang_getCursorKind(clang_getCursorSemanticParent(cur));
        if (pk == CXCursor_TranslationUnit || pk == CXCursor_Namespace)
            add_candidate(*col, cur, false, false);
        return CXChildVisit_Continue;
    }
    case CXCursor_ClassDecl: case CXCursor_StructDecl: {
        bool inTemplate =
            clang_getCursorKind(clang_getCursorSemanticParent(cur)) == CXCursor_ClassTemplate;
        std::pair<Collector*, bool> pack{col, inTemplate};
        clang_visitChildren(cur, visit_method, &pack);
        return CXChildVisit_Continue;
    }
    default:
        return CXChildVisit_Continue;
    }
}

void collect_universe(ParsedFile& f) {
    struct U {
        ParsedFile* f;
    } u{&f};
    clang_visitChildren(
        clang_getTranslationUnitCursor(f.tu),
        [](CXCursor cur, CXCursor, CXClientData d) {
            auto* u = static_cast<U*>(d);
            CXCursorKind k = clang_getCursorKind(cur);
            if (k != CXCursor_FunctionDecl && k != CXCursor_CXXMethod) return CXChildVisit_Recurse;
            if (!clang_isCursorDefinition(cur)) return CXChildVisit_Recurse;
            if (k == CXCursor_CXXMethod && clang_CXXMethod_isPureVirtual(cur)) return CXChildVisit_Continue;
            if (presumed_file(clang_getCursorLocation(cur)) != u->f->abs) return CXChildVisit_Recurse;
            CXSourceRange ext = clang_getCursorExtent(cur);
            UniverseEntry e;
            e.key = fully_qualified_name(cur);
            e.file = u->f->rel;
            e.start = presumed_line(clang_getRangeStart(ext));
            e.end = presumed_line(clang_getRangeEnd(ext));
            if (e.start && e.end >= e.start) u->f->universe.push_back(std::move(e));
            return CXChildVisit_Continue;
        },
        &u);
}

std::vector<std::string> param_names(CXCursor fn) {
    std::vector<std::string> out;
    clang_visitChildren(
        fn,
        [](CXCursor cur, CXCursor, CXClientData d) {
            auto* out = static_cast<std::vector<std::string>*>(d);
            if (clang_getCursorKind(cur) == CXCursor_ParmDecl) out->push_back(cursor_spelling(cur));
            return CXChildVisit_Continue;
        },
        &out);
    return out;
}

bool is_length_name(const std::string& n) {
    static const char* names[] = {"n", "len", "length", "count", "size", "numel", "nelem", "nbytes", "bytes"};
    for (const char* c : names)
        if (n == c) return true;
    return false;
}

bool integral_kind(CXTypeKind k) {
    switch (k) {
    case CXType_Short: case CXType_UShort: case CXType_Int: case CXType_UInt:
    case CXType_Long: case CXType_ULong: case CXType_LongLong: case CXType_ULongLong:
    case CXType_Char_S: case CXType_Char_U: case CXType_SChar: case CXType_UChar:
        return true;
    default:
        return false;
    }
}

// The buffer row's first half: a pointer to a one-byte type or to void.
bool byte_or_void_pointer(CXType t) {
    CXType p = clang_getPointeeType(t);
    if (p.kind == CXType_Invalid) return false;
    CXType pc = clang_getCanonicalType(p);
    switch (pc.kind) {
    case CXType_Char_S: case CXType_Char_U: case CXType_SChar: case CXType_UChar:
    case CXType_Void:
        return true;
    case CXType_Record:
        return strip_qual(type_spelling(pc)) == "std::byte";
    default:
        return false;
    }
}

struct SkipLite {
    std::string id, name, reason, detail, blockerType, blockerPath, blockerModule;
    bool materialization = false;
};

struct Classifier {
    Inventory* inv = nullptr;
    const Options* opt = nullptr;
    std::unordered_set<std::string> seenUsr;
    std::unordered_set<std::string> headerUsrs;
    std::vector<SkipLite> skips;
    // usr -> (definition file, line): a target declared in a header reports
    // the origin of its definition, so fault frames can name the same file.
    std::unordered_map<std::string, std::pair<std::string, unsigned>> defOrigin;
    std::vector<std::string> targetOrder; // (file, line) sort keys handled later
};

void add_skip(Classifier& cl, const Cand& c, const char* reason, const std::string& detail,
              const std::string& blockerType = "", const std::string& blockerPath = "",
              bool materialization = false) {
    SkipLite s;
    s.id = target_id(c.relFile, c.usr, c.sigText);
    s.name = c.qualified;
    s.reason = reason;
    s.detail = detail;
    s.blockerType = blockerType;
    s.blockerPath = blockerPath;
    s.blockerModule = c.relFile;
    s.materialization = materialization;
    cl.skips.push_back(std::move(s));
}

// Build one parameter: lower its type, fold the buffer row, decide the pass.
// Returns false with a skip reason on refusal or opacity.
struct ParamOutcome {
    bool ok = false;
    bool opaque = false;
    bool refuse = false;
    std::string detail;
    std::string blocker;
    Param param;
};

ParamOutcome build_param(CXType type, const std::string& name, bool buffer,
                         const std::string& lengthType, LowerCtx& ctx) {
    ParamOutcome po;
    po.param.name = name;
    if (buffer) {
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::BufferPtr;
        b->localType = "std::vector<std::uint8_t>";
        b->spelled = type_spelling(type);
        b->pointee = strip_qual(type_spelling(clang_getPointeeType(type)));
        b->elemSize = 1;
        po.param.build = b;
        po.param.node = json{{"kind", "buffer"}, {"length", {{"min", 0}, {"max", 4096}}}};
        po.param.pass = Pass::Buffer;
        po.param.lengthType = lengthType;
        po.ok = true;
        return po;
    }
    CXType canon = clang_getCanonicalType(type);
    LowerResult lr = lower_type(type, ctx);
    if (lr.status == LowerResult::Status::Refuse) {
        po.refuse = true;
        po.detail = lr.detail;
        return po;
    }
    if (lr.status == LowerResult::Status::Opaque) {
        po.opaque = true;
        po.detail = lr.detail;
        po.blocker = lr.blockerTy;
        return po;
    }
    po.param.node = lr.node;
    po.param.build = lr.build;
    if (lr.build->kind == Build::Kind::CStr) {
        po.param.pass = lr.build->mutableCharPtr ? Pass::CStrMut : Pass::CStrConst;
        po.param.mutableOut = lr.build->mutableCharPtr;
    } else if (canon.kind == CXType_LValueReference) {
        po.param.pass = Pass::Ref;
        po.param.mutableOut = !clang_isConstQualifiedType(clang_getPointeeType(canon));
    } else if (canon.kind == CXType_Pointer) {
        if (clang_getCanonicalType(clang_getPointeeType(canon)).kind == CXType_FunctionProto)
            po.param.pass = Pass::FnPtr;
        else
            po.param.pass = Pass::Ptr;
        po.param.mutableOut = !clang_isConstQualifiedType(clang_getPointeeType(canon));
    } else {
        po.param.pass = Pass::Value;
    }
    po.ok = true;
    return po;
}

void classify(ParsedFile& f, Classifier& cl) {
    for (const Cand& c : f.cands) {
        if (c.inClassTemplate) continue; // rejected, not reported
        if (!cl.seenUsr.insert(c.usr).second) continue; // the declaration pass already took it

        // 1. not-public.
        if (c.internalLinkage) {
            add_skip(cl, c, kSkipNotPublic, "the symbol has internal linkage (static or an anonymous namespace)");
            continue;
        }
        if (c.nonPublicMember) {
            add_skip(cl, c, kSkipNotPublic, "the member is not public");
            continue;
        }
        if (c.isDefinition && !f.isHeader && !cl.headerUsrs.count(c.usr)) {
            add_skip(cl, c, kSkipNotPublic, "a definition with no declaration in any scanned header");
            continue;
        }

        // Parameters, with the buffer row folded before lowering.
        CXType fnType = clang_getCursorType(c.cur);
        unsigned argc = clang_getNumArgTypes(fnType);
        std::vector<std::string> names = param_names(c.cur);
        names.resize(argc);
        for (unsigned i = 0; i < argc; ++i)
            if (names[i].empty()) names[i] = "arg" + std::to_string(i);
        std::vector<bool> isBuffer(argc, false), dropped(argc, false);
        std::vector<std::string> lengthType(argc);
        for (unsigned i = 0; i + 1 < argc; ++i) {
            CXType t = clang_getArgType(fnType, i);
            CXType nx = clang_getCanonicalType(clang_getArgType(fnType, i + 1));
            if (byte_or_void_pointer(t) && integral_kind(nx.kind) && is_length_name(names[i + 1])) {
                isBuffer[i] = true;
                dropped[i + 1] = true;
                lengthType[i] = type_spelling(nx);
            }
        }

        // 2. unsupported-signature.
        if (c.variadic) {
            add_skip(cl, c, kSkipUnsupportedSignature, "the function is variadic");
            continue;
        }

        std::vector<Param> params;
        std::string refuseDetail, opaqueDetail, opaqueBlocker, opaquePath;
        bool refused = false, opaque = false;
        if (c.isMethod && !c.isStaticMethod) {
            CXCursor parent = clang_getCursorSemanticParent(c.cur);
            CXType classType = clang_getCursorType(parent);
            ParamOutcome ro = build_param(classType, "receiver", false, "", cl.inv->lower);
            if (ro.refuse) {
                refused = true;
                refuseDetail = "receiver: " + ro.detail;
            } else if (ro.opaque) {
                opaque = true;
                opaqueDetail = ro.detail;
                opaqueBlocker = ro.blocker;
                opaquePath = "receiver";
            } else {
                params.push_back(std::move(ro.param));
            }
        }
        for (unsigned i = 0; i < argc && !refused && !opaque; ++i) {
            if (dropped[i]) continue;
            ParamOutcome po = build_param(clang_getArgType(fnType, i), names[i], isBuffer[i],
                                          lengthType[i], cl.inv->lower);
            if (po.refuse) {
                refused = true;
                refuseDetail = "parameter " + names[i] + ": " + po.detail;
            } else if (po.opaque) {
                opaque = true;
                opaqueDetail = po.detail;
                opaqueBlocker = po.blocker;
                opaquePath = names[i];
            } else {
                params.push_back(std::move(po.param));
            }
        }
        if (refused) {
            add_skip(cl, c, kSkipUnsupportedSignature, refuseDetail);
            continue;
        }
        // 3. unconstructible-parameter.
        if (opaque) {
            add_skip(cl, c, kSkipUnconstructible, opaqueDetail, opaqueBlocker, opaquePath, true);
            continue;
        }

        // 4. a target.
        Tgt t;
        t.id = target_id(c.relFile, c.usr, c.sigText);
        t.name = c.qualified;
        t.callPath = c.isMethod && !c.isStaticMethod ? c.bareName : c.qualified;
        t.file = c.relFile;
        t.line = c.line;
        if (!c.isDefinition) {
            auto def = cl.defOrigin.find(c.usr);
            if (def != cl.defOrigin.end()) {
                t.file = def->second.first;
                t.line = def->second.second;
            }
        }
        t.isMethod = c.isMethod && !c.isStaticMethod;
        t.params = std::move(params);
        // module: the namespace path of the qualified name (drop the symbol,
        // and the class for methods).
        {
            std::string q = c.qualified;
            auto cut = q.rfind("::");
            if (cut != std::string::npos) q = q.substr(0, cut);
            if (t.isMethod) {
                auto cut2 = q.rfind("::");
                // Keep the class out of the module only when the class is the
                // last part; namespaces stay.
                (void)cut2;
            }
            t.module = q;
            // For methods the module is the namespaces, not the class.
            if (t.isMethod) {
                // qualified = ns...::Class::method; module = ns...
                std::string m = c.qualified;
                auto c1 = m.rfind("::");
                if (c1 != std::string::npos) m = m.substr(0, c1);
                auto c2 = m.rfind("::");
                // The class is the last remaining part only if the parent is a
                // record; strip it when the direct parent is a record.
                CXCursor parent = clang_getCursorSemanticParent(c.cur);
                if (clang_getCursorKind(parent) == CXCursor_ClassDecl ||
                    clang_getCursorKind(parent) == CXCursor_StructDecl)
                    m = (c2 == std::string::npos) ? std::string() : m.substr(0, c2);
                t.module = m;
            }
        }
        CXType ret = clang_getResultType(fnType);
        std::string retName = strip_qual(type_spelling(clang_getCanonicalType(ret)));
        if (retName == "void") {
            t.returnsVoid = true;
        } else {
            LowerResult rr = lower_type(ret, cl.inv->lower);
            if (rr.status == LowerResult::Status::Ok) {
                t.returnsNode = rr.node;
                t.returnsOptional = rr.build->kind == Build::Kind::Optional;
                // The thunk unwraps the optional itself; the encoder is
                // built for the element type.
                t.returnBuild = t.returnsOptional ? rr.build->elem : rr.build;
                if (clang_getCanonicalType(ret).kind == CXType_Pointer &&
                    rr.build->kind != Build::Kind::CStr) {
                    // A pointer return would need a dereference the harness
                    // cannot size; report it as unencodable.
                    t.returnsNode = json{{"kind", kNodeOpaque}, {"ty", type_spelling(clang_getCanonicalType(ret))}};
                    t.returnBuild = nullptr;
                }
            } else {
                t.returnsNode = json{{"kind", kNodeOpaque}, {"ty", rr.opaqueTy}};
            }
        }

        bool callbacks = false;
        for (const Param& p : t.params)
            if (p.build && p.build->kind == Build::Kind::Fn) callbacks = true;

        json effects{{"throws", !c.throwLines.empty()}, {"callbacks", callbacks}};
        json paramNodes = json::array();
        for (const Param& p : t.params)
            paramNodes.push_back(json{{"name", p.name}, {"type", p.node}, {"required", true}});
        t.descriptor = json{{"id", t.id},
                            {"targetClass", "design-time"},
                            {"origin", {{"file", t.file}, {"line", t.line}, {"module", t.module}}},
                            {"name", t.name}, // collision suffix applied after the walk
                            {"parameters", paramNodes},
                            {"returns", t.returnsNode.is_null() ? json(nullptr) : t.returnsNode},
                            {"effects", effects},
                            {"materialization", kMatConstructible},
                            {"guardSites", c.throwLines}};
        cl.inv->targets.push_back(std::move(t));
    }
}

} // namespace

bool discover(const std::string& root, const std::vector<std::string>& include,
              const std::vector<std::string>& exclude, const std::string& configPath,
              Inventory& out, std::string& error) {
    Options opt = load_options(configPath);
    out.root = root;
    out.scratch = (fs::path(root) / load_output_dir(configPath) / "cpp").lexically_normal().string();
    if (!opt.error.empty() || !opt.unknown.empty()) {
        std::string detail = !opt.error.empty() ? opt.error : [&] {
            std::string s;
            for (std::size_t i = 0; i < opt.unknown.size(); ++i) {
                if (i) s += ", ";
                s += opt.unknown[i];
            }
            return "harness.options has unknown key(s): " + s;
        }();
        json skip{{"targetId", target_id(configPath, "", "")},
                  {"name", "harness.options"},
                  {"reason", kSkipDiscoveryError},
                  {"detail", detail}};
        out.discovery = json{{"targets", json::array()}, {"skipped", json::array({skip})}};
        return true;
    }
    out.lower.index = clang_createIndex(0, 0);

    // Walk.
    std::vector<std::string> candidates;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        std::error_code isReg;
        if (!fs::is_regular_file(p, isReg)) {
            if (fs::is_directory(p, isReg) && walk_skip(p.filename())) it.disable_recursion_pending();
            continue;
        }
        std::string rel = fs::relative(p, root, ec).generic_string();
        if (ec) continue;
        {
            fs::path parts(rel);
            auto last = parts.end();
            if (last != parts.begin()) --last;
            bool skippedDir = false;
            for (auto s = parts.begin(); s != last; ++s)
                if (walk_skip(*s)) skippedDir = true;
            if (skippedDir) continue;
        }
        if (!header_ext(rel) && !source_ext(rel)) continue;
        if (excluded_by(exclude, rel)) continue;
        if (!include.empty()) {
            bool any = false;
            for (const auto& pat : include)
                if (glob_match(pat, rel)) any = true;
            if (!any) continue;
        }
        candidates.push_back(rel);
    }
    std::sort(candidates.begin(), candidates.end());

    // Parse every candidate.
    std::vector<ParsedFile> files;
    std::vector<std::string> argStrings = {"-std=" + opt.std, "-xc++", "-ferror-limit=0", "-Wno-everything"};
    for (const auto& f : opt.cxxFlags) argStrings.push_back(f);
    argStrings.push_back("-I" + fs::absolute(root).string());
    for (const std::string& rel : candidates) {
        ParsedFile pf;
        pf.rel = rel;
        pf.abs = (fs::path(root) / rel).lexically_normal().string();
        pf.isHeader = header_ext(rel);
        std::vector<const char*> args;
        std::string incDir = "-I" + fs::path(pf.abs).parent_path().string();
        for (const auto& s : argStrings) args.push_back(s.c_str());
        args.push_back(incDir.c_str());
        CXTranslationUnit tu = nullptr;
        CXErrorCode err = clang_parseTranslationUnit2(out.lower.index, pf.abs.c_str(), args.data(),
                                                      static_cast<int>(args.size()), nullptr, 0,
                                                      CXTranslationUnit_None, &tu);
        if (err != CXError_Success) tu = nullptr;
        if (!tu) {
            pf.parseFailed = true;
            pf.firstError = "clang could not parse the file";
            files.push_back(std::move(pf));
            continue;
        }
        pf.tu = tu;
        // Any error-severity diagnostic fails the file.
        unsigned n = clang_getNumDiagnostics(tu);
        for (unsigned i = 0; i < n; ++i) {
            CXDiagnostic d = clang_getDiagnostic(tu, i);
            if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
                pf.parseFailed = true;
                if (pf.firstError.empty())
                    pf.firstError = cstr(clang_formatDiagnostic(
                        d, static_cast<unsigned>(CXDiagnostic_DisplaySourceLocation) |
                               static_cast<unsigned>(CXDiagnostic_DisplayColumn)));
            }
            clang_disposeDiagnostic(d);
        }
        if (pf.parseFailed) {
            clang_disposeTranslationUnit(tu);
            pf.tu = nullptr;
            files.push_back(std::move(pf));
            continue;
        }
        {
            Collector c2{&pf};
            clang_visitChildren(clang_getTranslationUnitCursor(pf.tu), visit_top, &c2);
            collect_universe(pf);
        }
        files.push_back(std::move(pf));
    }

    // Header USRs: a definition in a source file is public only when its
    // declaration appears in a scanned header.
    Classifier cl;
    cl.inv = &out;
    cl.opt = &opt;
    for (const ParsedFile& f : files) {
        if (f.parseFailed) continue;
        for (const Cand& c : f.cands) {
            if (f.isHeader) cl.headerUsrs.insert(c.usr);
            else if (c.isDefinition) cl.defOrigin[c.usr] = {f.rel, c.line};
        }
    }

    // Classify: headers first so a declaration's origin wins over the
    // definition's duplicate; `origin` below then re-points at the definition.
    for (ParsedFile& f : files)
        if (f.isHeader && !f.parseFailed) classify(f, cl);
    for (ParsedFile& f : files)
        if (!f.isHeader && !f.parseFailed) classify(f, cl);

    // Parse failures: exactly one skip entry naming the file.
    for (const ParsedFile& f : files) {
        if (!f.parseFailed) continue;
        SkipLite s;
        s.id = target_id(f.rel, "", "");
        s.name = f.rel;
        s.reason = kSkipDiscoveryError;
        s.detail = f.firstError;
        cl.skips.push_back(std::move(s));
    }

    // Overload collisions get the parameter list appended.
    {
        std::map<std::string, int> nameCount;
        for (const Tgt& t : out.targets) nameCount[t.name]++;
        for (Tgt& t : out.targets) {
            if (nameCount[t.name] <= 1) continue;
            auto paren = t.descriptor["name"].get<std::string>().find('(');
            (void)paren;
            // Signature spellings: rebuild from the descriptor parameters.
            std::string types;
            for (const json& p : t.descriptor["parameters"]) {
                if (!types.empty()) types += ",";
                const json& n = p["type"];
                types += n.contains("scalar") ? n["scalar"].get<std::string>()
                                              : n["kind"].get<std::string>();
            }
            t.name += "(" + types + ")";
            t.descriptor["name"] = t.name;
        }
    }

    // unlocksTargets: the other skipped symbols sharing one blocker.
    std::map<std::string, int> blockerCount;
    for (const SkipLite& s : cl.skips)
        if (!s.blockerType.empty()) blockerCount[s.blockerType]++;

    json skipped = json::array();
    for (const SkipLite& s : cl.skips) {
        json e{{"targetId", s.id}, {"name", s.name}, {"reason", s.reason}};
        if (!s.detail.empty()) e["detail"] = s.detail;
        if (!s.blockerType.empty())
            e["blockedBy"] = json{{"type", s.blockerType}, {"path", s.blockerPath}, {"module", s.blockerModule}};
        if (s.materialization) e["materialization"] = kMatOpaque;
        auto it = blockerCount.find(s.blockerType);
        if (!s.blockerType.empty() && it != blockerCount.end() && it->second > 1)
            e["unlocksTargets"] = it->second - 1;
        skipped.push_back(std::move(e));
    }
    json targets = json::array();
    for (const Tgt& t : out.targets) targets.push_back(t.descriptor);
    out.discovery = json{{"targets", std::move(targets)}, {"skipped", std::move(skipped)}};

    // Universe: dedup on (key, file, start).
    {
        std::set<std::tuple<std::string, std::string, unsigned>> seen;
        for (ParsedFile& f : files)
            for (UniverseEntry& u : f.universe)
                if (seen.insert({u.key, u.file, u.start}).second) out.universe.push_back(std::move(u));
        std::sort(out.universe.begin(), out.universe.end(),
                  [](const UniverseEntry& a, const UniverseEntry& b) {
                      return std::tie(a.file, a.start) < std::tie(b.file, b.start);
                  });
    }

    // The headers afz_calls.cpp includes, once each.
    for (const ParsedFile& f : files)
        if (f.isHeader && !f.parseFailed) out.headers.push_back(f.rel);
    std::sort(out.headers.begin(), out.headers.end());
    out.headers.erase(std::unique(out.headers.begin(), out.headers.end()), out.headers.end());

    for (ParsedFile& f : files)
        if (f.tu) clang_disposeTranslationUnit(f.tu);
    clang_disposeIndex(out.lower.index);
    out.lower.index = nullptr;
    return true;
}

} // namespace afzh

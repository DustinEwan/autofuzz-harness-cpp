#include "lower.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace afzh {
namespace {

std::string cstr(CXString s) {
    const char* p = clang_getCString(s);
    std::string out = p ? p : "";
    clang_disposeString(s);
    return out;
}

bool starts(std::string_view s, std::string_view p) { return s.rfind(p, 0) == 0; }

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

// `const std::string` and `const char` keep their qualifier in the canonical
// spelling; the harness strips it because every lowered local is non-const so
// a mutated value can be read back.
std::string strip_qualifiers(std::string s) {
    while (starts(s, "const ")) s.erase(0, 6);
    while (starts(s, "volatile ")) s.erase(0, 9);
    return s;
}

bool is_byte_kind(CXTypeKind k) {
    switch (k) {
    case CXType_Char_S: case CXType_Char_U: case CXType_SChar: case CXType_UChar: return true;
    default: return false;
    }
}

bool is_byte_type(CXType t) {
    CXType c = clang_getCanonicalType(t);
    if (is_byte_kind(c.kind)) return true;
    return strip_qualifiers(type_spelling(c)) == "std::byte";
}

LowerResult opaque_of(std::string ty, std::string blocker, std::string detail) {
    LowerResult r;
    r.status = LowerResult::Status::Opaque;
    r.opaqueTy = std::move(ty);
    r.blockerTy = std::move(blocker);
    r.detail = std::move(detail);
    return r;
}

LowerResult refuse_of(std::string detail) {
    LowerResult r;
    r.status = LowerResult::Status::Refuse;
    r.detail = std::move(detail);
    return r;
}

LowerResult ok_of(json node, BuildPtr build) {
    LowerResult r;
    r.status = LowerResult::Status::Ok;
    r.node = std::move(node);
    r.build = std::move(build);
    return r;
}

json scalar_node(std::string_view scalar) {
    return json{{"kind", kNodeScalar}, {"scalar", scalar}};
}

json string_node() {
    return json{{"kind", kNodeScalar}, {"scalar", kScalarString}, {"length", {{"min", 0}, {"max", 4096}}}};
}

json length_node(std::int64_t max) {
    return json{{"min", 0}, {"max", max}};
}

// Integer bounds per the canonical kind, LP64.
void int_bounds(CXTypeKind k, Build& b) {
    b.isUnsigned = false;
    switch (k) {
    case CXType_Short: b.minBound = -32768; b.maxBound = 32767; break;
    case CXType_UShort: b.isUnsigned = true; b.uMaxBound = 65535; break;
    case CXType_Int: b.minBound = -2147483648LL; b.maxBound = 2147483647LL; break;
    case CXType_UInt: b.isUnsigned = true; b.uMaxBound = 4294967295ULL; break;
    case CXType_Long: b.minBound = -9223372036854775807LL - 1; b.maxBound = 9223372036854775807LL; break;
    case CXType_ULong: b.isUnsigned = true; b.uMaxBound = 18446744073709551615ULL; break;
    case CXType_LongLong: b.minBound = -9223372036854775807LL - 1; b.maxBound = 9223372036854775807LL; break;
    case CXType_ULongLong: b.isUnsigned = true; b.uMaxBound = 18446744073709551615ULL; break;
    case CXType_Char_S: case CXType_SChar: b.minBound = -128; b.maxBound = 127; break;
    case CXType_Char_U: case CXType_UChar: b.isUnsigned = true; b.uMaxBound = 255; break;
    default: break;
    }
}

json bigint_bounds(const Build& b) {
    if (b.isUnsigned) return json{{"int", true}, {"min", 0}, {"max", b.uMaxBound}};
    return json{{"int", true}, {"min", b.minBound}, {"max", b.maxBound}};
}

struct CtorInfo {
    bool hasAny = false;
    bool hasUsableDefault = false;
    bool defaultDeleted = false;
    bool hasFirstParam = false;
    CXType firstParam{};
};

CtorInfo ctor_info(CXCursor record) {
    CtorInfo info;
    clang_visitChildren(
        record,
        [](CXCursor cur, CXCursor, CXClientData d) {
            auto* info = static_cast<CtorInfo*>(d);
            if (clang_getCursorKind(cur) != CXCursor_Constructor) return CXChildVisit_Continue;
            info->hasAny = true;
            if (clang_CXXConstructor_isDefaultConstructor(cur)) {
                if (clang_CXXMethod_isDeleted(cur))
                    info->defaultDeleted = true;
                else
                    info->hasUsableDefault = true;
            }
            if (!info->hasFirstParam) {
                CXType t = clang_getCursorType(cur);
                if (clang_getNumArgTypes(t) >= 1) {
                    info->firstParam = clang_getArgType(t, 0);
                    info->hasFirstParam = true;
                }
            }
            return CXChildVisit_Continue;
        },
        &info);
    if (!info.hasAny) info.hasUsableDefault = true; // implicit default constructor
    return info;
}

// The deepest unbuildable type behind an unconstructible class: the class
// itself unless a constructor parameter is the blocker, in which case the
// recursion into that parameter's type decides.
std::string deepest_blocker(CXType classType, const CtorInfo& info, LowerCtx& ctx) {
    std::string self = strip_qualifiers(type_spelling(clang_getCanonicalType(classType)));
    if (!info.hasAny || info.defaultDeleted || !info.hasFirstParam) return self;
    CXType p = clang_getCanonicalType(info.firstParam);
    if (p.kind == CXType_LValueReference || p.kind == CXType_RValueReference || p.kind == CXType_Pointer)
        p = clang_getCanonicalType(clang_getPointeeType(p));
    LowerResult inner = lower_type(p, ctx);
    if (inner.status != LowerResult::Status::Ok && !inner.blockerTy.empty()) return inner.blockerTy;
    return strip_qualifiers(type_spelling(p));
}

// The N in `std::array<T, N>`, read from the canonical spelling.
bool array_size(std::string_view canon, std::size_t& out) {
    auto close = canon.rfind('>');
    if (close == std::string_view::npos) return false;
    auto comma = canon.rfind(',', close);
    if (comma == std::string_view::npos) return false;
    std::string digits(canon.substr(comma + 1, close - comma - 1));
    digits.erase(std::remove_if(digits.begin(), digits.end(),
                                [](unsigned char ch) { return std::isspace(ch) != 0; }),
                 digits.end());
    if (digits.empty() || digits.size() > 10) return false;
    for (char ch : digits)
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    out = std::stoul(digits);
    return true;
}

// The function-pointer and std::function rows share one lowering.
LowerResult lower_callable(CXType proto, const std::string& canon, const std::string& localType, LowerCtx& ctx) {
    auto b = std::make_shared<Build>();
    b->kind = Build::Kind::Fn;
    b->spelled = canon;
    b->localType = localType;
    CXType ret = clang_getResultType(proto);
    b->returnType = strip_qualifiers(type_spelling(clang_getCanonicalType(ret)));
    unsigned argc = clang_getNumArgTypes(proto);
    json retNode;
    for (unsigned i = 0; i < argc; ++i)
        b->argTypes.push_back(strip_qualifiers(type_spelling(clang_getCanonicalType(clang_getArgType(proto, i)))));
    LowerResult rr = lower_type(ret, ctx);
    if (rr.status == LowerResult::Status::Ok) retNode = rr.node;
    json node{{"kind", kNodeFunction}};
    if (!retNode.is_null()) node["type"] = retNode;
    return ok_of(node, b);
}

LowerResult lower_record(CXType canon, LowerCtx& ctx);

} // namespace

std::string cursor_spelling(CXCursor c) { return cstr(clang_getCursorSpelling(c)); }

std::string fully_qualified_name(CXCursor c) {
    std::vector<std::string> parts;
    parts.push_back(cursor_spelling(c));
    CXCursor cur = c;
    for (int guard = 0; guard < 64; ++guard) {
        CXCursor parent = clang_getCursorSemanticParent(cur);
        if (clang_equalCursors(parent, cur) || clang_Cursor_isNull(parent)) break;
        CXCursorKind k = clang_getCursorKind(parent);
        if (k == CXCursor_TranslationUnit) break;
        if (k == CXCursor_Namespace || k == CXCursor_ClassDecl || k == CXCursor_StructDecl ||
            k == CXCursor_UnionDecl || k == CXCursor_ClassTemplate)
            parts.push_back(cursor_spelling(parent));
        cur = parent;
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += "::";
        out += *it;
    }
    return out;
}

std::string type_spelling(CXType t) { return cstr(clang_getTypeSpelling(t)); }

LowerResult lower_type(CXType raw, LowerCtx& ctx) {
    CXType t = clang_getCanonicalType(raw);
    std::string canon = strip_qualifiers(type_spelling(t));
    if (canon.empty()) canon = strip_qualifiers(type_spelling(raw));

    switch (t.kind) {
    case CXType_Bool:
        return ok_of(scalar_node(kScalarBoolean),
                     std::make_shared<Build>(Build{.kind = Build::Kind::Bool, .localType = "bool", .spelled = canon}));
    case CXType_Char_S: case CXType_Char_U: case CXType_SChar: case CXType_UChar:
    case CXType_Short: case CXType_UShort: case CXType_Int: case CXType_UInt:
    case CXType_Long: case CXType_ULong: case CXType_LongLong: case CXType_ULongLong: {
        auto b = std::make_shared<Build>();
        b->kind = (t.kind == CXType_UShort || t.kind == CXType_UInt || t.kind == CXType_ULong ||
                   t.kind == CXType_ULongLong || t.kind == CXType_Char_U || t.kind == CXType_UChar)
                      ? Build::Kind::UInt
                      : Build::Kind::Int;
        b->localType = canon;
        b->spelled = canon;
        int_bounds(t.kind, *b);
        return ok_of(json{{"kind", kNodeScalar}, {"scalar", kScalarBigInt}, {"bounds", bigint_bounds(*b)}}, b);
    }
    case CXType_Float: case CXType_Double: case CXType_LongDouble: {
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Float;
        b->localType = canon;
        b->spelled = canon;
        return ok_of(json{{"kind", kNodeScalar},
                          {"scalar", kScalarNumber},
                          {"bounds", {{"int", false}, {"finiteOnly", false}}}},
                     b);
    }
    case CXType_Pointer: {
        // A byte or char pointer alone is a string (`char*`) or an opaque
        // (`void*`, `std::byte*`): the buffer row needs a following length
        // parameter, which discovery applies before these types reach here.
        CXType pointee = clang_getPointeeType(t);
        CXType pc = clang_getCanonicalType(pointee);
        bool pointeeConst = clang_isConstQualifiedType(pointee) != 0;
        if (pc.kind == CXType_FunctionProto) {
            // Qualifiers stay: the synthesized signature must match the real
            // function-pointer type exactly, or the assignment will not compile.
            std::string ret = type_spelling(clang_getCanonicalType(clang_getResultType(pointee)));
            std::vector<std::string> args;
            for (unsigned i = 0; i < clang_getNumArgTypes(pointee); ++i)
                args.push_back(type_spelling(clang_getCanonicalType(clang_getArgType(pointee, i))));
            return lower_callable(pointee, canon, "std::function<" + ret + "(" + join(args, ",") + ")>", ctx);
        }
        if (is_byte_kind(pc.kind)) {
            std::string pe = strip_qualifiers(type_spelling(pc));
            auto b = std::make_shared<Build>();
            b->kind = Build::Kind::CStr;
            b->localType = "std::string";
            b->spelled = canon;
            b->mutableCharPtr = !pointeeConst;
            b->pointee = pe;
            return ok_of(string_node(), b);
        }
        if (pc.kind == CXType_Void)
            return opaque_of(canon, canon, "a bare void* carries no shape");
        LowerResult inner = lower_type(pointee, ctx);
        if (inner.status != LowerResult::Status::Ok) return inner;
        // A pointer to a lowerable type passes a local's address; the node is
        // the pointee's node (table row 15).
        return inner;
    }
    case CXType_LValueReference: {
        // A reference parameter lowers through to the pointee: the driver
        // builds a non-const local and passes it.
        CXType pointee = clang_getPointeeType(t);
        LowerResult inner = lower_type(pointee, ctx);
        if (inner.status != LowerResult::Status::Ok) return inner;
        return inner;
    }
    case CXType_RValueReference:
        return refuse_of("an rvalue-reference parameter cannot be driven from a value tree");
    case CXType_MemberPointer:
        return refuse_of("a member pointer needs an object the driver does not have");
    case CXType_FunctionProto:
        return refuse_of("a function cannot be passed by value");
    case CXType_ConstantArray:
        // A real C++ parameter of array type is adjusted to a pointer before
        // it reaches here, so this kind only appears for non-parameter uses.
        return opaque_of(canon, canon, "the array type has no value-tree shape");
    case CXType_Enum: {
        CXCursor decl = clang_getTypeDeclaration(t);
        struct E { std::vector<long long> values; } e;
        clang_visitChildren(
            decl,
            [](CXCursor cur, CXCursor, CXClientData d) {
                auto* e = static_cast<E*>(d);
                if (clang_getCursorKind(cur) == CXCursor_EnumConstantDecl)
                    e->values.push_back(clang_getEnumConstantDeclValue(cur));
                return CXChildVisit_Continue;
            },
            &e);
        if (e.values.empty())
            return opaque_of(canon, canon, "the enum has no enumerators to choose from");
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Enum;
        b->localType = canon;
        b->spelled = canon;
        return ok_of(json{{"kind", kNodeLiteral}, {"values", e.values}}, b);
    }
    case CXType_Record:
        // std:: library templates reach here too in canonical form; the
        // spelling dispatch below decides. Project records go to lower_record.
        if (!starts(canon, "std::")) return lower_record(t, ctx);
        break;
    case CXType_Vector: case CXType_IncompleteArray: case CXType_VariableArray:
        return opaque_of(canon, canon, "the array form is not one the harness can size");
    case CXType_Unexposed:
        // Elaborated and template types land here; the spelling decides.
        break;
    }
    if (starts(canon, "std::basic_string<char") || canon == "std::string") {
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::String;
        b->localType = "std::string";
        b->spelled = canon;
        return ok_of(string_node(), b);
    }
    if (starts(canon, "std::basic_string_view<char") || canon == "std::string_view") {
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::StringView;
        b->localType = "std::string_view";
        b->spelled = canon;
        return ok_of(string_node(), b);
    }
    if (starts(canon, "std::function<")) {
        if (clang_Type_getNumTemplateArguments(t) >= 1) {
            CXType proto = clang_Type_getTemplateArgumentAsType(t, 0);
            if (proto.kind == CXType_FunctionProto)
                return lower_callable(proto, canon, canon, ctx);
        }
        return opaque_of(canon, canon, "the std::function signature cannot be read");
    }
    if (starts(canon, "std::unique_ptr<") || starts(canon, "std::shared_ptr<")) {
        CXType arg = clang_Type_getTemplateArgumentAsType(t, 0);
        if (arg.kind == CXType_Invalid)
            return opaque_of(canon, canon, "the smart-pointer pointee cannot be read");
        LowerResult inner = lower_type(arg, ctx);
        if (inner.status != LowerResult::Status::Ok) return inner;
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::PtrWrap;
        b->localType = canon;
        b->spelled = canon;
        b->elem = inner.build;
        return ok_of(inner.node, b);
    }
    if (starts(canon, "std::optional<")) {
        CXType arg = clang_Type_getTemplateArgumentAsType(t, 0);
        LowerResult inner = lower_type(arg, ctx);
        if (inner.status != LowerResult::Status::Ok) return inner;
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Optional;
        b->localType = "std::optional<" + inner.build->localType + ">";
        b->spelled = canon;
        b->elem = inner.build;
        return ok_of(json{{"kind", kNodeComposite},
                          {"composite", kCompUnion},
                          {"members", json::array({inner.node, scalar_node(kScalarNull)})}},
                     b);
    }
    if (starts(canon, "std::variant<")) {
        unsigned n = clang_Type_getNumTemplateArguments(t);
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Variant;
        b->localType = canon;
        b->spelled = canon;
        json members = json::array();
        for (unsigned i = 0; i < n; ++i) {
            LowerResult m = lower_type(clang_Type_getTemplateArgumentAsType(t, i), ctx);
            if (m.status != LowerResult::Status::Ok)
                return opaque_of(canon, m.blockerTy.empty() ? canon : m.blockerTy,
                                 "std::variant member " + std::to_string(i) + " (" + m.detail + ")");
            b->items.push_back(m.build);
            members.push_back(m.node);
        }
        return ok_of(json{{"kind", kNodeComposite}, {"composite", kCompUnion}, {"members", members}}, b);
    }
    if (starts(canon, "std::vector<") || starts(canon, "std::deque<")) {
        CXType arg = clang_Type_getTemplateArgumentAsType(t, 0);
        if (is_byte_type(arg)) {
            auto b = std::make_shared<Build>();
            b->kind = Build::Kind::BytesVec;
            b->localType = canon;
            b->spelled = canon;
            return ok_of(json{{"kind", kNodeBuffer}, {"length", length_node(4096)}}, b);
        }
        LowerResult inner = lower_type(arg, ctx);
        if (inner.status != LowerResult::Status::Ok) return inner;
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Seq;
        b->localType = canon;
        b->spelled = canon;
        b->container = starts(canon, "std::vector<") ? "std::vector" : "std::deque";
        b->elem = inner.build;
        return ok_of(json{{"kind", kNodeCollection},
                          {"collection", kCollArray},
                          {"type", inner.node},
                          {"length", length_node(64)}},
                     b);
    }
    if (starts(canon, "std::span<")) {
        CXType arg = clang_Type_getTemplateArgumentAsType(t, 0);
        if (!is_byte_type(arg)) return opaque_of(canon, canon, "only a byte span lowers to a buffer");
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Span;
        b->localType = canon;
        b->spelled = canon;
        return ok_of(json{{"kind", kNodeBuffer}, {"length", length_node(4096)}}, b);
    }
    if (starts(canon, "std::set<") || starts(canon, "std::unordered_set<")) {
        CXType arg = clang_Type_getTemplateArgumentAsType(t, 0);
        LowerResult inner = lower_type(arg, ctx);
        if (inner.status != LowerResult::Status::Ok) return inner;
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Set;
        b->localType = canon;
        b->spelled = canon;
        b->container = starts(canon, "std::set<") ? "std::set" : "std::unordered_set";
        b->elem = inner.build;
        return ok_of(json{{"kind", kNodeCollection},
                          {"collection", kCollSet},
                          {"type", inner.node},
                          {"length", length_node(64)}},
                     b);
    }
    if (starts(canon, "std::map<") || starts(canon, "std::unordered_map<")) {
        CXType key = clang_Type_getTemplateArgumentAsType(t, 0);
        std::string keyName = strip_qualifiers(type_spelling(clang_getCanonicalType(key)));
        if (!starts(keyName, "std::basic_string<char") && keyName != "std::string")
            return opaque_of(canon, keyName, "a map key must be a string type");
        CXType val = clang_Type_getTemplateArgumentAsType(t, 1);
        LowerResult inner = lower_type(val, ctx);
        if (inner.status != LowerResult::Status::Ok) return inner;
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Map;
        b->localType = canon;
        b->spelled = canon;
        b->container = starts(canon, "std::map<") ? "std::map" : "std::unordered_map";
        b->elem = inner.build;
        return ok_of(json{{"kind", kNodeCollection},
                          {"collection", kCollMap},
                          {"key", string_node()},
                          {"type", inner.node},
                          {"length", length_node(64)}},
                     b);
    }
    if (starts(canon, "std::pair<") || starts(canon, "std::tuple<")) {
        unsigned n = clang_Type_getNumTemplateArguments(t);
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Tuple;
        b->localType = canon;
        b->spelled = canon;
        b->isPair = starts(canon, "std::pair<");
        b->isTuple = !b->isPair;
        json items = json::array();
        for (unsigned i = 0; i < n; ++i) {
            LowerResult m = lower_type(clang_Type_getTemplateArgumentAsType(t, i), ctx);
            if (m.status != LowerResult::Status::Ok) return m;
            b->items.push_back(m.build);
            items.push_back(m.node);
        }
        return ok_of(json{{"kind", kNodeTuple}, {"items", items}}, b);
    }
    if (starts(canon, "std::array<")) {
        std::size_t n = 0;
        if (!array_size(canon, n) || n > 64)
            return opaque_of(canon, canon, "the std::array bound is unreadable or over 64");
        CXType arg = clang_Type_getTemplateArgumentAsType(t, 0);
        LowerResult first = lower_type(arg, ctx);
        if (first.status != LowerResult::Status::Ok) return first;
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Tuple;
        b->localType = canon;
        b->spelled = canon;
        b->fixedSize = n;
        json items = json::array();
        for (std::size_t i = 0; i < n; ++i) {
            b->items.push_back(first.build);
            items.push_back(first.node);
        }
        return ok_of(json{{"kind", kNodeTuple}, {"items", items}}, b);
    }
    if (starts(canon, "std::initializer_list<"))
        return refuse_of("an initializer_list parameter cannot be driven from a value tree");

    return opaque_of(canon, canon, "the type is not one the harness can build");
}

namespace {

struct FieldInfo {
    std::string name;
    CXType type;
};

LowerResult lower_record(CXType raw, LowerCtx& ctx) {
    CXType t = clang_getCanonicalType(raw);
    std::string name = strip_qualifiers(type_spelling(t));
    CXCursor decl = clang_getTypeDeclaration(t);
    CXCursorKind dk = clang_getCursorKind(decl);
    if (clang_Cursor_isNull(decl) || (dk != CXCursor_StructDecl && dk != CXCursor_ClassDecl))
        return opaque_of(name, name, "the class declaration is not visible or is a union");
    if (clang_Cursor_isNull(clang_getCursorDefinition(decl)))
        return opaque_of(name, name, "the class is only forward-declared");
    if (clang_getCursorKind(clang_getCursorSemanticParent(decl)) == CXCursor_ClassTemplate)
        return opaque_of(name, name, "the class is a template instantiation the driver cannot name");
    if (clang_CXXRecord_isAbstract(decl))
        return opaque_of(name, name, "the class is abstract");

    // Recursion through a member pointer to a class on the stack becomes a
    // `ref` back to the object that carries `id`.
    for (const auto& on : ctx.stack) {
        if (on != name) continue;
        auto it = ctx.objectIndex.find(name);
        if (it == ctx.objectIndex.end()) break;
        auto b = std::make_shared<Build>();
        b->kind = Build::Kind::Ref;
        b->localType = name;
        b->spelled = name;
        b->qualifiedName = name;
        b->builderFn = ctx.objects[it->second].builderFn;
        b->encoderFn = ctx.objects[it->second].encoderFn;
        return ok_of(json{{"kind", kNodeRef}, {"name", name}, {"fields", json::array()}}, b);
    }

    // A non-public data member makes the class itself the blocker: no value
    // tree can name the field, whatever the constructors allow.
    std::vector<FieldInfo> fields;
    bool nonPublic = false;
    struct Pack {
        std::vector<FieldInfo>* fields;
        bool* nonPublic;
    } pack{&fields, &nonPublic};
    clang_visitChildren(
        decl,
        [](CXCursor cur, CXCursor, CXClientData d) {
            auto* pack = static_cast<Pack*>(d);
            if (clang_getCursorKind(cur) != CXCursor_FieldDecl) return CXChildVisit_Continue;
            if (clang_getCXXAccessSpecifier(cur) != CX_CXXPublic) {
                *pack->nonPublic = true;
                return CXChildVisit_Break;
            }
            pack->fields->push_back({cursor_spelling(cur), clang_getCursorType(cur)});
            return CXChildVisit_Continue;
        },
        &pack);
    if (nonPublic) return opaque_of(name, name, "the class has a non-public data member");

    CtorInfo ci = ctor_info(decl);
    if (!ci.hasUsableDefault)
        return opaque_of(name, deepest_blocker(t, ci, ctx), "the class has no default constructor");

    // Reserve the builder/encoder names before the fields lower so a `ref`
    // back to this object can name them.
    std::size_t id = ctx.objects.size();
    ObjectDef def;
    def.qualifiedName = name;
    def.localType = name;
    def.builderFn = "afz_build_" + std::to_string(id);
    def.encoderFn = "afz_enc_" + std::to_string(id);
    ctx.objects.push_back(std::move(def));
    ctx.objectIndex[name] = id;

    ctx.stack.push_back(name);
    json fieldNodes = json::array();
    LowerResult failure;
    std::string failedField;
    bool failed = false;
    for (const auto& f : fields) {
        LowerResult fr = lower_type(f.type, ctx);
        if (fr.status != LowerResult::Status::Ok) {
            failure = std::move(fr);
            failedField = f.name;
            failed = true;
            break;
        }
        BuildPtr fb = fr.build;
        // A data member whose declared type is a pointer or a view cannot
        // take the built local directly: the builder heap-copies the pointee
        // so the member stays valid after the builder returns. A `ref` is
        // already a pointer member, and a function-pointer member takes the
        // std::function local through its conversion operator.
        CXType fc = clang_getCanonicalType(f.type);
        if (fb->kind != Build::Kind::Ref && fb->kind != Build::Kind::Fn &&
            (fc.kind == CXType_Pointer || fb->kind == Build::Kind::StringView)) {
            auto w = std::make_shared<Build>();
            w->kind = Build::Kind::FieldPtr;
            w->elem = fb;
            w->localType = fb->localType;
            w->spelled = fb->spelled;
            fb = w;
        }
        ctx.objects[id].fields.emplace_back(f.name, fb);
        fieldNodes.push_back(json{{"name", f.name}, {"type", fr.node}, {"required", true}});
    }
    ctx.stack.pop_back();
    if (failed) {
        ctx.objects.resize(id);
        ctx.objectIndex.erase(name);
        std::string blocker = failure.blockerTy.empty() ? name : failure.blockerTy;
        return opaque_of(name, blocker, "field " + failedField + " is not buildable: " + failure.detail);
    }

    auto b = std::make_shared<Build>();
    b->kind = Build::Kind::Object;
    b->localType = name;
    b->spelled = name;
    b->qualifiedName = name;
    b->builderFn = ctx.objects[id].builderFn;
    b->encoderFn = ctx.objects[id].encoderFn;
    b->fields = ctx.objects[id].fields;
    return ok_of(json{{"kind", kNodeObject}, {"name", name}, {"id", name}, {"fields", fieldNodes}}, b);
}

} // namespace

} // namespace afzh

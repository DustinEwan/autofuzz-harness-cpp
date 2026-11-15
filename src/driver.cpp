#include "driver.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <tuple>
#include <string_view>

#include "lower.h"
#include "sha256.h"
#include "templates.h"

namespace fs = std::filesystem;
namespace afzh {

namespace {

// ---------------------------------------------------------------------------
// Code generation.
// ---------------------------------------------------------------------------

std::string indent(int n) { return std::string(4 * n, ' '); }

std::string cstr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + '"';
}

// The aggregate's template head without its namespace: pair, tuple, or the
// whole spelling of an array-like type.
std::string base_name(std::string_view t) {
    auto lt = t.find('<');
    std::string head(lt == std::string_view::npos ? t : t.substr(0, lt));
    auto colon = head.rfind(':');
    if (colon != std::string::npos) head.erase(0, colon + 1);
    return head;
}

class Emitter {
public:
    // Declare and build `var` from JSON expression `expr`. Every kind emits
    // its own declaration so callers can nest freely. `hint` carries the
    // parameter's pass mode, which is what separates a std::function
    // parameter from a raw function-pointer one.
    void build(std::ostream& os, const Build& b, const std::string& var, const std::string& expr,
               int ind, Pass hint = Pass::Value, const std::string& fieldLvalue = "") {
        switch (b.kind) {
        case Build::Kind::Bool:
            decl(os, "bool", var, "afz::b_of(" + expr + ")", ind);
            break;
        case Build::Kind::Int:
        case Build::Kind::Enum:
            decl(os, b.localType, var, "static_cast<" + b.localType + ">(afz::i_of(" + expr + "))",
                 ind);
            break;
        case Build::Kind::UInt:
            decl(os, b.localType, var, "static_cast<" + b.localType + ">(afz::u_of(" + expr + "))",
                 ind);
            break;
        case Build::Kind::Float:
            decl(os, b.localType, var, "static_cast<" + b.localType + ">(afz::f_of(" + expr + "))",
                 ind);
            break;
        case Build::Kind::String:
        case Build::Kind::CStr:
            decl(os, "std::string", var, "afz::s_of(" + expr + ")", ind);
            break;
        case Build::Kind::StringView:
            decl(os, "std::string", var + "_backing", "afz::s_of(" + expr + ")", ind);
            decl(os, "std::string_view", var, var + "_backing", ind);
            break;
        case Build::Kind::BufferPtr:
            decl(os, "std::vector<std::uint8_t>", var, "afz::bytes_of(" + expr + ")", ind);
            break;
        case Build::Kind::BytesVec:
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << "{ auto __b = afz::bytes_of(" << expr << "); " << var
               << ".assign(__b.begin(), __b.end()); }\n";
            break;
        case Build::Kind::Span:
            os << indent(ind) << "std::vector<std::uint8_t> " << var << "_buf = afz::bytes_of("
               << expr << ");\n";
            os << indent(ind) << b.localType << " " << var << "(" << var << "_buf);\n";
            break;
        case Build::Kind::Seq: {
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << "for (const auto& __it : afz::arr_of(" << expr << ")) {\n";
            build(os, *b.elem, "__e", "__it", ind + 1, hint);
            os << indent(ind + 1) << var << ".push_back(std::move(__e));\n";
            os << indent(ind) << "}\n";
            break;
        }
        case Build::Kind::Set: {
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << "for (const auto& __it : afz::arr_of(" << expr << ")) {\n";
            build(os, *b.elem, "__e", "__it", ind + 1, hint);
            os << indent(ind + 1) << var << ".insert(std::move(__e));\n";
            os << indent(ind) << "}\n";
            break;
        }
        case Build::Kind::Map: {
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << "for (const auto& __e : afz::entries_of(" << expr << ")) {\n";
            build(os, *b.elem, "__v", "__e.second", ind + 1, hint);
            os << indent(ind + 1) << var << "[afz::s_of(__e.first)] = std::move(__v);\n";
            os << indent(ind) << "}\n";
            break;
        }
        case Build::Kind::Tuple: {
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << "{ const auto __arr = afz::arr_of(" << expr << ");\n";
            os << indent(ind + 1) << "if (__arr.size() != " << b.items.size()
               << ") afz::fail(\"expected " << b.items.size()
               << " items, got \" + std::to_string(__arr.size()));\n";
            for (std::size_t i = 0; i < b.items.size(); ++i)
                build(os, *b.items[i], "__t" + std::to_string(i),
                      "__arr[" + std::to_string(i) + "]", ind + 1, hint);
            std::string mv;
            for (std::size_t i = 0; i < b.items.size(); ++i) {
                if (i) mv += ", ";
                mv += "std::move(__t" + std::to_string(i) + ")";
            }
            std::string base = base_name(b.localType);
            if (base == "pair")
                os << indent(ind + 1) << var << " = std::make_pair(" << mv << ");\n";
            else if (base == "tuple")
                os << indent(ind + 1) << var << " = std::make_tuple(" << mv << ");\n";
            else
                os << indent(ind + 1) << var << " = " << b.localType << "{" << mv << "};\n";
            os << indent(ind) << "}\n";
            break;
        }
        case Build::Kind::Optional: {
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << "if (!afz::is_null_node(" << expr << ")) {\n";
            build(os, *b.elem, "__t", expr, ind + 1, hint);
            os << indent(ind + 1) << var << " = std::move(__t);\n";
            os << indent(ind) << "}\n";
            break;
        }
        case Build::Kind::Variant: {
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << "{ bool __ok = false;\n";
            for (std::size_t i = 0; i < b.items.size(); ++i) {
                if (i) os << indent(ind + 1) << "if (!__ok) ";
                os << indent(i ? 0 : ind + 1) << "try {\n";
                std::ostringstream member;
                build(member, *b.items[i], "__m", expr, 0, hint);
                std::istringstream lines(member.str());
                std::string line;
                while (std::getline(lines, line))
                    if (!line.empty()) os << indent(ind + 2) << line << "\n";
                os << indent(ind + 2) << var << " = std::move(__m);\n";
                os << indent(ind + 2) << "__ok = true;\n";
                os << indent(ind + 1) << "} catch (const afz::BuildError&) {}\n";
            }
            os << indent(ind + 1) << "if (!__ok) afz::fail(\"no member of " << b.localType
               << " accepts \" + afz::show(" << expr << "));\n";
            os << indent(ind) << "}\n";
            break;
        }
        case Build::Kind::Object:
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << b.builderFn << "(" << expr << ", " << var << ", 0);\n";
            break;
        case Build::Kind::Ref:
            // Only reachable as a struct field; the object builder handles
            // it. A stray one stays null.
            os << indent(ind) << b.localType << "* " << var << " = nullptr;\n";
            break;
        case Build::Kind::Fn: {
            std::string sig = b.localType; // std::function<R(A...)>
            auto lt = sig.find('<');
            auto gt = sig.rfind('>');
            std::string inner = (lt != std::string::npos && gt != std::string::npos && gt > lt)
                                    ? sig.substr(lt + 1, gt - lt - 1)
                                    : "void()";
            if (hint == Pass::FnPtr) {
                os << indent(ind) << "afz::Stub<" << inner << "> " << var << ";\n";
                os << indent(ind) << var << ".behavior = afz::stub_behavior(" << expr << ");\n";
            } else {
                decl(os, b.localType, var, "afz::stub_function<" + inner + ">(" + expr + ")", ind);
            }
            break;
        }
        case Build::Kind::PtrWrap: {
            os << indent(ind) << b.localType << " " << var << ";\n";
            os << indent(ind) << "{\n";
            build(os, *b.elem, "__t", expr, ind + 1, hint);
            std::string inner = b.localType;
            std::string maker = inner.rfind("std::shared_ptr", 0) == 0 ? "std::make_shared"
                                                                       : "std::make_unique";
            auto sp = inner.find('<');
            if (sp != std::string::npos) inner = inner.substr(sp + 1, inner.size() - sp - 2);
            os << indent(ind + 1) << var << " = " << maker << "<" << inner << ">(std::move(__t));\n";
            os << indent(ind) << "}\n";
            break;
        }
        case Build::Kind::FieldPtr: {
            // A pointer or view data member: build the pointee into a heap
            // object that outlives the builder. The process is short-lived;
            // the leak is accepted.
            const std::string& lv = fieldLvalue.empty() ? var : fieldLvalue;
            os << indent(ind) << "{\n";
            if (b.elem->kind == Build::Kind::CStr || b.elem->kind == Build::Kind::String ||
                b.elem->kind == Build::Kind::StringView) {
                os << indent(ind + 1) << "auto* __s = new std::string(afz::s_of(" << expr
                   << "));\n";
                if (b.elem->kind == Build::Kind::StringView)
                    os << indent(ind + 1) << lv << " = std::string_view(*__s);\n";
                else
                    os << indent(ind + 1) << lv << " = __s->data();\n";
            } else {
                build(os, *b.elem, "__t", expr, ind + 1, hint);
                os << indent(ind + 1) << lv << " = new " << b.elem->localType
                   << "(std::move(__t));\n";
            }
            os << indent(ind) << "}\n";
            break;
        }
        }
    }

    void decl(std::ostream& os, const std::string& type, const std::string& var,
              const std::string& init, int ind) {
        os << indent(ind) << type << " " << var << "(" << init << ");\n";
    }

    // Encode expression for the value tree of `expr`.
    std::string encode(const Build& b, const std::string& x, int depth) {
        switch (b.kind) {
        case Build::Kind::Bool: return "afz::vt_bool(" + x + ")";
        case Build::Kind::Int:
        case Build::Kind::Enum: return "afz::vt_int(" + x + ")";
        case Build::Kind::UInt: return "afz::vt_uint(" + x + ")";
        case Build::Kind::Float: return "afz::vt_double(" + x + ")";
        case Build::Kind::String:
        case Build::Kind::CStr:
        case Build::Kind::StringView: return "afz::vt_string(" + x + ")";
        case Build::Kind::BufferPtr:
        case Build::Kind::BytesVec:
        case Build::Kind::Span: return "afz::vt_bytes(" + x + ")";
        case Build::Kind::Seq:
            return "afz::vt_seq(" + x + ", [](const auto& __e) { return " +
                   encode(*b.elem, "__e", depth) + "; })";
        case Build::Kind::Set:
            return "afz::vt_setv(" + x + ", [](const auto& __e) { return " +
                   encode(*b.elem, "__e", depth) + "; })";
        case Build::Kind::Map:
            return "afz::vt_map(" + x +
                   ", [](const auto& __k) { return afz::vt_string(__k); }, [](const auto& __v) { return " +
                   encode(*b.elem, "__v", depth) + "; })";
        case Build::Kind::Tuple: {
            std::string items;
            std::string base = base_name(b.localType);
            for (std::size_t i = 0; i < b.items.size(); ++i) {
                if (i) items += ", ";
                std::string acc;
                if (base == "pair") acc = i == 0 ? (x + ".first") : (x + ".second");
                else if (base == "tuple") acc = "std::get<" + std::to_string(i) + ">(" + x + ")";
                else acc = x + "[" + std::to_string(i) + "]";
                items += encode(*b.items[i], acc, depth);
            }
            return "afz::json{{\"node\",\"tuple\"},{\"items\", afz::json::array({" + items + "})}}";
        }
        case Build::Kind::Optional:
            return "((" + x + ".has_value()) ? " + encode(*b.elem, "(*" + x + ")", depth) +
                   " : afz::vt_null())";
        case Build::Kind::Variant: {
            std::string arms;
            for (std::size_t i = 0; i < b.items.size(); ++i) {
                arms += (i ? " else " : "") +
                        std::string("if constexpr (std::is_same_v<std::decay_t<decltype(__m)>, ") +
                        b.items[i]->localType + ">) return " + encode(*b.items[i], "__m", depth) + ";";
            }
            return "std::visit([&](const auto& __m) -> afz::json { " + arms +
                   " return afz::vt_null(); }, " + x + ")";
        }
        case Build::Kind::Object: return b.encoderFn + "(" + x + ", " + std::to_string(depth) + ")";
        case Build::Kind::Ref:
            return "(((" + x + ") && " + std::to_string(depth) +
                   " < 16) ? " + b.encoderFn + "(*" + x + ", " + std::to_string(depth + 1) +
                   ") : afz::vt_null())";
        case Build::Kind::Fn: return "afz::vt_null()";
        case Build::Kind::PtrWrap:
            return "((" + x + ") ? " + encode(*b.elem, "(*" + x + ")", depth) + " : afz::vt_null())";
        case Build::Kind::FieldPtr:
            if (b.elem->kind == Build::Kind::CStr)
                return "afz::vt_string(" + x + " ? std::string(" + x + ") : std::string())";
            if (b.elem->kind == Build::Kind::StringView || b.elem->kind == Build::Kind::String)
                return "afz::vt_string(" + x + ")";
            return "((" + x + ") ? " + encode(*b.elem, "(*" + x + ")", depth) + " : afz::vt_null())";
        }
        return "afz::vt_null()";
    }
};

// The object builders and encoders, one pair per lowered record. A failed
// variant member or a field beyond the recursion cap throws so the caller
// answers `driver-error` instead of corrupting memory.
void emit_object(std::ostream& os, const ObjectDef& o) {
    Emitter em;
    os << "// " << o.qualifiedName << "\n";
    os << "[[maybe_unused]] static void " << o.builderFn << "(const afz::json& v, "
       << o.qualifiedName << "& out, int depth) {\n";
    os << "    const afz::json& o = afz::obj_of(v);\n";
    for (std::size_t fi = 0; const auto& [fname, fbuild] : o.fields) {
        ++fi;
        const std::string slot = std::to_string(fi);
        if (fbuild->kind == Build::Kind::Fn) {
            // A field may be std::function or a raw function pointer; the
            // Stub's conversion operator serves both, exactly as a raw
            // function-pointer parameter does.
            std::string sig = fbuild->localType;
            auto lt = sig.find('<'), gt = sig.rfind('>');
            std::string inner = (lt != std::string::npos && gt != std::string::npos && gt > lt)
                                    ? sig.substr(lt + 1, gt - lt - 1)
                                    : "void()";
            os << "    { afz::Stub<" << inner << "> __st;\n";
            os << "      __st.behavior = afz::stub_behavior(afz::field(o, " << cstr(fname)
               << "));\n";
            os << "      out." << fname
               << " = static_cast<typename afz::Stub<" << inner << ">::FnPtr>(__st); }\n";
        } else if (fbuild->kind == Build::Kind::Ref) {
            os << "    out." << fname << " = nullptr;\n";
            os << "    { const afz::json& __t = afz::field_or_null(o, " << cstr(fname) << ");\n";
            os << "      if (!afz::is_null_node(__t)) {\n";
            os << "        if (depth >= 16) afz::fail(\"recursive value deeper than 16\");\n";
            os << "        auto* __n = new " << o.qualifiedName << ";\n";
            os << "        " << o.builderFn << "(__t, *__n, depth + 1);\n";
            os << "        out." << fname << " = __n;\n";
            os << "      } }\n";
        } else if (fbuild->kind == Build::Kind::FieldPtr) {
            em.build(os, *fbuild, "__fp" + slot, "afz::field(o, " + cstr(fname) + ")", 1,
                     Pass::Value, "out." + fname);
        } else {
            em.build(os, *fbuild, "__f" + slot, "afz::field(o, " + cstr(fname) + ")", 1);
            os << "    out." << fname << " = std::move(__f" << slot << ");\n";
        }
    }
    os << "}\n";
    os << "[[maybe_unused]] static afz::json " << o.encoderFn << "(const " << o.qualifiedName
       << "& x, int depth) {\n";
    os << "    return afz::json{{\"node\",\"object\"},{\"fields\", afz::json::array({\n";
    bool first = true;
    for (const auto& [fname, fbuild] : o.fields) {
        if (!first) os << ",\n";
        first = false;
        os << "        afz::json::array({" << cstr(fname) << ", "
           << em.encode(*fbuild, "x." + fname, 0) << "})";
    }
    os << "\n    })}};\n}\n\n";
}

// Forward declarations, so a builder or encoder may reference objects that
// are defined later in the file.
void emit_object_forwards(std::ostream& os, const std::vector<ObjectDef>& objects) {
    for (const auto& o : objects) {
        os << "static void " << o.builderFn << "(const afz::json&, " << o.qualifiedName
           << "&, int);\n";
        os << "static afz::json " << o.encoderFn << "(const " << o.qualifiedName << "&, int);\n";
    }
    os << "\n";
}

// The argument expression for one built parameter.
std::string pass_expr(const Param& p, const std::string& var) {
    switch (p.pass) {
    case Pass::Buffer: {
        const std::string& pe = p.build->pointee;
        std::string ptr;
        if (!p.padToVar.empty()) {
            std::string want = p.padToVar + ".size()";
            if (p.padMode == 1) want = "afz::afz_span(" + p.padToVar + ")";
            else if (p.padMode == 2)
                want = p.padToVar + ".size() + " + p.padToVar + ".size() / 2 + 32";
            ptr = "reinterpret_cast<" + pe + "*>(afz::pad_to(" + var + ", " + want + "))";
        } else if (pe == "void") {
            ptr = "static_cast<void*>(" + var + ".data())";
        } else {
            ptr = "reinterpret_cast<" + pe + "*>(" + var + ".data())";
        }
        // The folded count is passed at its original position from
        // Tgt::derived, never appended here.
        return ptr;
    }
    case Pass::CStrConst:
    case Pass::CStrMut: {
        // The local is std::string; a byte-typed pointee (uint8_t*, not just
        // char*) needs its address reinterpreted, exactly like a buffer does.
        const std::string& pe = p.build->pointee;
        bool plain = pe.empty() || pe == "char" || pe == "signed char";
        if (plain) return p.pass == Pass::CStrConst ? var + ".c_str()" : var + ".data()";
        std::string t = p.pass == Pass::CStrConst ? "const " + pe : pe;
        return "reinterpret_cast<" + t + "*>(" + var + ".data())";
    }
    case Pass::Ptr: return "&" + var;
    case Pass::Value:
    case Pass::Ref:
    case Pass::FnPtr: return var;
    }
    return var;
}

// One target's thunk.
void emit_thunk(std::ostream& os, const Tgt& t, int index) {
    Emitter em;
    os << "// afz:begin " << t.id << "\n";
    os << "static afz::json call_" << index << "(const afz::json& a) {\n";
    std::size_t n = t.params.size();
    os << "    if (a.size() != " << n << ") afz::fail("
       << cstr(t.name + " takes " + std::to_string(n) + " arguments, got ")
       << " + std::to_string(a.size()));\n";
    for (std::size_t i = 0; i < n; ++i)
        em.build(os, *t.params[i].build, "p" + std::to_string(i),
                 "a[" + std::to_string(i) + "]", 1, t.params[i].pass);

    std::string call;
    // A method thunk builds the receiver as p0; the argument list starts
    // after it.
    std::size_t dnext = 0;
    auto drain_derived = [&](std::size_t upto) {
        for (; dnext < t.derived.size() && t.derived[dnext].pos <= upto; ++dnext) {
            if (!call.empty()) call += ", ";
            call += "static_cast<" + t.derived[dnext].castType + ">(p" +
                    std::to_string(t.derived[dnext].ownerParam) + ".size())";
        }
    };
    for (std::size_t i = t.isMethod ? 1 : 0; i < n; ++i) {
        drain_derived(i);
        if (!call.empty()) call += ", ";
        call += pass_expr(t.params[i], "p" + std::to_string(i));
    }
    drain_derived(n);
    std::string expr = t.isMethod ? "p0." + t.callPath + "(" + call + ")"
                                  : t.callPath + "(" + call + ")";

    if (!t.returnsVoid) os << "    auto __r = " << expr << ";\n";
    else os << "    " << expr << ";\n";

    std::string valueExpr; // the `value` node, empty when there is none
    if (t.returnBuild) {
        if (t.returnsOptional) {
            os << "    if (!__r.has_value()) return afz::json{{\"status\",\"empty\"}};\n";
            valueExpr = em.encode(*t.returnBuild, "(*__r)", 0);
        } else {
            valueExpr = em.encode(*t.returnBuild, "__r", 0);
        }
    }

    bool reportsState = t.isMethod;
    for (const Param& p : t.params)
        if (p.mutableOut) reportsState = true;

    if (!reportsState) {
        if (valueExpr.empty()) os << "    return afz::ret_void();\n";
        else os << "    return afz::ret(" << valueExpr << ");\n";
    } else {
        os << "    afz::json __fields = afz::json::array();\n";
        if (!valueExpr.empty())
            os << "    __fields.push_back(afz::json::array({\"$return\", " << valueExpr << "}));\n";
        for (std::size_t i = 0; i < n; ++i) {
            if (!t.params[i].mutableOut) continue;
            os << "    __fields.push_back(afz::json::array({" << cstr(t.params[i].name) << ", "
               << em.encode(*t.params[i].build, "p" + std::to_string(i), 0) << "}));\n";
        }
        if (t.isMethod && !t.params.empty() && !t.params[0].build->encoderFn.empty())
            os << "    __fields.push_back(afz::json::array({\"$receiver\", "
               << t.params[0].build->encoderFn << "(p0, 0)}));\n";
        os << "    return afz::ret(afz::json{{\"node\",\"object\"},{\"fields\", __fields}});\n";
    }
    os << "}\n";
    os << "// afz:end " << t.id << "\n\n";
}

} // namespace

std::string render_calls(const Inventory& inv, const std::vector<std::string>& dropped,
                         std::vector<std::pair<std::string, std::pair<int, int>>>& spans) {
    std::ostringstream os;
    os << "// Generated by afz-cpp: the call layer. Do not edit.\n";
    os << "#include \"afz_rt.h\"\n\n";
    os << "#include <array>\n#include <deque>\n#include <functional>\n#include <map>\n"
          "#include <memory>\n#include <optional>\n#include <set>\n#include <span>\n"
          "#include <string>\n#include <string_view>\n#include <tuple>\n#include <type_traits>\n"
          "#include <unordered_map>\n#include <unordered_set>\n#include <variant>\n#include <vector>\n\n";
    for (const std::string& h : inv.headers) os << "#include \"" << h << "\"\n";
    os << "\nnamespace afzcalls {\n\n";
    emit_object_forwards(os, inv.lower.objects);
    for (const auto& o : inv.lower.objects) emit_object(os, o);

    std::vector<const Tgt*> kept;
    for (const Tgt& t : inv.targets) {
        if (std::find(dropped.begin(), dropped.end(), t.id) != dropped.end()) continue;
        kept.push_back(&t);
    }
    int index = 0;
    auto line_count = [&os] {
        std::string sofar = os.str();
        return static_cast<int>(std::count(sofar.begin(), sofar.end(), '\n')) + 1;
    };
    for (const Tgt* t : kept) {
        int begin = line_count();
        emit_thunk(os, *t, index++);
        int end = line_count();
        spans.emplace_back(t->id, std::make_pair(begin, end));
    }

    os << "afz::json dispatch(const std::string& id, const afz::json& a) {\n";
    index = 0;
    for (const Tgt* t : kept)
        os << "    if (id == " << cstr(t->id) << ") return call_" << index++ << "(a);\n";
    os << "    afz::fail(\"the driver has no target with id \" + id);\n}\n";
    os << "} // namespace afzcalls\n";
    return os.str();
}

std::string render_repro(const Inventory& inv, const Tgt& tgt, const json& values) {
    std::ostringstream os;
    os << "// afz-cpp reproducer for " << tgt.name << " (" << tgt.file << ":" << tgt.line << ")\n";
    os << "// Build from the project root with the project's own flags and sources:\n";
    os << "//   clang++ -std=c++20 -g repro.cpp <project sources> <project flags> -I <harness>/third_party -o repro\n";
    os << "// Then run ./repro: a returned call prints its value and exits 0; a fault\n";
    os << "// prints its status and exits 1, or dies on the same signal as the driver.\n";
    os << templates::kRtHeader;
    os << "\n#include <array>\n#include <deque>\n#include <functional>\n#include <map>\n"
          "#include <memory>\n#include <optional>\n#include <set>\n#include <span>\n"
          "#include <string>\n#include <string_view>\n#include <tuple>\n#include <type_traits>\n"
          "#include <unordered_map>\n#include <unordered_set>\n#include <variant>\n#include <vector>\n"
          "#include <cstdio>\n\n";
    for (const std::string& h : inv.headers) os << "#include \"" << h << "\"\n";
    os << "\nnamespace afzcalls {\n\n";
    emit_object_forwards(os, inv.lower.objects);
    for (const auto& o : inv.lower.objects) emit_object(os, o);
    emit_thunk(os, tgt, 0);
    os << "} // namespace afzcalls\n\n";
    // A raw-string delimiter that cannot collide with the embedded JSON.
    std::string body = afz_dump(values);
    std::string delim = "AFZV";
    while (body.find(")" + delim + "(") != std::string::npos) delim += "x";
    os << "int main() {\n";
    os << "    static const char kArgs[] = R\"DELIM(" << body << ")DELIM\";\n";
    os << "    afz::json args = afz::json::parse(kArgs);\n";
    os << "    afz::json out;\n";
    os << "    try { out = afzcalls::call_0(args); }\n";
    os << "    catch (const afz::BuildError& e) { out = {{\"status\",\"error\"},{\"message\",e.message}}; }\n";
    os << "    catch (const std::exception& e) { out = {{\"status\",\"throw\"},{\"what\",e.what()}}; }\n";
    os << "    catch (...) { out = {{\"status\",\"throw-unknown\"}}; }\n";
    os << "    std::printf(\"%s\\n\", afz::afz_dump(out).c_str());\n";
    os << "    std::fflush(stdout);\n";
    os << "    std::string s = out.is_object() && out.contains(\"status\")\n";
    os << "                      ? out[\"status\"].get<std::string>() : \"\";\n";
    os << "    return s == \"returned\" ? 0 : 1;\n";
    os << "}\n";
    std::string text = os.str();
    // Swap the placeholder delimiter for the collision-free one.
    for (std::size_t at = text.find("DELIM"); at != std::string::npos; at = text.find("DELIM", at + delim.size() + 2))
        text.replace(at, 5, delim);
    return text;
}

// ---------------------------------------------------------------------------
// Build.
// ---------------------------------------------------------------------------

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return out.good();
}

// The vendored header tree that `afz_rt.h` includes (<nlohmann/json.hpp>).
// The plugin lives at <harness>/build/, so the checkout's third_party/ sits
// two levels up from the executable directory.
std::string vendor_include() {
    static std::string cached;
    static bool probed = false;
    if (!probed) {
        probed = true;
        char buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            fs::path p = fs::path(buf).parent_path().parent_path() / "third_party";
            if (fs::exists(p / "nlohmann" / "json.hpp")) cached = p.string();
        }
    }
    return cached;
}

std::string shell_quote(const std::string& a) {
    std::string q = "'";
    for (char c : a) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    return q + "'";
}

std::string run_capture(const std::vector<std::string>& argv, const std::string& cwd,
                        int& exitCode) {
    std::string cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += ' ';
        cmd += shell_quote(a);
    }
    std::string full = "cd " + shell_quote(cwd) + " && " + cmd + " 2>&1";
    FILE* p = popen(full.c_str(), "r");
    if (!p) {
        exitCode = -1;
        return "cannot launch the compiler";
    }
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof buf, p)) out += buf;
    int st = pclose(p);
    exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
}

// The compiler lines that point into the generated call layer.
std::vector<int> failing_lines(const std::string& compilerOutput) {
    std::vector<int> lines;
    std::istringstream in(compilerOutput);
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find("afz_calls.cpp:");
        if (pos == std::string::npos) continue;
        if (line.find(": error:") == std::string::npos &&
            line.find(": fatal error:") == std::string::npos)
            continue;
        std::size_t i = pos + std::string_view("afz_calls.cpp:").size();
        std::size_t j = i;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
        if (j > i) lines.push_back(std::stoi(line.substr(i, j - i)));
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

std::string tail(const std::string& text, std::size_t bytes) {
    if (text.size() <= bytes) return text;
    return text.substr(text.size() - bytes);
}

std::string options_fingerprint(const Options& o) {
    json j{{"cxx", o.cxx},
           {"std", o.std},
           {"cxxFlags", o.cxxFlags},
           {"sources", o.sources},
           {"libraries", o.libraries},
           {"linkFlags", o.linkFlags},
           {"sanitize", o.sanitize},
           {"coverage", o.coverage}};
    return afz_dump(j);
}


// Staged build: compile every translation unit, then link. The scaffolding
// units (driver, calls, reproducer) pull in nlohmann/json.hpp, and they are
// scaffolding rather than the code under test, so they compile at -O0; the
// project's own sources keep the -O1 that fault frames and timings expect.
struct StagedBuild {
    std::vector<std::string> common;
    std::string root;
    std::string output;
    int code = 0;

    // Compile every unit concurrently: the units are independent, and the
    // slowest one dominates the wall time anyway. Outputs are concatenated
    // in unit order, so the repair loop's line attribution survives.
    bool compile_all(const std::vector<std::tuple<std::string, std::string, const char*>>& units,
                     unsigned cap) {
        std::vector<std::string> outs(units.size());
        std::vector<int> codes(units.size(), -1);
        std::atomic<std::size_t> next{0};
        auto worker = [&] {
            for (std::size_t i = next.fetch_add(1); i < units.size(); i = next.fetch_add(1)) {
                std::vector<std::string> argv = common;
                argv.push_back(std::get<2>(units[i]));
                argv.push_back("-c");
                argv.push_back(std::get<0>(units[i]));
                argv.push_back("-o");
                argv.push_back(std::get<1>(units[i]));
                outs[i] = run_capture(argv, root, codes[i]);
            }
        };
        unsigned n = std::max<std::size_t>(1, std::min<std::size_t>(cap, units.size()));
        std::vector<std::thread> pool;
        pool.reserve(n);
        for (unsigned t = 0; t < n; ++t) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        bool ok = true;
        for (std::size_t i = 0; i < units.size(); ++i) {
            output += outs[i];
            if (codes[i] != 0) {
                if (ok) code = codes[i] > 0 ? codes[i] : 1;
                ok = false;
            }
        }
        if (ok) code = 0;
        return ok;
    }

    bool link(const std::vector<std::string>& objs, const std::string& bin, const Options& opt,
              bool coverage) {
        std::vector<std::string> argv = {opt.cxx};
        for (const auto& s : opt.sanitize) argv.push_back("-fsanitize=" + s);
        // The instrumented objects need the profiling runtime at link time.
        if (coverage) argv.push_back("-fprofile-instr-generate");
        for (const auto& f : opt.cxxFlags) argv.push_back(f);
        for (const auto& o : objs) argv.push_back(o);
        argv.push_back("-o");
        argv.push_back(bin);
        if (!opt.libraries.empty()) {
            argv.push_back("-Wl,--start-group");
            for (const auto& l : opt.libraries) argv.push_back(l);
            argv.push_back("-Wl,--end-group");
        }
        for (const auto& f : opt.linkFlags) argv.push_back(f);
        output += run_capture(argv, root, code);
        return code == 0;
    }
};
} // namespace

bool build_driver(const Inventory& inv, const Options& opt, Driver& out, std::string& error) {
    out.dir = inv.scratch;
    out.bin = out.dir + "/build/afz-cpp-driver";
    out.profiles = out.dir + "/profiles";
    std::error_code ec;
    fs::create_directories(out.dir + "/build", ec);
    fs::create_directories(out.profiles, ec);
    if (!write_file(out.dir + "/afz_rt.h", templates::kRtHeader) ||
        !write_file(out.dir + "/afz_driver.cpp", templates::kDriverMain)) {
        error = "cannot write the scratch files under " + out.dir;
        return false;
    }

    std::vector<std::string> dropped;
    std::string lastOutput;
    for (int round = 0; round <= 40; ++round) {
        std::vector<std::pair<std::string, std::pair<int, int>>> spans;
        std::string calls = render_calls(inv, dropped, spans);
        std::string callsPath = out.dir + "/afz_calls.cpp";
        // The stamp covers the templates too: a plugin upgrade changes the
        // runtime header and the driver entry without touching afz_calls.cpp.
        std::string stamp = sha256_hex(calls + "\0" + options_fingerprint(opt) + "\0" +
                                       templates::kRtHeader + templates::kDriverMain);
        std::string stampPath = out.dir + "/build/afz.stamp";
        bool fresh = false;
        int lockFd = ::open((out.dir + "/build/afz.lock").c_str(), O_CREAT | O_RDWR, 0644);
        if (lockFd < 0) {
            error = "cannot open " + out.dir + "/build/afz.lock";
            return false;
        }
        ::flock(lockFd, LOCK_EX);
        if (read_file(callsPath) == calls && read_file(stampPath) == stamp &&
            access(out.bin.c_str(), X_OK) == 0) {
            fresh = true; // another worker already built this exact driver
        } else if (!write_file(callsPath, calls)) {
            ::flock(lockFd, LOCK_UN);
            ::close(lockFd);
            error = "cannot write " + callsPath;
            return false;
        } else {
            StagedBuild sb;
            sb.root = inv.root;
            sb.common = {opt.cxx, "-std=" + opt.std, "-g", "-fno-omit-frame-pointer"};
            if (opt.coverage) {
                sb.common.push_back("-fprofile-instr-generate");
                sb.common.push_back("-fcoverage-mapping");
            }
            for (const auto& s : opt.sanitize) sb.common.push_back("-fsanitize=" + s);
            for (const auto& f : opt.cxxFlags) sb.common.push_back(f);
            sb.common.push_back("-I" + out.dir);
            if (!vendor_include().empty()) sb.common.push_back("-I" + vendor_include());
            sb.common.push_back("-I" + inv.root);
            std::vector<std::tuple<std::string, std::string, const char*>> units;
            units.emplace_back(out.dir + "/afz_driver.cpp", out.dir + "/build/afz_driver.o",
                               "-O0");
            units.emplace_back(callsPath, out.dir + "/build/afz_calls.o", "-O0");
            for (std::size_t i = 0; i < opt.sources.size(); ++i)
                units.emplace_back(opt.sources[i],
                                   out.dir + "/build/src" + std::to_string(i) + ".o", "-O1");
            bool ok = sb.compile_all(units, std::thread::hardware_concurrency());
            std::vector<std::string> objs;
            if (ok)
                for (const auto& u : units) objs.push_back(std::get<1>(u));
            if (ok) ok = sb.link(objs, out.bin, opt, opt.coverage);
            lastOutput = sb.output;
            const int code = ok ? 0 : (sb.code != 0 ? sb.code : 1);
            if (code == 0) {
                write_file(stampPath, stamp);
                fresh = true;
            } else {
                std::vector<int> bad = failing_lines(lastOutput);
                std::vector<std::string> blamed;
                for (const auto& [id, range] : spans) {
                    for (int line : bad)
                        if (line >= range.first && line <= range.second) {
                            blamed.push_back(id);
                            break;
                        }
                }
                if (blamed.empty()) {
                    ::flock(lockFd, LOCK_UN);
                    ::close(lockFd);
                    error = "the driver does not compile:\n" + tail(lastOutput, 4000);
                    return false;
                }
                std::string ids;
                for (const auto& id : blamed) {
                    if (std::find(dropped.begin(), dropped.end(), id) == dropped.end()) {
                        dropped.push_back(id);
                        ids += id + " ";
                    }
                }
                std::fprintf(stderr, "afz-cpp: round %d dropped [%s]; compiler tail:\n%s\n",
                             round, ids.c_str(), tail(lastOutput, 1500).c_str());
                std::fflush(stderr);
            }
        }
        ::flock(lockFd, LOCK_UN);
        ::close(lockFd);
        if (fresh) break;
        if (round == 40) {
            error = "the repair loop did not converge after 40 rounds";
            return false;
        }
    }
    out.dropped = dropped;
    out.binExists = access(out.bin.c_str(), X_OK) == 0;
    if (!out.binExists) {
        error = "the driver binary is missing after the build";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Run one call.
// ---------------------------------------------------------------------------

RunResult run_call(const Driver& drv, const Tgt& tgt, const json& args, double deadlineMs,
                   const std::string& profraw) {
    RunResult res;
    json request{{"call", tgt.id}, {"args", args}};
    std::string input = afz_dump(request);

    int inPipe[2], outPipe[2], errPipe[2], fd3Pipe[2];
    if (pipe(inPipe) || pipe(outPipe) || pipe(errPipe) || pipe(fd3Pipe)) {
        res.detail = "cannot create the pipes for the driver child";
        return res;
    }
    auto start = std::chrono::steady_clock::now();
    pid_t pid = fork();
    // Own the child to its own process group from the parent side too: a
    // deadline that fires before the child's own setpgid would otherwise
    // make kill(-pid) signal the plugin's group - and the engine with it.
    ::setpgid(pid, 0);
    if (pid < 0) {
        res.detail = "cannot start the driver child";
        return res;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        dup2(fd3Pipe[1], 3);
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        close(fd3Pipe[0]); close(fd3Pipe[1]);
        if (!profraw.empty()) {
            ::setenv("LLVM_PROFILE_FILE", profraw.c_str(), 1);
            // Merge-free per-call files; the plugin merges with llvm-profdata.
            ::setenv("LLVM_PROFILE_FILE_PERCENT", "1", 0);
        }
        ::setenv("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0", 0);
        ::setenv("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1", 0);
        execl(drv.bin.c_str(), "afz-cpp-driver", nullptr);
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    close(fd3Pipe[1]);

    for (int fd : {inPipe[1], outPipe[0], errPipe[0], fd3Pipe[0]})
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    std::string outText, errText, fd3Text;
    std::size_t written = 0;
    bool stdinDone = false;
    int status = 0;
    bool killed = false;
    while (true) {
        if (!stdinDone) {
            ssize_t n = ::write(inPipe[1], input.data() + written, input.size() - written);
            if (n > 0) written += static_cast<std::size_t>(n);
            if (written >= input.size() || (n < 0 && errno == EPIPE)) {
                close(inPipe[1]);
                stdinDone = true;
            }
        }
        for (int fd : {outPipe[0], errPipe[0], fd3Pipe[0]}) {
            char buf[8192];
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n > 0)
                (fd == outPipe[0] ? outText : fd == errPipe[0] ? errText : fd3Text)
                    .append(buf, static_cast<std::size_t>(n));
        }
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) break;
        double waited = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start).count();
        if (waited > deadlineMs) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL); // belt and braces if the group was never set
            killed = true;
            waitpid(pid, &status, 0);
            break;
        }
        struct pollfd pfd[4];
        int count = 0;
        if (!stdinDone) pfd[count++] = {inPipe[1], POLLOUT, 0};
        pfd[count++] = {outPipe[0], POLLIN, 0};
        pfd[count++] = {errPipe[0], POLLIN, 0};
        pfd[count++] = {fd3Pipe[0], POLLIN, 0};
        ::poll(pfd, static_cast<nfds_t>(count), 5);
    }
    for (int fd : {outPipe[0], errPipe[0], fd3Pipe[0]}) {
        char buf[8192];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof buf)) > 0)
            (fd == outPipe[0] ? outText : fd == errPipe[0] ? errText : fd3Text)
                .append(buf, static_cast<std::size_t>(n));
    }
    close(outPipe[0]);
    close(errPipe[0]);
    close(fd3Pipe[0]);
    if (!stdinDone) close(inPipe[1]);

    res.durationMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start).count();
    res.stderrText = errText;

    if (killed) {
        res.kind = RunKind::Deadline;
        return res;
    }

    // The answer: the first line of the coverage channel, else the last
    // stdout line that starts with '{'.
    std::string answerLine;
    {
        std::istringstream in(fd3Text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.front() == '{') { answerLine = line; break; }
        }
        if (answerLine.empty()) {
            std::istringstream so(outText);
            while (std::getline(so, line))
                if (!line.empty() && line.front() == '{') answerLine = line; // last wins
        }
    }
    if (!answerLine.empty()) {
        json parsed = json::parse(answerLine, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("status")) {
            res.kind = RunKind::Answered;
            res.answer = std::move(parsed);
            return res;
        }
    }

    res.kind = RunKind::Silent;
    std::string exitText;
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        const char* name = strsignal(sig);
        exitText = std::string("exited on signal ") + std::to_string(sig) +
                   (name ? std::string(" (") + name + ")" : "");
    } else {
        exitText = "exited with code " + std::to_string(WEXITSTATUS(status));
    }
    std::string errTail = tail(errText, 2000);
    res.detail = tgt.name + " " + exitText + " without answering the driver" +
                 (errTail.empty() ? "" : "; stderr: " + errTail);
    return res;
}

// ---------------------------------------------------------------------------
// Reproducer compile.
// ---------------------------------------------------------------------------

bool compile_repro(const Inventory& inv, const Driver& drv, const Options& opt,
                   const std::string& stem, const std::string& script, std::string& binaryOut,
                   std::string& error) {
    std::string src = drv.dir + "/build/" + stem + ".cpp";
    std::string bin = drv.dir + "/build/" + stem;
    // The stem hashes the script, so an existing binary is this exact
    // script's successful build; rebuilding it would only burn the caller's
    // deadline.
    if (fs::exists(bin) && fs::exists(src)) {
        binaryOut = bin;
        return true;
    }
    if (!write_file(src, script)) {
        error = "cannot write " + src;
        return false;
    }
    StagedBuild sb;
    sb.root = inv.root;
    sb.common = {opt.cxx, "-std=" + opt.std, "-g", "-fno-omit-frame-pointer"};
    for (const auto& s : opt.sanitize) sb.common.push_back("-fsanitize=" + s);
    for (const auto& f : opt.cxxFlags) sb.common.push_back(f);
    if (!vendor_include().empty()) sb.common.push_back("-I" + vendor_include());
    sb.common.push_back("-I" + inv.root);
    std::vector<std::tuple<std::string, std::string, const char*>> units;
    units.emplace_back(src, src + ".o", "-O0");
    for (std::size_t i = 0; i < opt.sources.size(); ++i)
        units.emplace_back(opt.sources[i],
                           drv.dir + "/build/" + stem + "-src" + std::to_string(i) + ".o", "-O1");
    bool ok = sb.compile_all(units, std::thread::hardware_concurrency());
    std::vector<std::string> objs;
    if (ok)
        for (const auto& u : units) objs.push_back(std::get<1>(u));
    if (ok) ok = sb.link(objs, bin, opt, false);
    std::string output = sb.output;
    int code = ok ? 0 : (sb.code != 0 ? sb.code : 1);
    if (code != 0) {
        ::unlink(src.c_str());
        error = "the reproducer does not compile:\n" + tail(output, 4000);
        return false;
    }
    binaryOut = bin;
    return true;
}

std::string find_llvm_tool(const std::string& tool, const std::string& llvmBin) {
    auto usable = [](const std::string& p) { return !p.empty() && access(p.c_str(), X_OK) == 0; };
    if (!llvmBin.empty()) {
        std::string p = llvmBin + "/" + tool;
        if (usable(p)) return p;
    }
    if (const char* env = ::getenv("AFZ_LLVM_BIN")) {
        std::string p = std::string(env) + "/" + tool;
        if (usable(p)) return p;
    }
    if (const char* path = ::getenv("PATH")) {
        std::string_view sv(path);
        std::size_t start = 0;
        while (start <= sv.size()) {
            std::size_t colon = sv.find(':', start);
            std::string dir(sv.substr(start, colon == std::string_view::npos
                                                ? std::string_view::npos
                                                : colon - start));
            if (!dir.empty() && usable(dir + "/" + tool)) return dir + "/" + tool;
            if (colon == std::string_view::npos) break;
            start = colon + 1;
        }
    }
    return "";
}

std::string summarize(const Tgt& tgt, const json& values) {
    std::string out = tgt.name + "(";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        std::string s = afz_dump(values[i]);
        if (s.size() > 40) s = s.substr(0, 40) + "...";
        out += s;
    }
    return out + ")";
}

} // namespace afzh

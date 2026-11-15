// sha256 against its reference vectors, and the targetId derivation built on
// it: stable across calls, sensitive to every input field, 12 hex digits.
#include "discover.h"
#include "sha256.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string hex32(const std::uint8_t* d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) {
        s += digits[d[i] >> 4];
        s += digits[d[i] & 15];
    }
    return s;
}

std::string sha(const std::string& s) {
    afzh::Sha256 h;
    h.update(s);
    std::uint8_t d[32];
    h.finish(d);
    return hex32(d);
}

} // namespace

int main() {
    check(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256(\"abc\")");
    check(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256(\"\")");
    check(sha(std::string(1000000, 'a')) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256(1e6 x 'a')");
    const std::string a = afzh::target_id("src/a.cpp", "c:@F@f#I#", "int,int");
    check(a.rfind("afz:t:", 0) == 0 && a.size() == 18, "targetId shape");
    check(a == afzh::target_id("src/a.cpp", "c:@F@f#I#", "int,int"), "targetId is stable");
    check(a != afzh::target_id("src/b.cpp", "c:@F@f#I#", "int,int"), "file changes the id");
    check(a != afzh::target_id("src/a.cpp", "c:@F@f#J#", "int,int"), "usr changes the id");
    check(a != afzh::target_id("src/a.cpp", "c:@F@f#I#", "int,float"), "signature changes the id");
    // Separator injection must not collide: the NUL joins keep (ab,c) distinct
    // from (a,bc).
    check(afzh::target_id("a", "b", "c") != afzh::target_id("a\0b", "c", ""),
          "NUL joins prevent field collisions");
    for (char c : a.substr(6))
        check(std::strchr("0123456789abcdef", c) != nullptr, "id suffix is hex");

    return failures == 0 ? 0 : 1;
}

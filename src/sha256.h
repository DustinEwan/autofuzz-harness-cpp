// SHA-256 over a byte sequence, for target ids, edge ids, and build stamps.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace afzh {

struct Sha256 {
    void update(const void* data, std::size_t len);
    void update(std::string_view text);
    // 32 raw bytes.
    void finish(std::uint8_t out[32]);

    Sha256() { reset(); }
    void reset();

private:
    void block(const std::uint8_t* p);

    std::uint32_t state_[8];
    std::uint64_t bits_ = 0;
    std::uint8_t buf_[64];
    std::size_t buflen_ = 0;
};

// Lowercase hex of the whole digest.
std::string sha256_hex(std::string_view text);

} // namespace afzh

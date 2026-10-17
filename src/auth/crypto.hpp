#pragma once

#include <cstddef>
#include <string>

namespace mtfs::crypto {

constexpr std::size_t kSaltBytes = 16;

// Hex-encoded bytes from OpenSSL's cryptographically secure RNG.
std::string randomHex(std::size_t bytes);

std::string sha256Hex(const std::string& data);

// SHA-256(salt || password), hex-encoded. `saltHex` is the hex-encoded salt.
// The salt makes equal passwords hash differently, so precomputed tables
// (rainbow tables) and "same hash, same password" comparisons stop working.
std::string hashPassword(const std::string& saltHex, const std::string& password);

// Comparison whose running time depends only on the length, not on where
// the strings first differ, so timing does not leak how much of a hash matched.
bool constantTimeEquals(const std::string& a, const std::string& b);

std::string toHex(const unsigned char* data, std::size_t n);
bool fromHex(const std::string& hex, std::string& out);

}  // namespace mtfs::crypto

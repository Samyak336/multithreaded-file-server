#include "auth/crypto.hpp"

#include <stdexcept>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace mtfs::crypto {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string toHex(const unsigned char* data, std::size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0f];
    }
    return out;
}

bool fromHex(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) return false;
    std::string bytes(hex.size() / 2, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        int hi = hexValue(hex[2 * i]);
        int lo = hexValue(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = static_cast<char>((hi << 4) | lo);
    }
    out = std::move(bytes);
    return true;
}

std::string randomHex(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1)
        throw std::runtime_error("RAND_bytes failed: the random generator is not seeded");
    return toHex(buf.data(), buf.size());
}

std::string sha256Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    // EVP is OpenSSL's algorithm-independent digest API; the older SHA256_*
    // functions are deprecated since OpenSSL 3.0.
    if (EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 computation failed");
    return toHex(digest, length);
}

std::string hashPassword(const std::string& saltHex, const std::string& password) {
    std::string salt;
    if (!fromHex(saltHex, salt)) throw std::invalid_argument("salt is not valid hex");
    return sha256Hex(salt + password);
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace mtfs::crypto

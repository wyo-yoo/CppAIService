#include "security/Password.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace {
constexpr int iterations = 600000;
const std::string prefix = "pbkdf2_sha256$600000$";
std::string hex(const unsigned char* bytes, size_t size) {
    const char* digits = "0123456789abcdef";
    std::string result; result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15];
    }
    return result;
}
bool unhex(const std::string& text, unsigned char* result, size_t size) {
    if (text.size() != size * 2) return false;
    const auto value = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < size; ++i) {
        int a = value(text[2*i]), b = value(text[2*i+1]);
        if (a < 0 || b < 0) return false;
        result[i] = static_cast<unsigned char>((a << 4) | b);
    }
    return true;
}
std::array<unsigned char, 32> derive(const std::string& password, const unsigned char* salt) {
    std::array<unsigned char, 32> key{};
    if (password.size() > 1024 || PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
            salt, 16, iterations, EVP_sha256(), key.size(), key.data()) != 1)
        throw std::runtime_error("Password derivation failed");
    return key;
}
}

std::string Password::randomToken() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("Secure random generator failed");
    return hex(bytes.data(), bytes.size());
}
bool Password::isEncoded(const std::string& stored) {
    return stored.compare(0, 14, "pbkdf2_sha256$") == 0;
}
std::string Password::hash(const std::string& password) {
    std::array<unsigned char, 16> salt{};
    if (RAND_bytes(salt.data(), salt.size()) != 1) throw std::runtime_error("Secure random generator failed");
    auto key = derive(password, salt.data());
    auto result = prefix + hex(salt.data(), salt.size()) + "$" + hex(key.data(), key.size());
    OPENSSL_cleanse(key.data(), key.size());
    return result;
}
bool Password::verify(const std::string& password, const std::string& stored) {
    std::array<unsigned char, 16> salt{};
    std::array<unsigned char, 32> expected{};
    if (stored.size() != prefix.size() + 32 + 1 + 64 || stored.compare(0, prefix.size(), prefix) != 0 ||
        stored[prefix.size()+32] != '$' ||
        !unhex(stored.substr(prefix.size(), 32), salt.data(), salt.size()) ||
        !unhex(stored.substr(prefix.size()+33), expected.data(), expected.size())) return false;
    auto key = derive(password, salt.data());
    const bool equal = CRYPTO_memcmp(key.data(), expected.data(), key.size()) == 0;
    OPENSSL_cleanse(key.data(), key.size());
    return equal;
}
bool Password::validNew(const std::string& password) {
    // JSON parsing validates UTF-8. Count code points, allowing long passphrases.
    auto characters = std::count_if(password.begin(), password.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    return password.size() <= 512 && characters >= 15 && characters <= 128 && password.find('\0') == std::string::npos;
}

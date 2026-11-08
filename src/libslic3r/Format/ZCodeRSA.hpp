#pragma once
// Fixed, published device protocol key ported from local UVtools ZCodeFile.cs,
// AGPLv3. Raw 512-bit RSA with type-1 padding is framing, not secure encryption.
// No user credential, external crypto process or network is involved.
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include <string>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <vector>

namespace Slic3r::sla::zcode {
using Integer = boost::multiprecision::cpp_int;
struct ProtocolKey {
    const Integer n{"0xe819c5de84fd029f8ec9916ca74d9946d4e96cc500f7a3e972d4d77afd5c3c27252eea305d635addfd890693db3ebccc"
                    "c0b6a6dc94903771e3bd87d2571edeeb"};
    const Integer e{"0x10001"};
    const Integer p{"0xf6aa7df91c18bdcbd0517953d9ba8a228decc6eecb40533854875d43548c561b"};
    const Integer q{"0xf0e22ef82ea176f39233efbbf0d4c5c4d3a2c728a04105adc08abd5034186771"};
    const Integer dp{"0x61f3b84cb93c95fa1ec28be4a15cb3481f85fc458d8f00bd28cc0c5c1572192b"};
    const Integer dq{"0xe2b31b9d3ad57ecb70b72c51e62f7908ea5c71aecc53257306980bdf25882a21"};
    const Integer q_inverse{"0xa06dcd8799078e844e80d2a5e625acc422b525af8e03469b0fbc681fc760422b"};
};
inline const ProtocolKey &key() {
    static const ProtocolKey k;
    return k;
}
inline std::array<uint8_t, 64> encrypt_line(const std::string &line) {
    // Original format accepts at most 61 UTF-8 bytes. Do not replace its
    // exact padding with library defaults that impose a different length.
    if (line.empty() || line.size() > 61 || line.find_first_of("\r\n") != std::string::npos)
        throw ExportError("ZCode protocol line must contain 1..61 bytes without line breaks");
    std::array<uint8_t, 64> padded{};
    padded[1] = 1;
    const size_t separator = 63 - line.size();
    std::fill(padded.begin() + 2, padded.begin() + separator, uint8_t(255));
    std::copy(line.begin(), line.end(), padded.begin() + separator + 1);
    Integer message = 0;
    for (uint8_t b : padded)
        message = (message << 8) | b;
    const auto &k = key();
    // CRT reduces exponentiation to two 256-bit moduli. Local intermediates
    // and an immutable shared key allow deterministic parallel export.
    const Integer m1 = boost::multiprecision::powm(Integer(message % k.p), k.dp, k.p);
    const Integer m2 = boost::multiprecision::powm(Integer(message % k.q), k.dq, k.q);
    Integer h = ((m1 - m2) * k.q_inverse) % k.p;
    if (h < 0)
        h += k.p;
    Integer encoded = m2 + k.q * h;
    std::array<uint8_t, 64> result{};
    for (size_t i = result.size(); i > 0; --i) {
        result[i - 1] = uint8_t((encoded & 255).convert_to<unsigned>());
        encoded >>= 8;
    }
    return result;
}
inline std::string base64(const std::array<uint8_t, 64> &bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(88);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned v = unsigned(bytes[i]) << 16 | (i + 1 < bytes.size() ? unsigned(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? unsigned(bytes[i + 2]) : 0);
        result += alphabet[(v >> 18) & 63];
        result += alphabet[(v >> 12) & 63];
        result += i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? alphabet[v & 63] : '=';
    }
    return result;
}
inline std::string encrypt_lines(const std::vector<std::string> &lines) {
    // Fixed output slots preserve line order across worker counts.
    std::vector<std::string> encoded(lines.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, lines.size(), 64), [&](const auto &range) {
        for (size_t i = range.begin(); i < range.end(); ++i)
            encoded[i] = base64(encrypt_line(lines[i]));
    });
    std::string result;
    result.reserve(lines.size() * 89);
    for (const auto &line : encoded) {
        result += line;
        result += '\n';
    }
    return result;
}
} // namespace Slic3r::sla::zcode

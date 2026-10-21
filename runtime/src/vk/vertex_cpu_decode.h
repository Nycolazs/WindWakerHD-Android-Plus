// The CPU side of vertex_cpu_policy.h: the byte swap and format conversion of one vertex attribute,
// giving four 32-bit values, exactly the shader decoder's result (attrDecoder in Cemu's
// LatteDecompilerEmitGLSLAttrDecoder.cpp, MPL-2.0, which this mirrors case by case). Float results
// (nfa 0 and 2) are fed as R32G32B32A32_SFLOAT, integer results (nfa 1) as R32G32B32A32_UINT; the
// shader then only copies its input. Ported from the original project (vertex_cpu_decode.h).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <volk.h>
#include "Cafe/HW/Latte/Core/FetchShader.h"

namespace gfx {
// a float result of the shader decoder (fed as SFLOAT), else integer bits (fed as UINT)
inline bool cpu_vertex_float(const LatteParsedFetchShaderAttribute& a) { return a.nfa != 1; }
inline VkFormat cpu_vertex_format(const LatteParsedFetchShaderAttribute& a) {
    return cpu_vertex_float(a) ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32A32_UINT;
}

namespace cpu_decode_detail {
inline uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline uint32_t bswap32(uint32_t x) { return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24); }
inline uint32_t bswap16(uint32_t x) { return ((x >> 8) & 0xFF) | ((x << 8) & 0xFF00); }
inline uint32_t sext8(uint32_t x) { return (x & 0x80) ? x | 0xFFFFFF00u : x; }
inline uint32_t sext16(uint32_t x) { return (x & 0x8000) ? x | 0xFFFF0000u : x; }
inline uint32_t sext10(uint32_t x) { return (x & 0x200) ? x | 0xFFFFFC00u : x; }
// max(float(int(x))/d, -1.0)
inline uint32_t snorm(uint32_t x, float d) { float f = float(int32_t(x)) / d; return f2u(f < -1.0f ? -1.0f : f); }
inline uint32_t half_bits(uint32_t h) {
    h &= 0xFFFF;
    const uint32_t sign = (h & 0x8000) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0) {
        if (!mantissa) return sign;
        return sign | f2u(std::ldexp(float(mantissa), -24));  // subnormal half
    }
    if (exponent == 31) return sign | 0x7F800000u | (mantissa << 13);
    return sign | ((exponent + 112) << 23) | (mantissa << 13);
}
// components and their size of the raw uint fetch of the shader path (vertex_format, vk_formats.cpp)
inline bool layout(Latte::E_HWFMT format, uint32_t& n, uint32_t& width) {
    switch ((uint32_t)format) {
    case 0x01: n = 1; width = 1; return true;                         // 8
    case 0x02: case 0x07: n = 2; width = 1; return true;              // 4_4, 8_8
    case 0x05: case 0x06: n = 1; width = 2; return true;              // 16, 16_FLOAT
    case 0x0D: case 0x0E: n = 1; width = 4; return true;              // 32, 32_FLOAT
    case 0x0F: case 0x10: n = 2; width = 2; return true;              // 16_16(_FLOAT)
    case 0x19: case 0x1B: n = 1; width = 4; return true;              // 10_10_10_2, 2_10_10_10
    case 0x1A: n = 4; width = 1; return true;                         // 8_8_8_8
    case 0x1D: case 0x1E: n = 2; width = 4; return true;              // 32_32(_FLOAT)
    case 0x1F: case 0x20: n = 4; width = 2; return true;              // 16_16_16_16(_FLOAT)
    case 0x22: case 0x23: n = 4; width = 4; return true;              // 32_32_32_32(_FLOAT)
    case 0x2C: n = 3; width = 1; return true;                         // 8_8_8
    case 0x2D: case 0x2E: n = 3; width = 2; return true;              // 16_16_16(_FLOAT)
    case 0x2F: case 0x30: n = 3; width = 4; return true;              // 32_32_32(_FLOAT)
    default: return false;
    }
}
// that fetch: little-endian components, missing ones read as (0, 0, 0, 1)
inline void fetch(Latte::E_HWFMT format, const uint8_t* p, uint32_t v[4]) {
    v[0] = v[1] = v[2] = 0; v[3] = 1;
    uint32_t n, width;
    if (!layout(format, n, width)) return;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x = 0;
        std::memcpy(&x, p + i * width, width);  // little-endian host (all supported targets)
        v[i] = x;
    }
}
}  // namespace cpu_decode_detail

// bytes one attribute reads (0: a format without a fetch)
inline uint32_t cpu_vertex_bytes(const LatteParsedFetchShaderAttribute& a) {
    uint32_t n, width;
    return cpu_decode_detail::layout(a.format, n, width) ? n * width : 0;
}

// Fetch bytes of one attribute (p: vertex start + attribute offset); false: a combination the shader
// decoder does not handle either (out is zero).
inline bool cpu_decode_attribute(const LatteParsedFetchShaderAttribute& a, const uint8_t* p, uint32_t out[4]) {
    using namespace cpu_decode_detail;
    using F = Latte::E_HWFMT;
    using E = LatteConst::VertexFetchEndianMode;
    uint32_t s[4];
    fetch(a.format, p, s);
    uint32_t* d = out;
    auto set = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t w) { d[0] = x; d[1] = y; d[2] = z; d[3] = w; };
    const uint8_t nfa = a.nfa;
    const bool sig = a.isSigned != 0;
    if (a.endianSwap == E::SWAP_U32) {
        if (a.format == F::HWFMT_32_32_32_32_FLOAT && nfa == 2) set(bswap32(s[0]), bswap32(s[1]), bswap32(s[2]), bswap32(s[3]));
        else if (a.format == F::HWFMT_32_32_32_FLOAT && nfa == 2) set(bswap32(s[0]), bswap32(s[1]), bswap32(s[2]), 0);
        else if (a.format == F::HWFMT_32_32_FLOAT && nfa == 2) set(bswap32(s[0]), bswap32(s[1]), 0, 0);
        else if (a.format == F::HWFMT_32_FLOAT && nfa == 2) set(bswap32(s[0]), 0, 0, 0);
        else if (a.format == F::HWFMT_2_10_10_10 && nfa == 0) {
            const uint32_t x = bswap32(s[0]);
            uint32_t c[4] = {x & 0x3FF, (x >> 10) & 0x3FF, (x >> 20) & 0x3FF, (x >> 30) & 0x3};
            if (sig) for (int i = 0; i < 3; ++i) c[i] = snorm(sext10(c[i]), 511.0f);
            else for (int i = 0; i < 3; ++i) c[i] = snorm(c[i], 1023.0f);
            c[3] = f2u(float(c[3]) * (1.0f / 3.0f));
            set(c[0], c[1], c[2], c[3]);
        }
        else if (a.format == F::HWFMT_32_32_32_32 && nfa == 1 && !sig) set(bswap32(s[0]), bswap32(s[1]), bswap32(s[2]), bswap32(s[3]));
        else if (a.format == F::HWFMT_32_32_32 && nfa == 1 && !sig) set(bswap32(s[0]), bswap32(s[1]), bswap32(s[2]), 0);
        else if (a.format == F::HWFMT_32_32 && nfa == 1 && !sig) set(bswap32(s[0]), bswap32(s[1]), 0, 0);
        else if (a.format == F::HWFMT_32 && nfa == 1) set(bswap32(s[0]), 0, 0, 0);
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 0 && !sig)
            set(f2u(float(s[3]) / 255.0f), f2u(float(s[2]) / 255.0f), f2u(float(s[1]) / 255.0f), f2u(float(s[0]) / 255.0f));
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 0 && sig)
            set(snorm(sext8(s[3]), 127.0f), snorm(sext8(s[2]), 127.0f), snorm(sext8(s[1]), 127.0f), snorm(sext8(s[0]), 127.0f));
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 1 && !sig) set(s[3], s[2], s[1], s[0]);
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 2 && !sig)
            set(f2u(float(s[3])), f2u(float(s[2])), f2u(float(s[1])), f2u(float(s[0])));
        else { set(0, 0, 0, 0); return false; }
        return true;
    }
    if (a.endianSwap == E::SWAP_NONE) {
        if (a.format == F::HWFMT_32_32_32_32_FLOAT && nfa == 2) set(s[0], s[1], s[2], s[3]);
        else if (a.format == F::HWFMT_32_32_32_FLOAT && nfa == 2) set(s[0], s[1], s[2], 0);
        else if (a.format == F::HWFMT_32_32_FLOAT && nfa == 2) set(s[0], s[1], 0, 0);
        else if (a.format == F::HWFMT_32 && nfa == 1 && !sig) set(s[0], 0, 0, 0);
        else if (a.format == F::HWFMT_2_10_10_10 && nfa == 0 && !sig) {
            const uint32_t x = s[0];
            set(snorm(x & 0x3FF, 1023.0f), snorm((x >> 10) & 0x3FF, 1023.0f), snorm((x >> 20) & 0x3FF, 1023.0f),
                f2u(float((x >> 30) & 0x3) * (1.0f / 3.0f)));
        }
        else if (a.format == F::HWFMT_16_16_16_16 && nfa == 0 && sig)
            set(snorm(sext16(s[0]), 32767.0f), snorm(sext16(s[1]), 32767.0f), snorm(sext16(s[2]), 32767.0f), snorm(sext16(s[3]), 32767.0f));
        else if (a.format == F::HWFMT_16_16_16_16 && nfa == 2 && sig)
            set(f2u(float(int32_t(sext16(s[0])))), f2u(float(int32_t(sext16(s[1])))), f2u(float(int32_t(sext16(s[2])))), f2u(float(int32_t(sext16(s[3])))));
        else if (a.format == F::HWFMT_16_16_16_16_FLOAT && nfa == 2) set(half_bits(s[0]), half_bits(s[1]), half_bits(s[2]), half_bits(s[3]));
        else if (a.format == F::HWFMT_16_16 && nfa == 0 && sig) set(snorm(sext16(s[0]), 32767.0f), snorm(sext16(s[1]), 32767.0f), 0, 0);
        else if (a.format == F::HWFMT_16_16_FLOAT && nfa == 2) set(half_bits(s[0]), half_bits(s[1]), 0, 0);
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 0 && !sig)
            set(f2u(float(s[0]) / 255.0f), f2u(float(s[1]) / 255.0f), f2u(float(s[2]) / 255.0f), f2u(float(s[3]) / 255.0f));
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 0 && sig)
            set(snorm(sext8(s[0]), 127.0f), snorm(sext8(s[1]), 127.0f), snorm(sext8(s[2]), 127.0f), snorm(sext8(s[3]), 127.0f));
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 1 && !sig) set(s[0], s[1], s[2], s[3]);
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 1 && sig) set(sext8(s[0]), sext8(s[1]), sext8(s[2]), sext8(s[3]));
        else if (a.format == F::HWFMT_8_8_8_8 && nfa == 2 && !sig)
            set(f2u(float(s[0])), f2u(float(s[1])), f2u(float(s[2])), f2u(float(s[3])));
        else if (a.format == F::HWFMT_8_8 && nfa == 0 && !sig) set(f2u(float(s[0]) / 255.0f), f2u(float(s[1]) / 255.0f), 0, 0);
        else if (a.format == F::HWFMT_8_8 && nfa == 2 && !sig) set(f2u(float(s[0])), f2u(float(s[1])), 0, 0);
        else if (a.format == F::HWFMT_8_8 && nfa == 0 && sig) set(snorm(sext8(s[0]), 127.0f), snorm(sext8(s[1]), 127.0f), 0, 0);
        else if (a.format == F::HWFMT_8_8 && nfa == 1 && !sig) set(s[0], s[1], 0, 0);
        else if (a.format == F::HWFMT_8 && nfa == 0 && !sig) set(f2u(float(s[0]) / 255.0f), 0, 0, 0);
        else if (a.format == F::HWFMT_8 && nfa == 1 && !sig) set(s[0], 0, 0, 0);
        else { set(0, 0, 0, 0); return false; }
        return true;
    }
    if (a.endianSwap == E::SWAP_U16) {
        const uint32_t x = bswap16(s[0]), y = bswap16(s[1]), z = bswap16(s[2]), w = bswap16(s[3]);
        if (a.format == F::HWFMT_16_16_16_16_FLOAT && nfa == 2) set(half_bits(x), half_bits(y), half_bits(z), half_bits(w));
        else if (a.format == F::HWFMT_16_16_16_16 && nfa == 0 && sig)
            set(snorm(sext16(x), 32767.0f), snorm(sext16(y), 32767.0f), snorm(sext16(z), 32767.0f), snorm(sext16(w), 32767.0f));
        else if (a.format == F::HWFMT_16_16_16_16 && nfa == 0 && !sig)
            set(f2u(float(int32_t(x)) / 65535.0f), f2u(float(int32_t(y)) / 65535.0f), f2u(float(int32_t(z)) / 65535.0f), f2u(float(int32_t(w)) / 65535.0f));
        else if (a.format == F::HWFMT_16_16_16_16 && nfa == 2 && sig)
            set(f2u(float(int32_t(sext16(x)))), f2u(float(int32_t(sext16(y)))), f2u(float(int32_t(sext16(z)))), f2u(float(int32_t(sext16(w)))));
        else if (a.format == F::HWFMT_16_16_16_16 && nfa == 1 && sig) set(sext16(x), sext16(y), sext16(z), sext16(w));
        else if (a.format == F::HWFMT_16_16_16_16 && nfa == 1 && !sig) set(x, y, z, w);
        else if (a.format == F::HWFMT_16_16_FLOAT && nfa == 2) set(half_bits(x), half_bits(y), 0, 0);
        else if (a.format == F::HWFMT_16_16 && nfa == 0 && !sig) set(f2u(float(x) / 65535.0f), f2u(float(y) / 65535.0f), 0, 0);
        else if (a.format == F::HWFMT_16_16 && nfa == 0 && sig) set(snorm(sext16(x), 32767.0f), snorm(sext16(y), 32767.0f), 0, 0);
        else if (a.format == F::HWFMT_16_16 && nfa == 1 && !sig) set(x, y, 0, 0);
        else if (a.format == F::HWFMT_16_16 && nfa == 1 && sig) set(sext16(x), sext16(y), 0, 0);
        else if (a.format == F::HWFMT_16_16 && nfa == 2 && !sig) set(f2u(float(x)), f2u(float(y)), 0, 0);
        else if (a.format == F::HWFMT_16_16 && nfa == 2 && sig) set(f2u(float(int32_t(sext16(x)))), f2u(float(int32_t(sext16(y)))), 0, 0);
        else if (a.format == F::HWFMT_16 && nfa == 1 && !sig) set(x, 0, 0, 0);
        else if (a.format == F::HWFMT_16 && nfa == 0 && !sig) set(f2u(float(int32_t(x)) / 65535.0f), 0, 0, 0);
        else { set(0, 0, 0, 0); return false; }
        return true;
    }
    set(0, 0, 0, 0);
    return false;
}
}  // namespace gfx

// Area-sampled taps for the game's separable Gaussian blur on upscaled render targets (original
// project ea45a3d, issue #66). The game blurs its light buffer (ambient occlusion in R, the shadow
// mask in G) and its bloom chain with PS 44B3AB00 / 44B39A00: three bilinear taps 1.36 guest texels
// apart. At the console's size they cover five texels and the occlusion pass's 4x4 noise averages
// out; on a target scaled by the internal resolution the pixels between the taps get no weight and
// a fine grid shows around everything standing on the ground. Each tap of these two shaders averages
// bilinear samples spread over one guest texel instead, each sample covering about 2x2 image texels:
// n = max(2, ceil(scale / 2)) per axis above 1x (4 reads at 2x-4x; the original project's ceil(scale)^2
// was 9 at 3x and cost a lot of GPU time). At scale 1 it is the game's one sample.
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

namespace vk_area_sample {

// Cemu's program hash: the program's words, wherever it was loaded
inline uint64_t program_hash(const void* bytes, uint32_t size) {
    uint64_t a = 0, b = 0;
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t word;
        std::memcpy(&word, static_cast<const uint8_t*>(bytes) + i * 4, 4);
        a = std::rotl(a + word, 3);
        b = std::rotr(b ^ word, 7);
    }
    return a + b;
}

// texture units of this pixel shader that are read area-sampled (bit n = unit n); 0 for all others
inline uint32_t units_for_pixel_shader(const void* bytes, uint32_t size) {
    if (!bytes || size != 448) return 0;
    switch (program_hash(bytes, size)) {
    case 0x51a7ccdf69184627ull:  // PS 44B3AB00, horizontal Gaussian
    case 0x16285301a96cbf8dull:  // PS 44B39A00, vertical Gaussian
        return 1u;
    default:
        return 0;
    }
}

// Rewrites the sample calls of `units` in a decompiled Vulkan GLSL pixel shader to the helper and
// inserts the helper. Returns the number of calls rewritten (0: source left unchanged).
inline int rewrite(std::string& src, uint32_t units) {
    const size_t entry = src.find("void main(");
    if (!units || entry == std::string::npos) return 0;
    int count = 0;
    for (uint32_t unit = 0; unit < 32; unit++) {
        if (!(units >> unit & 1)) continue;
        const std::string n = std::to_string(unit);
        const std::string from = "texture(textureUnitPS" + n + ", ";
        const std::string to = "wwhdAreaSample(textureUnitPS" + n + ", uf_tex" + n + "Scale, ";
        for (size_t at = src.find(from); at != std::string::npos; at = src.find(from, at + to.size())) {
            src.replace(at, from.size(), to);
            count++;
        }
    }
    if (!count) return 0;
    static const char* const kGlsl =
        "// area-sampled tap for upscaled render targets (runtime/src/vk/area_sample.h)\n"
        "vec4 wwhdAreaSample(sampler2D t, vec2 scale, vec2 uv) {\n"
        "    if (scale.x <= 1.001 && scale.y <= 1.001) return texture(t, uv);\n"
        "    vec2 k = max(ceil(scale * 0.5 - 0.001), vec2(2.0));\n"
        "    vec2 step = scale / (vec2(textureSize(t, 0)) * k);\n"
        "    vec4 sum = vec4(0.0);\n"
        "    for (float j = 0.5; j < k.y; j += 1.0)\n"
        "        for (float i = 0.5; i < k.x; i += 1.0) sum += texture(t, uv + (vec2(i, j) - 0.5 * k) * step);\n"
        "    return sum / (k.x * k.y);\n"
        "}\n";
    src.insert(src.find("void main("), kGlsl);
    return count;
}

}  // namespace vk_area_sample

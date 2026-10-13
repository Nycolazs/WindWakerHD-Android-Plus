// 60 fps (generated): the game runs at its own 30 frames a second, and after each real frame an
// in-between one is made on the GPU and shown half a frame later (ZeldaWWHDRecomp's Android work).
// The in-between frame is the real one reprojected with the scene's depth to the camera moved on by
// half its last step (eye and turn: the camera orbits Link, so a turn alone would move Link
// sideways). The HUD stays where it is: at the first HUD layout drawn on the TV picture the scene
// (3D and its post-processing) is copied aside, and pixels where the finished picture differs from it
// are HUD. The frame interpolation mode draws every frame twice on the game thread; this costs one GPU
// pass instead, so it holds 60 where the game thread can't (Outset, Forest Haven while moving).
// It runs in the frame generation path (vk_device.cpp present_frame_fg): the window picture is
// composed as usual, record() copies it and makes the in-between one, the present thread shows both.
#include "reproj.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "runtime.h"

namespace gfx::rp {
namespace {

const char* kVertex = R"glsl(#version 450
layout(location=0) out vec2 uv;
void main() {
 vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
 uv = p;
 gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

// window picture uv -> the generated window picture. p.rect: where the TV picture lies in the window
// (uv); t: tanX, tanY, HUD difference threshold, unused; z: near, far, depth usable, unused;
// r0..r2: rows of H (the generated camera's axes in the real camera's space), .w: its eye there
const char* kWarp = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D comp;      // the window picture (display encoded)
layout(set=0,binding=1) uniform sampler2D depthImg;  // the scene's depth (TV picture space)
layout(set=0,binding=2) uniform sampler2D hudMask;   // where the HUD is, a quarter of the TV picture's size (kHudMask)
layout(set=0,binding=3) uniform sampler2D unused3;
layout(set=0,binding=4) uniform sampler2D flowImg;   // objects' own movement per 16x16 block (kFlow)
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
// the nearest depth of the 2x2 pixels around q: the game's outlines around characters lie on the
// background's depth and would otherwise move with the background (depth range [-1,1] as GX2)
float viewDepth(vec2 q) {
 vec4 m = textureGather(depthImg, q);
 float d = min(min(m.x, m.y), min(m.z, m.w)), n = p.z.x, f = p.z.y;
 return 2.0 * n * f / max((f + n) - (d * 2.0 - 1.0) * (f - n), 1e-6);
}
// how much the pixel at q moves with the camera, 0..1: what lies about as far as the point the
// camera looks at (a.x; a.y: on). The camera follows Link (and the title's boat): they and what is
// near them keep their place on the screen while the camera moves, so moving them with the camera's
// movement shook them back and forth every frame; the background, further away, is reprojected.
// (z: the view depth at q, read once per step of the motion)
float follows(float z) {
 if (p.a.y < 0.5 || p.a.x <= 0.0) return 0.0;
 float r = abs(log(max(z, 1e-3) / p.a.x));
 return 1.0 - smoothstep(0.35, 0.75, r);
}
// where the real picture's pixel q (TV uv), at view depth z, is in the generated picture
vec2 toGenCam(vec2 q, float z);
vec2 toGen(vec2 q, float z, float fw) { return mix(toGenCam(q, z), q, fw); }
vec2 toGenCam(vec2 q, float z) {
 vec3 P = vec3((q.x * 2.0 - 1.0) * p.t.x, (1.0 - q.y * 2.0) * p.t.y, -1.0) * z - vec3(p.r0.w, p.r1.w, p.r2.w);
 vec3 g = p.r0.xyz * P.x + p.r1.xyz * P.y + p.r2.xyz * P.z;
 if (g.z > -1e-4) return q;
 return vec2(g.x / (-g.z) / p.t.x * 0.5 + 0.5, 0.5 - g.y / (-g.z) / p.t.y * 0.5);
}
// half the movement of the object at q beyond the camera's (TV uv), where q's depth is its block's
// (characters and enemies: with the camera's movement alone they stood still in the in-between frame,
// then jumped: 30 fps motion)
vec2 objectMove(vec2 q, float z, float fw) {
 if (p.z.w < 0.5) return vec2(0.0);
 vec4 f = textureLod(flowImg, q, 0.0);
 if (f.w < 0.5) return vec2(0.0);
 float bz = p.z.x * exp(f.z * log(p.z.y / p.z.x));
 if (abs(z - bz) > 0.15 * bz) return vec2(0.0);
 return (f.xy - 0.5) * (0.5 / 8.0) * (1.0 - fw);  // (what follows the camera already keeps its place)
}
// the HUD and a few pixels around it (glows, outlines and their soft edges): the mask's texels cover
// 4x4 TV pixels, its bilinear tap anything within one texel of them
bool hud(vec2 q) { return textureLod(hudMask, q, 0.0).r > 0.01; }
// the window picture's pixel nearest to q, unfiltered: bilinear resampling softened fine detail (water,
// grass, distant edges) in every other frame, which shimmered; the movement is rounded to whole pixels
vec4 window(vec2 q) {
 ivec2 sz = textureSize(comp, 0);
 return texelFetch(comp, clamp(ivec2((p.rect.xy + q * p.rect.zw) * vec2(sz)), ivec2(0), sz - 1), 0);
}
void main() {
 vec2 tv = (uv - p.rect.xy) / p.rect.zw;
 if (p.z.z < 0.5 || any(lessThan(tv, vec2(0.0))) || any(greaterThan(tv, vec2(1.0))) || hud(tv)) {
  result = textureLod(comp, uv, 0.0);  // outside the TV picture, without a depth buffer, or the HUD: unmoved
  return;
 }
 vec2 q = tv;  // the source pixel that lands here: a few fixed-point steps of the motion
 vec2 raw = tv;
 for (int i = 0; i < 2; i++) {
  float z = viewDepth(q), fw = follows(z);
  raw = tv - (toGen(q, z, fw) - q) - objectMove(q, z, fw);
  q = clamp(raw, vec2(0.0), vec2(1.0));
 }
 // it would come from outside the real picture (the edge while the camera turns): the real picture's
 // own pixel there (half a frame behind) rather than the edge column stretched across
 if (any(notEqual(q, raw))) q = tv;
 if (hud(q)) {  // the scene there is hidden under the HUD: the nearest visible scene along the motion
  vec2 px = 1.0 / vec2(textureSize(depthImg, 0));
  vec2 d = q - tv;
  float l = length(d / px);
  d = l > 0.5 ? d / l : vec2(px.x, 0.0);
  for (int k = 1; k <= 8; k++) {
   vec2 a = clamp(q + d * float(k * 12), vec2(0.0), vec2(1.0));
   if (!hud(a)) { q = a; break; }
   a = clamp(q - d * float(k * 12), vec2(0.0), vec2(1.0));
   if (!hud(a)) { q = a; break; }
  }
 }
 result = window(q);
 if (p.t.w > 0.5) result.rgb *= vec3(1.0, 0.6, 0.6);  // (debug: WWHD_FRAMEGEN_TINT=1) generated frames tinted
}
)glsl";

// Objects' own movement, one value per 16x16 block of the TV picture: the block of this frame's scene
// (quarter size, no HUD) is looked for in the previous one around where the camera's movement alone
// puts it (depth). Output: the movement beyond the camera's, previous -> this frame (uv * 8 + 0.5), the
// block's nearest depth (log scale), valid. Blocks with little detail or no clearly better match: 0.
// r0..r2: rows of D (this camera's axes in the previous camera's space), .w: this eye there
const char* kFlow = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D curImg;
layout(set=0,binding=1) uniform sampler2D prevImg;
layout(set=0,binding=2) uniform sampler2D depthImg;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; } p;
float lin(float d) {
 float n = p.z.x, f = p.z.y;
 return 2.0 * n * f / max((f + n) - (d * 2.0 - 1.0) * (f - n), 1e-6);
}
// 4x4 quarter-size pixels as four gathers of the green channel (brightness is enough to match blocks)
void block(sampler2D img, vec2 at, vec2 ts, out vec4 b[4]) {
 b[0] = textureGather(img, at + vec2(-1.0, -1.0) * ts, 1); b[1] = textureGather(img, at + vec2(1.0, -1.0) * ts, 1);
 b[2] = textureGather(img, at + vec2(-1.0, 1.0) * ts, 1);  b[3] = textureGather(img, at + vec2(1.0, 1.0) * ts, 1);
}
float sad(vec4 a[4], vec4 b[4]) {
 vec4 d = abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) + abs(a[3] - b[3]);
 return d.x + d.y + d.z + d.w;
}
void main() {
 result = vec4(0.5, 0.5, 0.0, 0.0);
 vec2 ts = 1.0 / vec2(textureSize(curImg, 0));
 vec4 cur[4]; block(curImg, uv, ts, cur);
 vec4 m4 = (cur[0] + cur[1] + cur[2] + cur[3]) * 0.25; float mean = (m4.x + m4.y + m4.z + m4.w) * 0.25;
 vec4 dv = abs(cur[0] - mean) + abs(cur[1] - mean) + abs(cur[2] - mean) + abs(cur[3] - mean);
 if (dv.x + dv.y + dv.z + dv.w < 0.25) return;  // little detail: no reliable match
 vec2 dt = 1.0 / vec2(textureSize(depthImg, 0));
 vec4 g = min(min(textureGather(depthImg, uv + vec2(-4.0, -4.0) * dt), textureGather(depthImg, uv + vec2(4.0, -4.0) * dt)),
              min(textureGather(depthImg, uv + vec2(-4.0, 4.0) * dt), textureGather(depthImg, uv + vec2(4.0, 4.0) * dt)));
 float z = lin(min(min(g.x, g.y), min(g.z, g.w)));
 vec3 P = vec3((uv.x * 2.0 - 1.0) * p.t.x, (1.0 - uv.y * 2.0) * p.t.y, -1.0) * z;
 vec3 q = vec3(dot(p.r0.xyz, P), dot(p.r1.xyz, P), dot(p.r2.xyz, P)) + vec3(p.r0.w, p.r1.w, p.r2.w);
 if (q.z > -1e-4) return;
 vec2 pp = vec2(q.x / (-q.z) / p.t.x * 0.5 + 0.5, 0.5 - q.y / (-q.z) / p.t.y * 0.5);
 if (any(lessThan(pp, vec2(0.02))) || any(greaterThan(pp, vec2(0.98)))) return;
 float best = 1e9, zero = 0.0; vec2 bo = vec2(0.0); vec4 b[4];
 for (int oy = -6; oy <= 6; oy += 2) for (int ox = -6; ox <= 6; ox += 2) {
  vec2 o = vec2(ox, oy); block(prevImg, pp + o * ts, ts, b); float sd = sad(cur, b);
  if (ox == 0 && oy == 0) zero = sd;
  if (sd < best) { best = sd; bo = o; }
 }
 vec2 c0 = bo;
 for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
  if (ox == 0 && oy == 0) continue;
  vec2 o = c0 + vec2(ox, oy); block(prevImg, pp + o * ts, ts, b); float sd = sad(cur, b);
  if (sd < best) { best = sd; bo = o; }
 }
 if (zero <= best * 2.0 + 0.1 || best > 1.2) bo = vec2(0.0);  // the camera's movement explains it, or no clear match
 vec2 v = -bo * ts;
 result = vec4(clamp(v * 8.0 + 0.5, 0.0, 1.0), clamp(log(z / p.z.x) / log(p.z.y / p.z.x), 0.0, 1.0), bo == vec2(0.0) ? 0.0 : 1.0);
}
)glsl";
// a block keeps its movement only when at least 3 of its 8 neighbours moved the same way, and takes
// their average; lone blocks (water, flicker) are dropped. t.x: the agreement distance in the encoding
const char* kFilter = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D flowIn;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; } p;
void main() {
 vec4 c = textureLod(flowIn, uv, 0.0);
 result = vec4(0.5, 0.5, c.z, 0.0);
 if (c.w < 0.5) return;
 vec2 ts = 1.0 / vec2(textureSize(flowIn, 0));
 vec2 sum = c.xy; float n = 0.0;
 for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
  if (i == 0 && j == 0) continue;
  vec4 o = textureLod(flowIn, uv + vec2(i, j) * ts, 0.0);
  if (o.w > 0.5 && all(lessThan(abs(o.xy - c.xy), vec2(p.t.x)))) { sum += o.xy; n += 1.0; }
 }
 if (n >= 3.0) result = vec4(sum / (n + 1.0), c.z, 1.0);
}
)glsl";
// where the HUD is (WWHD: the finished TV picture differs from the scene there), one texel per 4x4 TV
// pixels: 1 if any of them differs by more than t.z
const char* kHudMask = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D scanImg;
layout(set=0,binding=1) uniform sampler2D sceneImg;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
void main() {
 ivec2 sz = textureSize(scanImg, 0), b = ivec2(uv * vec2(sz) / 4.0) * 4;
 float m = 0.0;
 for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
  ivec2 c = min(b + ivec2(x, y), sz - 1);
  vec3 d = abs(texelFetch(scanImg, c, 0).rgb - texelFetch(sceneImg, c, 0).rgb);
  m = max(m, max(max(d.r, d.g), d.b));
 }
 result = vec4(m > p.t.z ? 1.0 : 0.0, 0.0, 0.0, 1.0);
}
)glsl";

// the scene at quarter size (a 4x4 average from 4 bilinear taps)
const char* kQuarter = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D src;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
void main() {
 vec2 t = 1.0 / vec2(textureSize(src, 0));
 result = 0.25 * (textureLod(src, uv + vec2(-t.x, -t.y), 0.0) + textureLod(src, uv + vec2(t.x, -t.y), 0.0) +
                  textureLod(src, uv + vec2(-t.x, t.y), 0.0) + textureLod(src, uv + vec2(t.x, t.y), 0.0));
}
)glsl";

// a small render target of the generation passes (RGBA8, GENERAL once drawn)
struct Target {
    Image img;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
};

struct Push { float r0[4], r1[4], r2[4], t[4], z[4], rect[4], a[4]; };

struct Cam {
    float m[9];  // camera-to-world: columns right, up, back (the camera looks along -z)
    float eye[3];
    float target = 0;  // distance from the eye to the point the camera looks at (Link, the title's boat)
    float fovy = 40, znear = 1, zfar = 100000;
    uint64_t frame = 0;
    bool valid = false;
};

struct State {
    bool enabled = false;
    std::mutex mu;
    Cam cur, prev;
    // the window-sized pictures handed to the present thread: real copies and generated ones (3 sets:
    // record()'s images stay unchanged until it was called twice more)
    Image real[3], gen[3];
    VkImageView realView[3]{}, genView[3]{};
    VkFramebuffer genFb[3]{};
    int next = 0;
    uint32_t w = 0, h = 0;
    // the TV picture before the HUD, and the scene's depth buffer, of this frame
    Image scene;
    uint64_t sceneFrame = ~0ull;
    uint32_t depthAddr = 0, depthW = 0, depthH = 0;
    uint64_t depthFrame = ~0ull;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;  // the warp
    std::vector<std::pair<const char*, VkPipeline>> pipelines;  // the other passes, by shader
    VkSampler nearest = VK_NULL_HANDLE;
    // objects' movement: the scene at quarter size (this frame's and the one before), per-block movement
    Target small[2], flowRaw, flow, hudMask;
    uint64_t smallFrame[2] = {~0ull, ~0ull};
    int smallCur = 0;
    uint64_t generated = 0, skipped = 0;
} S;

void basis(const float eye[3], const float center[3], const float upv[3], float m[9]) {
    float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6f) fl = 1;
    for (float& v : f) v /= fl;
    float r[3] = {f[1] * upv[2] - f[2] * upv[1], f[2] * upv[0] - f[0] * upv[2], f[0] * upv[1] - f[1] * upv[0]};
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) rl = 1;
    for (float& v : r) v /= rl;
    float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    for (int i = 0; i < 3; i++) {
        m[i * 3 + 0] = r[i];
        m[i * 3 + 1] = u[i];
        m[i * 3 + 2] = -f[i];
    }
}

VkShaderModule module(const char* src, bool vertex) {
    std::vector<uint32_t> spirv;
    std::string log;
    if (!compile_glsl(src, vertex, spirv, log)) {
        LOG("[framegen] shader failed: %s", log.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(R.device, &ci, nullptr, &m));
    return m;
}

VkPipeline create_pipeline(const char* src);
VkPipeline pipeline_for(const char* src);
bool make_pipeline() {
    if (S.pipeline) return true;
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_R8G8B8A8_UNORM;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // read by the present thread
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkSubpassDependency deps[2] = {
        // the present thread's earlier read of the image, before it is written again
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        // the sampled images were written by earlier passes and copies
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_SHADER_READ_BIT, 0}};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &a;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(R.device, &rp, nullptr, &S.pass));

    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 5;
    dci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(R.device, &dci, nullptr, &S.dsl));
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &S.dsl;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(R.device, &lci, nullptr, &S.layout));

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    VK_CHECK(vkCreateSampler(R.device, &si, nullptr, &S.nearest));

    S.pipeline = create_pipeline(kWarp);
    return S.pipeline != VK_NULL_HANDLE;
}

// a full-screen pass's pipeline (the shared render pass and layout) for fragment shader `src`
VkPipeline create_pipeline(const char* src) {
    VkShaderModule vs = module(kVertex, true), fs = module(src, false);
    if (!vs || !fs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = i ? fs : vs;
        stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = 1;
    bs.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &bs;
    ci.pDynamicState = &ds;
    ci.layout = S.layout;
    ci.renderPass = S.pass;
    VkPipeline pipe = VK_NULL_HANDLE;
    VK_CHECK(create_graphics_pipeline(&ci, &pipe));
    vkDestroyShaderModule(R.device, vs, nullptr);
    vkDestroyShaderModule(R.device, fs, nullptr);
    return pipe;
}

VkPipeline pipeline_for(const char* src) {
    for (auto& [k, v] : S.pipelines)
        if (k == src) return v;
    VkPipeline pl = create_pipeline(src);
    if (pl) S.pipelines.push_back({src, pl});
    return pl;
}

void release_images() {
    for (int i = 0; i < 3; i++) {
        if (S.genFb[i]) vkDestroyFramebuffer(R.device, S.genFb[i], nullptr);
        if (S.genView[i]) vkDestroyImageView(R.device, S.genView[i], nullptr);
        if (S.realView[i]) vkDestroyImageView(R.device, S.realView[i], nullptr);
        if (S.gen[i].image) vmaDestroyImage(R.vma, S.gen[i].image, S.gen[i].alloc);
        if (S.real[i].image) vmaDestroyImage(R.vma, S.real[i].image, S.real[i].alloc);
        S.genFb[i] = VK_NULL_HANDLE;
        S.genView[i] = S.realView[i] = VK_NULL_HANDLE;
        S.gen[i] = S.real[i] = Image{};
    }
    S.w = S.h = 0;
}

void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                   VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void release_target(Target& t) {
    if (t.fb) vkDestroyFramebuffer(R.device, t.fb, nullptr);
    if (t.view) vkDestroyImageView(R.device, t.view, nullptr);
    if (t.img.image) vmaDestroyImage(R.vma, t.img.image, t.img.alloc);
    t = Target{};
}

// a small RGBA8 target of w x h (recreated when the size changes; the GPU may still read the old one:
// it is released once the frame is done)
bool ensure_target(Target& t, uint32_t w, uint32_t h) {
    if (t.img.image && t.img.width == w && t.img.height == h) return true;
    if (t.img.image) {
        Target old = t;
        on_complete([old]() mutable { release_target(old); });
        t = Target{};
    }
    if (!create_image(t.img, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, false, false))
        return false;
    t.view = make_view(t.img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = S.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &t.view;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VK_CHECK(vkCreateFramebuffer(R.device, &fi, nullptr, &t.fb));
    return true;
}

struct Input {
    VkImageView view;
    VkSampler sampler;
    VkImageLayout layout;
};
// one full-screen pass of `pipe` into framebuffer `fb` (w x h); up to 5 sampled inputs
void run_pass(VkCommandBuffer cmd, VkPipeline pipe, VkFramebuffer fb, uint32_t w, uint32_t h, const Input* in, int n, const Push& pc) {
    VkDescriptorSet set = alloc_descriptor_set(S.dsl);
    VkDescriptorImageInfo ii[5];
    VkWriteDescriptorSet wr[5]{};
    for (int k = 0; k < 5; k++) {
        const Input& x = in[k < n ? k : 0];  // unused bindings get the first input
        ii[k] = {x.sampler, x.view, x.layout};
        wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[k].dstSet = set;
        wr[k].dstBinding = (uint32_t)k;
        wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(R.device, 5, wr, 0, nullptr);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = S.pass;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, S.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

// the scene's depth buffer of this frame (the TV picture's size), or null
Surface* scene_depth() {
    if (S.depthFrame != S.sceneFrame || !S.depthAddr) return nullptr;
    auto range = R.surfaces.equal_range(S.depthAddr);
    for (auto it = range.first; it != range.second; ++it) {
        Surface* s = it->second.get();
        if (s->isDepth && s->img.image && s->img.width == S.depthW && s->img.height == S.depthH) return s;
    }
    return nullptr;
}
}  // namespace

bool enabled() { return S.enabled; }
void set_enabled(bool on) {
    S.enabled = on;
    LOG("[framegen] built-in 60 fps generation %s", on ? "on" : "off");
}

// render thread (OP_FRAMEGEN_CAMERA): the 3D camera of the frame being drawn
void note_camera(const float eye[3], const float center[3], const float up[3], float fovy, float znear, float zfar) {
    if (!S.enabled) return;
    std::lock_guard<std::mutex> lk(S.mu);
    if (S.cur.frame != R.frame) S.prev = S.cur;  // the frame's last camera counts (others: 2D, menus)
    basis(eye, center, up, S.cur.m);
    std::memcpy(S.cur.eye, eye, sizeof S.cur.eye);
    S.cur.target = std::sqrt((center[0] - eye[0]) * (center[0] - eye[0]) + (center[1] - eye[1]) * (center[1] - eye[1]) +
                             (center[2] - eye[2]) * (center[2] - eye[2]));
    S.cur.fovy = fovy;
    S.cur.znear = znear;
    S.cur.zfar = zfar;
    S.cur.frame = R.frame;
    S.cur.valid = true;
    static int logged = 0;
    if (logged < 2) {
        logged++;
        LOG("[framegen] camera: fovy %.1f near %.2f far %.1f", fovy, znear, zfar);
    }
}

// render thread (OP_LAYOUT_ROOT): a layout is about to be drawn into the target bound now. At the first
// one on the TV picture this frame, the scene is complete: copy it aside, and note its depth buffer.
void layout_root(const uint32_t* regs, bool) {
    if (!S.enabled || S.sceneFrame == R.frame) return;
    Surface* color = color_target(regs, 0);
    if (!color || !color->img.image || color->img.type != VK_IMAGE_TYPE_2D) return;
    // the TV picture: 16:9 at the console's size (the GamePad's 854x480 family is not it)
    if (color->height < 360 || color->height == 480 || std::abs((float)color->width * 9.0f / ((float)color->height * 16.0f) - 1.0f) > 0.03f)
        return;
    S.sceneFrame = R.frame;
    Image& src = color->img;
    if (S.scene.image && (S.scene.width != src.width || S.scene.height != src.height || S.scene.format != src.format)) {
        retire_image(S.scene);
        S.scene = Image{};
    }
    if (!S.scene.image &&
        !create_image(S.scene, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, src.format, src.width, src.height, 1, 1, 1,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, false, false))
        return;
    prepare(src, Use::COPY_SRC);
    prepare(S.scene, Use::COPY_DST);
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {src.width, src.height, 1};
    vkCmdCopyImage(command_buffer(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.scene.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &r);
    // the scene's depth: the TV-sized depth buffer of its last pass (the HUD draws none)
    S.depthFrame = ~0ull;
    if (Surface* d = depth_target(regs))
        if (d->isDepth && d->img.image && d->img.width == src.width && d->img.height == src.height) {
            S.depthAddr = d->addr;
            S.depthW = d->img.width;
            S.depthH = d->img.height;
            S.depthFrame = R.frame;
        }
    if (S.depthFrame != R.frame && R.mainDepthAddr) {
        S.depthAddr = R.mainDepthAddr;
        S.depthW = src.width;
        S.depthH = src.height;
        S.depthFrame = R.frame;
    }
    static int logged = 0;
    if (logged < 2) {
        logged++;
        LOG("[framegen] scene %08X %ux%u format %d, depth %08X", color->addr, src.width, src.height, (int)src.format, S.depthAddr);
    }
}

bool resize(uint32_t w, uint32_t h) {
    release_images();
    for (int i = 0; i < 3; i++) {
        if (!create_image(S.real[i], VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, false, false) ||
            !create_image(S.gen[i], VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, false, false))
            return false;
        S.realView[i] = make_view(S.real[i], VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
        S.genView[i] = make_view(S.gen[i], VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    S.w = w;
    S.h = h;
    return true;
}

VkImageView view_of(const Image* img) {
    for (int i = 0; i < 3; i++) {
        if (img == &S.real[i]) return S.realView[i];
        if (img == &S.gen[i]) return S.genView[i];
    }
    return VK_NULL_HANDLE;
}

std::vector<const Image*> record(VkCommandBuffer cmd, const Image& comp, VkImageView compView, const float tvRect[4], bool generate) {
    std::vector<const Image*> out;
    if (!S.w) return out;
    if ((S.generated + S.skipped) % 600 == 599)
        LOG("[framegen] %llu in-between frames generated, %llu shown as the real one (no 3D scene or camera)",
            (unsigned long long)S.generated, (unsigned long long)S.skipped);
    const int i = S.next;
    S.next = (S.next + 1) % 3;
    // the real picture: a copy of the window picture (the composite is drawn again next frame)
    image_barrier(cmd, S.real[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    image_barrier(cmd, comp.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {S.w, S.h, 1};
    vkCmdCopyImage(cmd, comp.image, VK_IMAGE_LAYOUT_GENERAL, S.real[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    image_barrier(cmd, S.real[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    out.push_back(&S.real[i]);
    if (!generate || !make_pipeline()) return out;

    // the generated camera: half the last rotation (D = Cprev^T Ccur, D^(1/2)) and half the last
    // movement, in the real camera's space; none across a cut or without a camera of the last two frames
    float half[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, move[3] = {0, 0, 0};
    float full[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, fullMove[3] = {0, 0, 0};
    bool fullValid = false;
    float fovy, znear, zfar, target;
    // WWHD_FRAMEGEN_FOLLOW=0: everything moves with the camera's movement (as before)
    static const bool followOn = !getenv("WWHD_FRAMEGEN_FOLLOW") || atoi(getenv("WWHD_FRAMEGEN_FOLLOW")) != 0;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        target = S.cur.target;
        fovy = S.cur.fovy;
        znear = S.cur.znear;
        zfar = S.cur.zfar;
        // the camera the scene was drawn with and the one of the frame before (R.frame has moved on by
        // the time the frame is presented)
        const bool both = S.cur.valid && S.prev.valid && S.cur.frame == S.sceneFrame && S.prev.frame + 1 == S.cur.frame;
        float step[3] = {0, 0, 0}, dist = 0;
        if (both)
            for (int k = 0; k < 3; k++) {
                step[k] = S.cur.eye[k] - S.prev.eye[k];
                dist += step[k] * step[k];
            }
        if (both && std::sqrt(dist) < 400.0f) {
            for (int j = 0; j < 3; j++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += S.cur.m[k * 3 + j] * step[k];
                move[j] = 0.5f * v;
            }
            float d[9];
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) {
                    float s = 0;
                    for (int k = 0; k < 3; k++) s += S.prev.m[k * 3 + a] * S.cur.m[k * 3 + b];
                    d[a * 3 + b] = s;
                }
            // the flow pass: a point in this camera's space, in the previous camera's = D p + Cprev^T step
            std::memcpy(full, d, sizeof full);
            for (int j = 0; j < 3; j++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += S.prev.m[k * 3 + j] * step[k];
                fullMove[j] = v;
            }
            fullValid = true;
            const float tr = d[0] + d[4] + d[8];
            const float ang = std::acos(std::clamp((tr - 1) * 0.5f, -1.0f, 1.0f));
            if (ang > 1e-5f && ang < 0.5f) {
                float ax[3] = {d[7] - d[5], d[2] - d[6], d[3] - d[1]};
                const float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                if (l > 1e-8f) {
                    for (float& v : ax) v /= l;
                    const float a = ang * 0.5f, co = std::cos(a), si = std::sin(a), t = 1 - co;
                    const float x = ax[0], y = ax[1], z = ax[2];
                    const float rm[9] = {t * x * x + co,     t * x * y - si * z, t * x * z + si * y, t * x * y + si * z, t * y * y + co,
                                         t * y * z - si * x, t * x * z - si * y, t * y * z + si * x, t * z * z + co};
                    std::memcpy(half, rm, sizeof half);
                }
            } else if (ang >= 0.5f) {
                move[0] = move[1] = move[2] = 0;  // a cut
                fullValid = false;
            }
        }
    }
    Surface* depth = scene_depth();
    const bool sceneOk = S.sceneFrame != ~0ull && S.sceneFrame + 2 >= R.frame && S.scene.image && R.tv.img.image;
    const bool depthOk = depth && sceneOk && znear > 0 && zfar > znear;
    if (!depthOk) {  // no 3D scene this frame (menus, title, loading): the generated picture is the real one
        S.skipped++;
        if (S.skipped % 600 == 1)
            LOG("[framegen] not generated: scene frame %lld (now %llu), scene image %d, depth %p (frame %lld addr %08X), camera frame %llu, near %.2f far %.1f",
                (long long)S.sceneFrame, (unsigned long long)R.frame, S.scene.image != VK_NULL_HANDLE, (void*)depth, (long long)S.depthFrame,
                S.depthAddr, (unsigned long long)S.cur.frame, znear, zfar);
        out.push_back(&S.real[i]);
        return out;
    }
    // the TV picture's size relative to its own height: its projection's aspect (the game widens it)
    const float tanY = std::tan(fovy * 0.5f * 3.14159265f / 180.0f), tanX = tanY * (float)R.tv.img.width / (float)R.tv.img.height;
    static const float hudThreshold = getenv("WWHD_FRAMEGEN_HUD") ? (float)atof(getenv("WWHD_FRAMEGEN_HUD")) : 3.0f / 255.0f;
    Push pc{{half[0], half[1], half[2], move[0]}, {half[3], half[4], half[5], move[1]}, {half[6], half[7], half[8], move[2]},
            {tanX, tanY, hudThreshold, getenv("WWHD_FRAMEGEN_TINT") ? 1.0f : 0.0f}, {znear, zfar, 1, 0}, {tvRect[0], tvRect[1], tvRect[2], tvRect[3]},
            {target, followOn ? 1.0f : 0.0f, 0, 0}};

    // sampled images: layouts the renderer tracks (recorded into this command buffer before the pass)
    prepare(depth->img, Use::SAMPLED);
    prepare(R.tv.img, Use::SAMPLED);
    prepare(S.scene, Use::SAMPLED);
    // views made per frame (the depth surface may be recreated); released once the frame is done
    VkImageView depthView = make_view(depth->img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView scanView = make_view(R.tv.img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView sceneView = make_view(S.scene, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    on_complete([depthView, scanView, sceneView] {
        vkDestroyImageView(R.device, depthView, nullptr);
        vkDestroyImageView(R.device, scanView, nullptr);
        vkDestroyImageView(R.device, sceneView, nullptr);
    });
    // objects' own movement (WWHD_FRAMEGEN_OBJECTS=0: off): this frame's scene at quarter size, searched
    // in the previous frame's per 16x16 block, then filtered
    static const bool objects = !getenv("WWHD_FRAMEGEN_OBJECTS") || atoi(getenv("WWHD_FRAMEGEN_OBJECTS")) != 0;
    bool flow = false;
    VkPipeline quarterPipe = objects ? pipeline_for(kQuarter) : VK_NULL_HANDLE;
    VkPipeline flowPipe = objects ? pipeline_for(kFlow) : VK_NULL_HANDLE;
    VkPipeline filterPipe = objects ? pipeline_for(kFilter) : VK_NULL_HANDLE;
    if (quarterPipe && flowPipe && filterPipe) {
        const uint32_t tw = S.scene.width, th = S.scene.height;
        const uint32_t sw = std::max(1u, tw / 4), sh = std::max(1u, th / 4);
        S.smallCur ^= 1;
        Target& cur = S.small[S.smallCur];
        Target& prev = S.small[S.smallCur ^ 1];
        if (ensure_target(cur, sw, sh) && ensure_target(S.flowRaw, (tw + 15) / 16, (th + 15) / 16) &&
            ensure_target(S.flow, (tw + 15) / 16, (th + 15) / 16)) {
            Push none{};
            Input qin[1] = {{sceneView, R.linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            run_pass(cmd, quarterPipe, cur.fb, sw, sh, qin, 1, none);
            S.smallFrame[S.smallCur] = S.sceneFrame;
            if (fullValid && prev.img.image && prev.img.width == sw && prev.img.height == sh &&
                S.smallFrame[S.smallCur ^ 1] + 1 == S.sceneFrame) {
                Push fp{{full[0], full[1], full[2], fullMove[0]}, {full[3], full[4], full[5], fullMove[1]}, {full[6], full[7], full[8], fullMove[2]},
                        {tanX, tanY, 0, 0}, {znear, zfar, 1, 0}, {}};
                Input fin[3] = {{cur.view, S.nearest, VK_IMAGE_LAYOUT_GENERAL},
                                {prev.view, S.nearest, VK_IMAGE_LAYOUT_GENERAL},
                                {depthView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
                run_pass(cmd, flowPipe, S.flowRaw.fb, S.flowRaw.img.width, S.flowRaw.img.height, fin, 3, fp);
                Push ff{{}, {}, {}, {1.2f * 8.0f / (float)sw, 0, 0, 0}, {}, {}};  // ~1 quarter-size pixel in the encoding
                Input gin[1] = {{S.flowRaw.view, S.nearest, VK_IMAGE_LAYOUT_GENERAL}};
                run_pass(cmd, filterPipe, S.flow.fb, S.flow.img.width, S.flow.img.height, gin, 1, ff);
                flow = true;
            }
        }
    }
    pc.z[3] = flow ? 1.0f : 0.0f;
    if (!S.genFb[i]) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = S.pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &S.genView[i];
        fi.width = S.w;
        fi.height = S.h;
        fi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(R.device, &fi, nullptr, &S.genFb[i]));
    }
    // the HUD mask (one pass at a quarter of the TV picture's size; the warp used to compare the two
    // pictures at five places twice for every window pixel: about 7 ms a frame at 1.5x on an Adreno 650)
    VkPipeline maskPipe = pipeline_for(kHudMask);
    const uint32_t mw = std::max(1u, (R.tv.img.width + 3) / 4), mh = std::max(1u, (R.tv.img.height + 3) / 4);
    if (!maskPipe || !ensure_target(S.hudMask, mw, mh)) {
        out.push_back(&S.real[i]);
        return out;
    }
    {
        Input min_[2] = {{scanView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {sceneView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        run_pass(cmd, maskPipe, S.hudMask.fb, mw, mh, min_, 2, pc);
    }
    Input win[5] = {{compView, S.nearest, VK_IMAGE_LAYOUT_GENERAL},
                    {depthView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {S.hudMask.view, R.linearClamp, VK_IMAGE_LAYOUT_GENERAL},
                    {S.hudMask.view, R.linearClamp, VK_IMAGE_LAYOUT_GENERAL},
                    {flow ? S.flow.view : sceneView, flow ? R.linearClamp : S.nearest,
                     flow ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    run_pass(cmd, S.pipeline, S.genFb[i], S.w, S.h, win, 5, pc);
    S.generated++;
    out.push_back(&S.gen[i]);
    return out;
}

void destroy() {
    release_images();
    for (Target* t : {&S.small[0], &S.small[1], &S.flowRaw, &S.flow, &S.hudMask}) release_target(*t);
    S.smallFrame[0] = S.smallFrame[1] = ~0ull;
    if (S.scene.image) {
        retire_image(S.scene);
        S.scene = Image{};
    }
}

}  // namespace gfx::rp

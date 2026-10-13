// The app's frame rate setting on interp.cpp's modes. Frame interpolation (60 and 120 fps) is paced
// there: in-between frames that don't fit are left out and the game keeps 30 steps a second, which
// replaced this file's adaptive 60 (fallbacks to 30 measured from the swaps). 60 fps (generated)
// leaves the game at 30 and makes every other frame on the GPU (vk/reproj.cpp).
#include "fps60.h"

#include <algorithm>
#include <atomic>

#include "../interp.h"
#include "../runtime.h"

namespace gfx { void request_builtin_frame_generation(bool on); }  // vk/vk_device.cpp

namespace fps60 {
namespace {
std::atomic<int> g_mode{0};
}  // namespace

void set_mode(int m) {
    m = std::clamp(m, 0, 6);
    g_mode = m;
    interp::set_mode40(m == 4);  // 40 fps: interpolation at 4 frames per 3 steps (interp.cpp)
    // 60 fps (generated): the game at its own 30, in-between frames reprojected on the GPU (vk/reproj.cpp)
    gfx::request_builtin_frame_generation(m == 5);
    if (m == 3) {
        interp::set_mode(2);  // true 60 (experimental): Link and the camera at 60 logic steps
        return;
    }
    if (m == 5) {
        interp::set_mode(0);
        return;
    }
    if (m != 0) interp::set_fps(m == 6 ? 120 : 60);
    interp::set_mode(m != 0 ? 1 : 0);
}

int mode() { return g_mode; }

void on_swap() {}
}  // namespace fps60

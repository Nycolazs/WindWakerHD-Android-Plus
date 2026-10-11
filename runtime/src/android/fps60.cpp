// The app's frame rate setting on interp.cpp's modes. Frame interpolation (60 and 120 fps) is paced
// there: in-between frames that don't fit are left out and the game keeps 30 steps a second, which
// replaced this file's adaptive 60 (fallbacks to 30 measured from the swaps).
#include "fps60.h"

#include <algorithm>
#include <atomic>

#include "../interp.h"
#include "../runtime.h"

namespace fps60 {
namespace {
std::atomic<int> g_mode{0};
}  // namespace

void set_mode(int m) {
    m = std::clamp(m, 0, 5);
    g_mode = m;
    interp::set_mode40(m == 4);  // 40 fps: interpolation at 4 frames per 3 steps (interp.cpp)
    if (m == 3) {
        interp::set_mode(2);  // true 60 (experimental): Link and the camera at 60 logic steps
        return;
    }
    if (m != 0) interp::set_fps(m == 5 ? 120 : 60);
    interp::set_mode(m != 0 ? 1 : 0);
}

int mode() { return g_mode; }

void on_swap() {}
}  // namespace fps60

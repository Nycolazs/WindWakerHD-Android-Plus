#pragma once

// The app's frame rate setting (fps60.cpp): 0 off (30 fps), 1 60 fps (frame interpolation, paced
// so the game keeps its speed), 2 the same (the former adaptive 60, saved by older versions),
// 3 true 60 (experimental), 4 40 fps (120 Hz displays: 3 refreshes per frame), 5 60 fps (generated:
// the game at 30, every other frame reprojected on the GPU), 6 120 fps (frame interpolation, 120 Hz
// displays)
namespace fps60 {
void set_mode(int m);
int mode();
void on_swap();  // called by the game thread at each GX2 swap
}  // namespace fps60

#pragma once
#include "ppc.h"

// Select opens the game's menu on its MAP tab (map_shortcut.cpp)
namespace map_shortcut {
void request();          // any thread: Select was pressed (MainActivity, "open_map")
void service(Cpu* c);    // game main thread, frame start (interp.cpp)

// The in-game UI's current StateID (0 when there is none): the state machine of hdIngameUi_c, the
// one the pause menu, the Wind Waker's song sheet and "Select opens the map" all go through.
// Calls into guest code: game main thread only, and the caller's Cpu is left as it was.
uint32_t ui_state_key(Cpu* c);
}  // namespace map_shortcut

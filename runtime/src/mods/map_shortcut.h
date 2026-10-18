#pragma once
#include "ppc.h"

// Select opens the game's menu on its MAP tab (map_shortcut.cpp)
namespace map_shortcut {
void request();          // any thread: Select was pressed (MainActivity, "open_map")
void service(Cpu* c);    // game main thread, frame start (interp.cpp)
}  // namespace map_shortcut

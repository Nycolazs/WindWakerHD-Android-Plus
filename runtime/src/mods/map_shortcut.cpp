// Select opens the map. The game's − (Off-TV play on the Wii U) does nothing in this port; a short
// press of a controller's Select asks for the menu on its MAP tab instead (MainActivity, Controls >
// "Select opens the map").
//
// The in-game UI (hdIngameUi_c, sIngameUi) runs a state machine at +0x10 (hdStateMachine); the pause
// menu opens through StateID_ToMenu, as the player's + does (hdIngameChangeState), and the menu then
// shows the tab of mTransitionTab (+0x228) while no tab button is pressed (changeActiveScreen): 1 is
// the MAP tab. Addresses from ZeldaWWHDRecomp (controller_reapply.cpp, mods/story_chart.cpp).
// From play it opens the menu there; with the menu already open it goes to the MAP tab. Elsewhere
// (an event, a dialogue, the title) the press is ignored: the game couldn't open its menu either.
#include "map_shortcut.h"

#include <atomic>

#include "../release.h"
#include "../runtime.h"

namespace map_shortcut {
namespace {
const release::Data kIngameUi{0x101F8344};     // sIngameUi (hdIngameUi_c*, 0 outside gameplay)
const release::Data kPlayState{0x1049EC00};    // l_ingamePlayState
const release::Data kToMenuState{0x1049EC84};  // l_ingameToMenuState (the menu opening)
const release::Data kMenuState{0x1049ECB0};    // l_ingameMenuState
const release::Code kCurrentState{0x02006478}; // the state machine's current state id
const release::Code kChangeState{0x020063C0};  // hdIngameChangeState(machine, id)
constexpr uint32_t kUiState = 0x10, kUiTransitionTab = 0x228, kTabMap = 1;
constexpr int kOpenFrames = 120;  // the menu is open within this many frames

std::atomic<bool> g_request{false};
int g_opening = 0;  // frames left for the menu that was asked for to open

bool object(uint32_t p) { return p >= mem::kMem2Start && p < mem::kMem2End - 0x400; }

uint32_t state_key(Cpu* c, uint32_t id) {
    uint32_t vt = ld32(id + 8);
    return object(vt) ? guest_call(c, ld32(vt + 0x14), {id}) : 0;
}

// the in-game UI's current StateID key (0: none)
uint32_t ui_state(Cpu* c, uint32_t ui) {
    uint32_t current = guest_call(c, kCurrentState, {ui + kUiState});
    return object(current) ? state_key(c, current) : 0;
}

void step(Cpu* c, uint32_t ui) {
    const uint32_t state = ui_state(c, ui);
    if (g_opening > 0) {
        // the menu that was asked for is open: on the MAP tab (the opening may have reset the tab)
        if (state == state_key(c, kMenuState)) {
            st32(ui + kUiTransitionTab, kTabMap);
            g_opening = 0;
        } else if (--g_opening == 0) {
            LOG("[map] the menu did not open");
        }
    }
    if (!g_request.exchange(false)) return;
    if (state == state_key(c, kPlayState)) {
        guest_call(c, kChangeState, {ui + kUiState, kToMenuState});
        st32(ui + kUiTransitionTab, kTabMap);
        g_opening = kOpenFrames;
        LOG("[map] Select: the menu opens on the MAP tab");
    } else if (state == state_key(c, kMenuState)) {
        st32(ui + kUiTransitionTab, kTabMap);
        LOG("[map] Select: the menu goes to the MAP tab");
    }
}
}  // namespace

void request() { g_request = true; }

void service(Cpu* c) {
    if (!g_request.load(std::memory_order_relaxed) && g_opening == 0) return;
    const uint32_t ui = ld32(kIngameUi);
    if (!object(ui)) {
        g_request = false;
        g_opening = 0;
        return;
    }
    // the guest calls leave the registers as the game left them for this frame
    Cpu saved = *c;
    step(c, ui);
    uint32_t core = c->core;
    void* th = c->thread;
    *c = saved;
    c->core = core;
    c->thread = th;
}

// The in-game UI's current StateID (0 when there is none): the hybrid layout needs it to tell the
// pause reached with the Wind Waker out (a state of this machine) from conducting. The guest calls
// leave the registers as the game left them for this frame (as in service()).
uint32_t ui_state_key(Cpu* c) {
    const uint32_t ui = ld32(kIngameUi);
    if (!object(ui)) return 0;
    Cpu saved = *c;
    const uint32_t s = ui_state(c, ui);
    uint32_t core = c->core;
    void* th = c->thread;
    *c = saved;
    c->core = core;
    c->thread = th;
    return s;
}
}  // namespace map_shortcut

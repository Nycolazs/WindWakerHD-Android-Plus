// Upstream's canonical-address macros (runtime/include/guest_addr.h there) on this port's release
// maps (release.h): GC() for code, GD() for data, from a USA address to the running release's.
#pragma once
#include "release.h"
#define GC(a) release::code((uint32_t)(a))
#define GD(a) release::data((uint32_t)(a))

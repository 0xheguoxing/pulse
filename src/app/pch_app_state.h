// Precompiled header for the app-layer sources of pulse.exe that include
// app_state.h (directly or through app_internal.h and friends). Parsing that
// header tree is most of their compile time. CMake picks the sources by
// scanning their includes; see PULSE_USE_PCH in CMakeLists.txt.
#pragma once

#include "../common/pch_system.h"
#include "app_state.h"

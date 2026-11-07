// Interactive console frontend (the default mode of agent.exe). Runs entirely on the main thread:
// it waits on console input with a 50 ms timeout, then drains the backend's event queue.
#pragma once
#include "config.h"

namespace ag {

int run_ui(const Config& cfg);

}  // namespace ag

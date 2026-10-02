#pragma once

#include "common.h"

namespace ep {

// Runs the background agent on the calling thread until it is asked to exit.
// The agent keeps the "Pinned" group up to date in every open Explorer window.
int RunAgent();

// Returns the agent's window when an agent is running in this session.
HWND FindAgentWindow();

// Starts the agent in a new process when none is running.
bool EnsureAgentRunning();

}  // namespace ep

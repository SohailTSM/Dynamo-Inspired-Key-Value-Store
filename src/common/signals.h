// SIGINT/SIGTERM handling for server processes: block the signals in all threads, then a dedicated
// waiter thread runs `on_signal` once when one arrives.
#pragma once

#include <functional>

namespace dkv {

// Call at the very start of main(), before any thread is created.
void BlockTerminationSignals();
// Blocks the calling thread until SIGINT/SIGTERM arrives; returns the signal number.
int WaitForTerminationSignal();
// Starts a detached thread that waits for SIGINT/SIGTERM and then calls on_signal().
void OnTerminationSignal(std::function<void()> on_signal);

}  // namespace dkv

// The SteamVR side: waits for SteamVR, owns the dashboard tab, and runs auto on/off.
#pragma once

#include "model.h"
#include "ui.h"

#include <atomic>

namespace lh {

class Overlay {
public:
    Overlay(Model &model, Ui &ui) : model_(model), ui_(ui) {}
    // Loops until `stop`: connect to SteamVR when it appears, show the tab, handle its quit.
    void run(const std::atomic<bool> &stop);

private:
    enum class End { Quit, Stop, Error };
    End session(const std::atomic<bool> &stop);
    Model &model_;
    Ui &ui_;
};

// Prints what the sandbox can see (OpenVR runtime, SteamVR connection, Bluetooth). Returns 0 if all good.
int probe();

}  // namespace lh

#pragma once
#include "fish_brain.h"
#include <memory>
#include <string>

// A floating window that shows one fish's brain firing: every neuron as a
// dot on a side view of a larval zebrafish brain, lit by its recent spikes.
class BrainWindow {
public:
    BrainWindow();
    ~BrainWindow();
    BrainWindow(const BrainWindow&) = delete;
    BrainWindow& operator=(const BrainWindow&) = delete;

    void show();
    void hide();
    // False once hidden, including when the person closes the window.
    bool visible() const;
    // Clicks in the window since the last call; each one moves to the next fish.
    unsigned take_clicks();
    // Redraws with this brain; title and status describe the fish.
    void update(const FishBrain& brain, const std::string& title, const std::string& status);
    // Shows a message instead of a brain, when there is no fish.
    void update_empty(const std::string& message);
    // Draws the window's contents for this brain into a PNG, offscreen.
    static bool snapshot(const FishBrain& brain, const std::string& title, const std::string& status, const char* path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

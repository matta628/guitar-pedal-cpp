#pragma once

#include <chrono>
#include <functional>

// Turns a stream of debounced button presses into single-click and
// double-click events.
//
// A single click cannot be reported the moment the button goes down: until the
// double-click window expires there is no way to know whether a second press
// is coming. So on_single fires `window` milliseconds late by construction,
// while on_double fires immediately on the second press. That trade is the
// reason it belongs on a utility switch (clear / cycle effect) and never on the
// looper's record switch, where the press instant defines the loop boundary.
//
// Currently unwired: the second footswitch was dropped on 2026-08-31 so bench
// testing could concentrate on the looper, and both of its gestures are in the
// web UI. Kept because it is self-contained and tested — rewiring a second
// switch is a `make_unique<GpioButton>` plus a `set_handlers` call in main.
//
// Not thread-safe: on_press() and poll() are meant to be called from the same
// polling thread (GpioButton's), and the handlers run on that thread too.
class ClickDetector {
public:
    using Clock = std::chrono::steady_clock;

    explicit ClickDetector(std::chrono::milliseconds double_click_window =
                               std::chrono::milliseconds(350));

    void set_handlers(std::function<void()> on_single, std::function<void()> on_double);

    // Feed one debounced press.
    void on_press(Clock::time_point now);

    // Call regularly (every GpioButton poll tick). Flushes a pending single
    // click once the double-click window has closed.
    void poll(Clock::time_point now);

private:
    std::chrono::milliseconds window_;
    bool awaiting_second_ = false;
    Clock::time_point first_press_{};

    std::function<void()> on_single_;
    std::function<void()> on_double_;
};

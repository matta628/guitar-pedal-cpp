#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

struct gpiod_chip;
struct gpiod_line_request;

// Polls a single GPIO line on a background thread (never the audio thread)
// and calls on_press() after debouncing each release->press transition.
// Assumes the button is wired active-low: one leg to the GPIO line, the
// other to GND, relying on the line's internal pull-up.
//
// Built against the libgpiod v2 API (Debian 13 and later ship v2 only).
class GpioButton {
public:
    // chip_name accepts either a bare name ("gpiochip0") or a full
    // character-device path ("/dev/gpiochip0").
    GpioButton(const std::string& chip_name, unsigned int line_offset);
    ~GpioButton();

    GpioButton(const GpioButton&) = delete;
    GpioButton& operator=(const GpioButton&) = delete;

    // on_press fires once per debounced release->press transition.
    // on_tick, if supplied, fires once per poll interval on the same thread,
    // so callers needing their own timeouts (ClickDetector) don't have to
    // start a second thread to get one.
    void start(std::function<void()> on_press, std::function<void()> on_tick = {});
    void stop();

private:
    void poll_loop();

    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
    unsigned int line_offset_ = 0;

    std::function<void()> on_press_;
    std::function<void()> on_tick_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

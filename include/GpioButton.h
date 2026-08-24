#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

struct gpiod_chip;
struct gpiod_line;

// Polls a single GPIO line on a background thread (never the audio thread)
// and calls on_press() after debouncing each release->press transition.
// Assumes the button is wired active-low: one leg to the GPIO line, the
// other to GND, relying on the line's internal pull-up.
class GpioButton {
public:
    GpioButton(const std::string& chip_name, unsigned int line_offset);
    ~GpioButton();

    GpioButton(const GpioButton&) = delete;
    GpioButton& operator=(const GpioButton&) = delete;

    void start(std::function<void()> on_press);
    void stop();

private:
    void poll_loop();

    gpiod_chip* chip_ = nullptr;
    gpiod_line* line_ = nullptr;

    std::function<void()> on_press_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

#include "GpioButton.h"

#include <gpiod.h>

#include <chrono>
#include <stdexcept>

namespace {
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kDebounceStable = std::chrono::milliseconds(20);
}  // namespace

GpioButton::GpioButton(const std::string& chip_name, unsigned int line_offset) {
    chip_ = gpiod_chip_open_by_name(chip_name.c_str());
    if (!chip_) {
        throw std::runtime_error("GpioButton: failed to open GPIO chip '" + chip_name +
                                  "' (run `gpioinfo` on the Pi to find the right chip name)");
    }

    line_ = gpiod_chip_get_line(chip_, line_offset);
    if (!line_) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioButton: failed to get GPIO line " + std::to_string(line_offset));
    }

    if (gpiod_line_request_input_flags(line_, "guitar_pedal", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioButton: failed to request GPIO line " + std::to_string(line_offset) +
                                  " as input (already in use?)");
    }
}

GpioButton::~GpioButton() {
    stop();
    gpiod_line_release(line_);
    gpiod_chip_close(chip_);
}

void GpioButton::start(std::function<void()> on_press) {
    on_press_ = std::move(on_press);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&GpioButton::poll_loop, this);
}

void GpioButton::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void GpioButton::poll_loop() {
    int last_stable = gpiod_line_get_value(line_);  // 1 = released (pulled up), 0 = pressed
    int last_seen = last_stable;
    auto last_change = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(kPollInterval);

        const int value = gpiod_line_get_value(line_);
        if (value < 0) {
            continue;  // transient read error; retry next poll
        }

        const auto now = std::chrono::steady_clock::now();
        if (value != last_seen) {
            last_seen = value;
            last_change = now;
            continue;
        }

        if (value != last_stable && now - last_change >= kDebounceStable) {
            const bool pressed = (last_stable == 1 && value == 0);
            last_stable = value;
            if (pressed && on_press_) {
                on_press_();
            }
        }
    }
}

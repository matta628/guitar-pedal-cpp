#include "GpioButton.h"

#include <gpiod.h>

#include <chrono>
#include <memory>
#include <stdexcept>

namespace {
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kDebounceStable = std::chrono::milliseconds(20);

// libgpiod v2 hands back three short-lived C objects that only matter until
// the line request succeeds. Wrapping them keeps the throwing error paths
// below from leaking them.
template <typename T, void (*FreeFn)(T*)>
struct CFree {
    void operator()(T* p) const noexcept { FreeFn(p); }
};

using SettingsPtr = std::unique_ptr<gpiod_line_settings, CFree<gpiod_line_settings, gpiod_line_settings_free>>;
using LineConfigPtr = std::unique_ptr<gpiod_line_config, CFree<gpiod_line_config, gpiod_line_config_free>>;
using RequestConfigPtr =
    std::unique_ptr<gpiod_request_config, CFree<gpiod_request_config, gpiod_request_config_free>>;

std::string to_chip_path(const std::string& chip_name) {
    if (!chip_name.empty() && chip_name.front() == '/') {
        return chip_name;
    }
    return "/dev/" + chip_name;
}
}  // namespace

GpioButton::GpioButton(const std::string& chip_name, unsigned int line_offset)
    : line_offset_(line_offset) {
    const std::string path = to_chip_path(chip_name);

    // v2 opens the character device by path; v1's open-by-name is gone.
    chip_ = gpiod_chip_open(path.c_str());
    if (!chip_) {
        throw std::runtime_error("GpioButton: failed to open GPIO chip '" + path +
                                 "' (run `gpiodetect` to list the chips on this board)");
    }

    SettingsPtr settings(gpiod_line_settings_new());
    if (!settings ||
        gpiod_line_settings_set_direction(settings.get(), GPIOD_LINE_DIRECTION_INPUT) < 0 ||
        gpiod_line_settings_set_bias(settings.get(), GPIOD_LINE_BIAS_PULL_UP) < 0) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioButton: failed to configure line as pulled-up input");
    }

    LineConfigPtr line_cfg(gpiod_line_config_new());
    if (!line_cfg || gpiod_line_config_add_line_settings(line_cfg.get(), &line_offset_, 1,
                                                         settings.get()) < 0) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioButton: failed to build line config for line " +
                                 std::to_string(line_offset));
    }

    RequestConfigPtr req_cfg(gpiod_request_config_new());
    if (!req_cfg) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioButton: failed to allocate request config");
    }
    gpiod_request_config_set_consumer(req_cfg.get(), "guitar_pedal");

    request_ = gpiod_chip_request_lines(chip_, req_cfg.get(), line_cfg.get());
    if (!request_) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioButton: failed to request GPIO line " +
                                 std::to_string(line_offset) + " as input (already in use?)");
    }
}

GpioButton::~GpioButton() {
    stop();
    if (request_) {
        gpiod_line_request_release(request_);
    }
    if (chip_) {
        gpiod_chip_close(chip_);
    }
}

void GpioButton::start(std::function<void()> on_press, std::function<void()> on_tick) {
    on_press_ = std::move(on_press);
    on_tick_ = std::move(on_tick);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&GpioButton::poll_loop, this);
}

void GpioButton::set_release_handler(std::function<void(std::chrono::milliseconds)> on_release) {
    on_release_ = std::move(on_release);
}

void GpioButton::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void GpioButton::poll_loop() {
    // ACTIVE (1) = released, held high by the pull-up; INACTIVE (0) = pressed to GND.
    gpiod_line_value last_stable = gpiod_line_request_get_value(request_, line_offset_);
    if (last_stable == GPIOD_LINE_VALUE_ERROR) {
        last_stable = GPIOD_LINE_VALUE_ACTIVE;  // assume released until a read succeeds
    }
    gpiod_line_value last_seen = last_stable;
    auto last_change = std::chrono::steady_clock::now();
    auto pressed_at = last_change;

    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(kPollInterval);

        const gpiod_line_value value = gpiod_line_request_get_value(request_, line_offset_);

        const auto now = std::chrono::steady_clock::now();
        if (on_tick_) {
            on_tick_();
        }

        if (value == GPIOD_LINE_VALUE_ERROR) {
            continue;  // transient read error; retry next poll
        }
        if (value != last_seen) {
            last_seen = value;
            last_change = now;
            continue;
        }

        if (value != last_stable && now - last_change >= kDebounceStable) {
            const bool pressed =
                (last_stable == GPIOD_LINE_VALUE_ACTIVE && value == GPIOD_LINE_VALUE_INACTIVE);
            last_stable = value;
            if (pressed) {
                // Timed from the debounced edge, not the raw one, so the
                // duration excludes contact bounce rather than counting it.
                pressed_at = now;
                if (on_press_) {
                    on_press_();
                }
            } else if (on_release_) {
                on_release_(std::chrono::duration_cast<std::chrono::milliseconds>(now - pressed_at));
            }
        }
    }
}

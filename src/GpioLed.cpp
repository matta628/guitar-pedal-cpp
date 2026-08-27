#include "GpioLed.h"

#include <gpiod.h>

#include <memory>
#include <stdexcept>

namespace {
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

GpioLed::GpioLed(const std::string& chip_name, unsigned int line_offset)
    : line_offset_(line_offset) {
    const std::string path = to_chip_path(chip_name);

    chip_ = gpiod_chip_open(path.c_str());
    if (!chip_) {
        throw std::runtime_error("GpioLed: failed to open GPIO chip '" + path + "'");
    }

    SettingsPtr settings(gpiod_line_settings_new());
    if (!settings ||
        gpiod_line_settings_set_direction(settings.get(), GPIOD_LINE_DIRECTION_OUTPUT) < 0 ||
        gpiod_line_settings_set_output_value(settings.get(), GPIOD_LINE_VALUE_INACTIVE) < 0) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioLed: failed to configure line as output");
    }

    LineConfigPtr line_cfg(gpiod_line_config_new());
    if (!line_cfg || gpiod_line_config_add_line_settings(line_cfg.get(), &line_offset_, 1,
                                                         settings.get()) < 0) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioLed: failed to build line config for line " +
                                 std::to_string(line_offset));
    }

    RequestConfigPtr req_cfg(gpiod_request_config_new());
    if (!req_cfg) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioLed: failed to allocate request config");
    }
    gpiod_request_config_set_consumer(req_cfg.get(), "guitar_pedal");

    request_ = gpiod_chip_request_lines(chip_, req_cfg.get(), line_cfg.get());
    if (!request_) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("GpioLed: failed to request GPIO line " +
                                 std::to_string(line_offset) + " as output (already in use?)");
    }
}

GpioLed::~GpioLed() {
    if (request_) {
        gpiod_line_request_set_value(request_, line_offset_, GPIOD_LINE_VALUE_INACTIVE);
        gpiod_line_request_release(request_);
    }
    if (chip_) {
        gpiod_chip_close(chip_);
    }
}

void GpioLed::set(bool on) {
    if (on == value_) {
        return;
    }
    value_ = on;
    gpiod_line_request_set_value(request_, line_offset_,
                                 on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

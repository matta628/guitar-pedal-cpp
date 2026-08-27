#pragma once

#include <string>

struct gpiod_chip;
struct gpiod_line_request;

// A single GPIO line driven as an output, for a status LED.
// Wired active-high: line -> current-limiting resistor -> LED anode,
// LED cathode -> GND, so a high line lights it.
//
// Built against the libgpiod v2 API, same as GpioButton.
class GpioLed {
public:
    GpioLed(const std::string& chip_name, unsigned int line_offset);
    ~GpioLed();

    GpioLed(const GpioLed&) = delete;
    GpioLed& operator=(const GpioLed&) = delete;

    // Only writes when the value actually changes, so the 50 Hz indicator
    // loop isn't issuing an ioctl per tick just to restate a steady state.
    void set(bool on);

private:
    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
    unsigned int line_offset_ = 0;
    bool value_ = false;
};

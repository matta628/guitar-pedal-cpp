#include "Lcd1602.h"

#include <gpiod.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
constexpr std::size_t kRs = 0;
constexpr std::size_t kEnable = 1;
constexpr std::size_t kD4 = 2;

constexpr int kColumns = 16;

// HD44780 commands.
constexpr unsigned int kCmdClear = 0x01;
constexpr unsigned int kCmdEntryMode = 0x06;      // increment cursor, no shift
constexpr unsigned int kCmdDisplayOn = 0x0C;      // display on, cursor off, blink off
constexpr unsigned int kCmdDisplayOff = 0x08;
constexpr unsigned int kCmdFunctionSet = 0x28;    // 4-bit, 2 lines, 5x8 font
constexpr unsigned int kCmdSetDdram = 0x80;

void delay_us(long us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void delay_ms(long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

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

Lcd1602::Lcd1602(const std::string& chip_name, const Pins& pins) {
    offsets_ = {pins.rs, pins.enable, pins.d4, pins.d5, pins.d6, pins.d7};

    const std::string path = to_chip_path(chip_name);
    chip_ = gpiod_chip_open(path.c_str());
    if (!chip_) {
        throw std::runtime_error("Lcd1602: failed to open GPIO chip '" + path + "'");
    }

    SettingsPtr settings(gpiod_line_settings_new());
    if (!settings ||
        gpiod_line_settings_set_direction(settings.get(), GPIOD_LINE_DIRECTION_OUTPUT) < 0 ||
        gpiod_line_settings_set_output_value(settings.get(), GPIOD_LINE_VALUE_INACTIVE) < 0) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("Lcd1602: failed to configure lines as outputs");
    }

    LineConfigPtr line_cfg(gpiod_line_config_new());
    // All six lines go in one request so a nibble plus RS lands in a single
    // set_values ioctl — no chance of a data line changing after RS but
    // before the enable pulse.
    if (!line_cfg || gpiod_line_config_add_line_settings(line_cfg.get(), offsets_.data(),
                                                         offsets_.size(), settings.get()) < 0) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("Lcd1602: failed to build line config");
    }

    RequestConfigPtr req_cfg(gpiod_request_config_new());
    if (!req_cfg) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("Lcd1602: failed to allocate request config");
    }
    gpiod_request_config_set_consumer(req_cfg.get(), "guitar_pedal");

    request_ = gpiod_chip_request_lines(chip_, req_cfg.get(), line_cfg.get());
    if (!request_) {
        gpiod_chip_close(chip_);
        throw std::runtime_error("Lcd1602: failed to request LCD lines as outputs (already in use?)");
    }

    // Datasheet power-on reset sequence. The panel comes up in 8-bit mode and
    // has to be walked into 4-bit mode with three 0x3 nibbles before it will
    // accept a normal function set.
    delay_ms(50);
    send_nibble(0x03, false);
    delay_us(4500);
    send_nibble(0x03, false);
    delay_us(4500);
    send_nibble(0x03, false);
    delay_us(150);
    send_nibble(0x02, false);  // now in 4-bit mode

    send_byte(kCmdFunctionSet, false);
    send_byte(kCmdDisplayOff, false);
    send_byte(kCmdClear, false);
    delay_ms(2);
    send_byte(kCmdEntryMode, false);
    send_byte(kCmdDisplayOn, false);
}

Lcd1602::~Lcd1602() {
    if (request_) {
        send_byte(kCmdClear, false);
        delay_ms(2);
        send_byte(kCmdDisplayOff, false);
        gpiod_line_request_release(request_);
    }
    if (chip_) {
        gpiod_chip_close(chip_);
    }
}

void Lcd1602::set_line_values(unsigned int rs, unsigned int nibble) {
    gpiod_line_value values[6];
    values[kRs] = rs ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    values[kEnable] = GPIOD_LINE_VALUE_INACTIVE;
    for (unsigned int bit = 0; bit < 4; ++bit) {
        values[kD4 + bit] =
            ((nibble >> bit) & 1U) ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    }
    gpiod_line_request_set_values(request_, values);
}

void Lcd1602::pulse_enable() {
    gpiod_line_request_set_value(request_, offsets_[kEnable], GPIOD_LINE_VALUE_ACTIVE);
    delay_us(1);  // datasheet minimum is 450 ns; a syscall alone already exceeds it
    gpiod_line_request_set_value(request_, offsets_[kEnable], GPIOD_LINE_VALUE_INACTIVE);
    delay_us(50);  // most instructions complete in 37 us
}

void Lcd1602::send_nibble(unsigned int value, bool is_data) {
    set_line_values(is_data ? 1U : 0U, value & 0x0FU);
    pulse_enable();
}

void Lcd1602::send_byte(unsigned int value, bool is_data) {
    send_nibble(value >> 4, is_data);
    send_nibble(value, is_data);
}

void Lcd1602::clear() {
    send_byte(kCmdClear, false);
    delay_ms(2);  // clear is the slow one: ~1.52 ms
    shown_[0].clear();
    shown_[1].clear();
}

void Lcd1602::write_line(int row, const std::string& text) {
    if (row < 0 || row > 1) {
        return;
    }

    std::string padded = text.substr(0, kColumns);
    padded.resize(kColumns, ' ');
    if (padded == shown_[static_cast<std::size_t>(row)]) {
        return;
    }
    shown_[static_cast<std::size_t>(row)] = padded;

    // Row 1's DDRAM starts at 0x40 on every HD44780 16x2, not at 0x10 —
    // the two rows are not contiguous in memory.
    send_byte(kCmdSetDdram | (row == 0 ? 0x00U : 0x40U), false);
    for (char c : padded) {
        send_byte(static_cast<unsigned char>(c), true);
    }
}

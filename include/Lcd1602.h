#pragma once

#include <array>
#include <string>

struct gpiod_chip;
struct gpiod_line_request;

// Drives an HD44780-compatible 16x2 character LCD over six GPIO lines in
// 4-bit mode (RS, E, D4-D7).
//
// Write-only by design: R/W is tied to GND on the module, so the busy flag is
// never read back and every operation just waits out the datasheet's
// worst-case time instead. That is what keeps the panel safe to talk to from a
// 3.3V Pi — the module's logic runs at 5V, and with R/W grounded no 5V line
// ever drives a Pi input.
//
// Every call sleeps for hundreds of microseconds, so this belongs on the
// indicator thread and must never be touched from the audio callback.
class Lcd1602 {
public:
    struct Pins {
        unsigned int rs;
        unsigned int enable;
        unsigned int d4;
        unsigned int d5;
        unsigned int d6;
        unsigned int d7;
    };

    Lcd1602(const std::string& chip_name, const Pins& pins);
    ~Lcd1602();

    Lcd1602(const Lcd1602&) = delete;
    Lcd1602& operator=(const Lcd1602&) = delete;

    // Writes text padded/truncated to 16 columns. Skips the transfer entirely
    // if that row already shows this exact text, so the 50 Hz indicator loop
    // isn't repainting a static screen.
    void write_line(int row, const std::string& text);

    void clear();

private:
    void send_nibble(unsigned int value, bool is_data);
    void send_byte(unsigned int value, bool is_data);
    void pulse_enable();
    void set_line_values(unsigned int rs, unsigned int nibble);

    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
    std::array<unsigned int, 6> offsets_{};
    std::array<std::string, 2> shown_{};
};

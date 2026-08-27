#pragma once

#include <chrono>

// Computes whether a status LED should be lit at a given instant.
//
// Pure logic, no GPIO: the caller polls value(now) and pushes the result at a
// GpioLed. Keeping it separate means the blink timing is offline-testable
// without a Pi, and means no timer thread per LED — one indicator loop drives
// every pattern.
//
// A pattern has a steady base (off / solid / blink) plus an optional one-shot
// burst that temporarily overrides it. The burst is how a momentary event
// (loop cleared, preset changed) gets signalled on an LED whose steady state
// is already showing something else.
class LedPattern {
public:
    using Clock = std::chrono::steady_clock;

    enum class Base { Off, Solid, Blink };

    void set_off();
    void set_solid();
    // period is a full on+off cycle; the LED is lit for the first half.
    void set_blink(std::chrono::milliseconds period, Clock::time_point now);

    // Flash `count` times, then fall back to whatever the base is. Replaces
    // any burst already in flight.
    void flash_burst(int count, std::chrono::milliseconds on,
                     std::chrono::milliseconds off, Clock::time_point now);

    bool value(Clock::time_point now) const;

private:
    Base base_ = Base::Off;
    std::chrono::milliseconds blink_period_{0};
    Clock::time_point blink_start_{};

    int burst_count_ = 0;
    std::chrono::milliseconds burst_on_{0};
    std::chrono::milliseconds burst_off_{0};
    Clock::time_point burst_start_{};
};

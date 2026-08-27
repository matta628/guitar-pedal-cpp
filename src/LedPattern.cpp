#include "LedPattern.h"

void LedPattern::set_off() {
    base_ = Base::Off;
}

void LedPattern::set_solid() {
    base_ = Base::Solid;
}

void LedPattern::set_blink(std::chrono::milliseconds period, Clock::time_point now) {
    // Restarting the phase on every call would freeze the LED at the top of
    // its cycle when the indicator loop re-asserts the same blink each tick,
    // so an unchanged period leaves blink_start_ alone.
    if (base_ == Base::Blink && blink_period_ == period) {
        return;
    }
    base_ = Base::Blink;
    blink_period_ = period;
    blink_start_ = now;
}

void LedPattern::flash_burst(int count, std::chrono::milliseconds on,
                             std::chrono::milliseconds off, Clock::time_point now) {
    if (count <= 0) {
        burst_count_ = 0;
        return;
    }
    burst_count_ = count;
    burst_on_ = on;
    burst_off_ = off;
    burst_start_ = now;
}

bool LedPattern::value(Clock::time_point now) const {
    if (burst_count_ > 0) {
        const auto cycle = burst_on_ + burst_off_;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - burst_start_);
        if (elapsed < cycle * burst_count_) {
            return (elapsed % cycle) < burst_on_;
        }
        // Burst finished; fall through to the base. burst_count_ stays set
        // rather than being cleared here so value() can remain const — the
        // elapsed-time check above is what ends it.
    }

    switch (base_) {
        case Base::Off:
            return false;
        case Base::Solid:
            return true;
        case Base::Blink: {
            if (blink_period_.count() <= 0) {
                return false;
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - blink_start_);
            return (elapsed % blink_period_) < (blink_period_ / 2);
        }
    }
    return false;
}

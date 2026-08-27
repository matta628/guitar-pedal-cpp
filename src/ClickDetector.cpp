#include "ClickDetector.h"

ClickDetector::ClickDetector(std::chrono::milliseconds double_click_window)
    : window_(double_click_window) {}

void ClickDetector::set_handlers(std::function<void()> on_single, std::function<void()> on_double) {
    on_single_ = std::move(on_single);
    on_double_ = std::move(on_double);
}

void ClickDetector::on_press(Clock::time_point now) {
    if (awaiting_second_ && now - first_press_ <= window_) {
        awaiting_second_ = false;
        if (on_double_) {
            on_double_();
        }
        return;
    }

    // Either the first press of a gesture, or a press so late that the
    // previous one should already have been flushed as a single by poll().
    // Flushing it here too keeps the two paths consistent if poll() is called
    // less often than the window.
    if (awaiting_second_ && on_single_) {
        on_single_();
    }
    awaiting_second_ = true;
    first_press_ = now;
}

void ClickDetector::poll(Clock::time_point now) {
    if (!awaiting_second_ || now - first_press_ <= window_) {
        return;
    }
    awaiting_second_ = false;
    if (on_single_) {
        on_single_();
    }
}

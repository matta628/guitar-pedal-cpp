// Bench diagnostic for the footswitch / LED / LCD wiring.
//
// Deliberately separate from guitar_pedal: this needs no audio interface and
// exercises exactly one piece of hardware at a time, so a wiring fault can be
// isolated to a single wire instead of being debugged through the whole rig.
//
//   ./build/gpio_check led    22      blink an LED on GPIO22
//   ./build/gpio_check button 17      print each debounced press on GPIO17
//   ./build/gpio_check clicks 27      classify single vs double clicks
//   ./build/gpio_check echo 17 22     flash the LED on GPIO22 per press on GPIO17
//   ./build/gpio_check lcd            write a test pattern to the LCD1602
//   ./build/gpio_check all            every indicator + both switches at once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "ClickDetector.h"
#include "GpioButton.h"
#include "GpioLed.h"
#include "Lcd1602.h"

namespace {

constexpr const char* kChip = "gpiochip0";

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

void wait_for_ctrl_c() {
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int usage() {
    std::cerr << "usage: gpio_check <led|button|clicks> <gpio-line>\n"
                 "       gpio_check echo <switch-line> <led-line>\n"
                 "       gpio_check <lcd|all>\n";
    return 2;
}

int run_led(unsigned int line) {
    GpioLed led(kChip, line);
    std::cout << "Blinking GPIO" << line << " at 1 Hz. Ctrl+C to stop.\n"
              << "If it stays dark: check LED polarity (long leg toward the resistor),\n"
              << "that the resistor is in series, and that the cathode reaches the GND rail.\n";
    bool on = false;
    while (!g_stop.load(std::memory_order_relaxed)) {
        on = !on;
        led.set(on);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    led.set(false);
    return 0;
}

int run_button(unsigned int line) {
    GpioButton button(kChip, line);
    std::atomic<int> presses{0};
    button.start([&presses]() {
        std::cout << "press #" << presses.fetch_add(1) + 1 << "\n" << std::flush;
    });
    std::cout << "Watching GPIO" << line << ". Press the switch. Ctrl+C to stop.\n"
              << "Nothing at all: check the switch reaches this line and the GND rail.\n"
              << "A flood of presses with no touch: the line isn't actually connected\n"
              << "(floating), or a tactile button is bridging an already-shorted pair.\n";
    wait_for_ctrl_c();
    button.stop();
    std::cout << "\n" << presses.load() << " press(es) seen.\n";
    return 0;
}

// Wiring a switch is a two-person job when the only feedback is a terminal on
// another machine: someone has to watch the screen while someone else stomps.
// This closes the loop at the pedal itself -- each debounced press flashes an
// LED you already proved works, so the switch can be tested standing up.
int run_echo(unsigned int switch_line, unsigned int led_line) {
    GpioLed led(kChip, led_line);
    GpioButton button(kChip, switch_line);

    constexpr auto kFlash = std::chrono::milliseconds(200);
    std::atomic<int> presses{0};
    // steady_clock so the flash length is unaffected by wall-clock changes.
    std::atomic<std::chrono::steady_clock::rep> last_press{0};

    button.start([&]() {
        last_press.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                         std::memory_order_relaxed);
        std::cout << "press #" << presses.fetch_add(1) + 1 << "\n" << std::flush;
    });

    std::cout << "Watching GPIO" << switch_line << ", flashing GPIO" << led_line
              << " on each press.\n"
              << "Stomp the switch -- the LED should blink once per press. Ctrl+C to stop.\n";

    while (!g_stop.load(std::memory_order_relaxed)) {
        auto since = std::chrono::steady_clock::now().time_since_epoch()
                     - std::chrono::steady_clock::duration(
                           last_press.load(std::memory_order_relaxed));
        bool lit = presses.load(std::memory_order_relaxed) > 0 && since < kFlash;
        led.set(lit);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    button.stop();
    led.set(false);
    std::cout << "\n" << presses.load() << " press(es) seen.\n";
    return 0;
}

int run_clicks(unsigned int line) {
    ClickDetector clicks;
    clicks.set_handlers([]() { std::cout << "SINGLE click -> would clear the loop\n" << std::flush; },
                        []() { std::cout << "DOUBLE click -> would cycle the preset\n" << std::flush; });

    GpioButton button(kChip, line);
    button.start([&clicks]() { clicks.on_press(std::chrono::steady_clock::now()); },
                 [&clicks]() { clicks.poll(std::chrono::steady_clock::now()); });
    std::cout << "Watching GPIO" << line << " for clicks. Ctrl+C to stop.\n"
              << "The single click is reported ~350 ms late on purpose -- that is the\n"
              << "double-click window closing, not lag.\n";
    wait_for_ctrl_c();
    button.stop();
    return 0;
}

constexpr Lcd1602::Pins kLcdPins{/*rs=*/5, /*enable=*/6, /*d4=*/13, /*d5=*/19, /*d6=*/26, /*d7=*/16};

int run_lcd() {
    Lcd1602 lcd(kChip, kLcdPins);
    std::cout << "Writing to the LCD. Ctrl+C to stop.\n"
              << "Solid blocks on row 1 and nothing else = contrast pot, not a code fault.\n"
              << "Turn the pot slowly until text appears.\n";
    int tick = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        lcd.write_line(0, "LCD OK  row 1");
        lcd.write_line(1, "counter: " + std::to_string(tick++));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 0;
}

int run_all() {
    std::cout << "Full rig check. Press either switch; both LEDs blink; LCD counts.\n"
                 "Ctrl+C to stop.\n";

    GpioLed looper_led(kChip, 22);
    GpioLed preset_led(kChip, 23);
    Lcd1602 lcd(kChip, kLcdPins);

    std::atomic<int> looper_presses{0};
    std::atomic<int> utility_presses{0};

    ClickDetector clicks;
    clicks.set_handlers([]() { std::cout << "utility: SINGLE\n" << std::flush; },
                        []() { std::cout << "utility: DOUBLE\n" << std::flush; });

    GpioButton looper_switch(kChip, 17);
    GpioButton utility_switch(kChip, 27);
    looper_switch.start([&looper_presses]() {
        std::cout << "looper switch: press #" << looper_presses.fetch_add(1) + 1 << "\n" << std::flush;
    });
    utility_switch.start(
        [&clicks, &utility_presses]() {
            utility_presses.fetch_add(1, std::memory_order_relaxed);
            clicks.on_press(std::chrono::steady_clock::now());
        },
        [&clicks]() { clicks.poll(std::chrono::steady_clock::now()); });

    bool on = false;
    while (!g_stop.load(std::memory_order_relaxed)) {
        on = !on;
        looper_led.set(on);
        preset_led.set(!on);
        lcd.write_line(0, "LOOP sw: " + std::to_string(looper_presses.load()));
        lcd.write_line(1, "UTIL sw: " + std::to_string(utility_presses.load()));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    looper_switch.stop();
    utility_switch.stop();
    looper_led.set(false);
    preset_led.set(false);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);
    if (argc < 2) {
        return usage();
    }

    const std::string mode = argv[1];
    try {
        if (mode == "lcd") {
            return run_lcd();
        }
        if (mode == "all") {
            return run_all();
        }
        if (argc < 3) {
            return usage();
        }
        const auto line = static_cast<unsigned int>(std::strtoul(argv[2], nullptr, 10));
        if (mode == "led") {
            return run_led(line);
        }
        if (mode == "button") {
            return run_button(line);
        }
        if (mode == "clicks") {
            return run_clicks(line);
        }
        if (mode == "echo") {
            if (argc < 4) {
                return usage();
            }
            const auto led_line = static_cast<unsigned int>(std::strtoul(argv[3], nullptr, 10));
            return run_echo(line, led_line);
        }
        return usage();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

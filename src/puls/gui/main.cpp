// Command puls-gui opens the Puls dashboard. `puls gui` starts it as well.

#include "puls/core/interrupt.hpp"
#include "puls/core/text.hpp"
#include "puls/gui/run.hpp"
#include "puls/ui/terminal.hpp"

#include <exception>
#include <string>
#include <string_view>

#ifndef PULS_VERSION
#define PULS_VERSION "dev"
#endif

int main(int argc, char** argv) {
    puls::ui::ConsoleOutput errors(puls::ui::StandardStream::error);
    bool verbose = false;
#if !defined(__ANDROID__)
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--verbose" || argument == "-verbose") {
            verbose = true;
        } else if (argument.substr(0, 5) == "-psn_") {
            // Older macOS versions pass a process serial number to bundles.
        } else {
            errors.write("Ошибка: неизвестный аргумент " + puls::text::quote(argument) +
                         "\nИспользование: puls-gui [--verbose]\n");
            return 2;
        }
    }
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
#endif
    try {
        puls::CancelScope root{puls::Context()};
        const puls::InterruptHandler interrupts(root);
        puls::service::LogFunc log;
        if (verbose) {
            const puls::ui::Style style(puls::ui::color_enabled(errors, false));
            log = [&errors, style](std::string_view message) {
                errors.write(style.dim("подробно") + "  " + std::string(message) + "\n");
            };
        }
        if (const puls::Error error = puls::gui::run(root.context(), {PULS_VERSION, log})) {
            errors.write("Ошибка: " + error.message() + "\n");
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        errors.write(std::string("Ошибка: ") + error.what() + "\n");
        return 1;
    }
}

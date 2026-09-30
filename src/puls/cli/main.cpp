// Command puls measures Internet latency and throughput using the public
// first-party protocols of the supported measurement services.

#include "puls/cli/application.hpp"
#include "puls/cli/interrupt.hpp"
#include "puls/ui/terminal.hpp"

#include <exception>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#else
#include <csignal>
#endif

#ifndef PULS_VERSION
#define PULS_VERSION "dev"
#endif

namespace {

std::vector<std::string> arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
#if defined(_WIN32)
    // The narrow argv uses the ANSI code page; read UTF-16 arguments instead.
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> result;
    for (int index = 1; wide != nullptr && index < count; ++index) {
        const int size =
            WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, value.data(), size, nullptr, nullptr);
        result.push_back(std::move(value));
    }
    LocalFree(wide);
    return result;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

} // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    // Write errors on closed pipes are reported through return values.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    puls::ui::enable_virtual_terminal();
    puls::ui::ConsoleOutput output(puls::ui::StandardStream::output);
    puls::ui::ConsoleOutput errors(puls::ui::StandardStream::error);
    try {
        puls::CancelScope root{puls::Context()};
        const puls::cli::InterruptHandler interrupts(root);
        puls::cli::Application application(output, errors, PULS_VERSION);
        application.input_terminal = puls::ui::stdin_is_terminal();
        return application.run(root.context(), arguments(argc, argv));
    } catch (const std::exception& error) {
        errors.write(std::string("Ошибка: ") + error.what() + "\n");
        return 1;
    }
}

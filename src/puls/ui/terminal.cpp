#include "puls/ui/terminal.hpp"

#include "puls/core/text.hpp"

#include <algorithm>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace puls::ui {

namespace {

constexpr std::string_view ansi_reset = "\x1b[0m";
constexpr std::string_view ansi_bold = "\x1b[1m";
constexpr std::string_view ansi_dim = "\x1b[2m";
constexpr std::string_view ansi_red = "\x1b[31m";
constexpr std::string_view ansi_green = "\x1b[32m";
constexpr std::string_view ansi_yellow = "\x1b[33m";
constexpr std::string_view ansi_cyan = "\x1b[36m";

#if defined(_WIN32)

HANDLE handle_of(StandardStream stream) {
    return GetStdHandle(stream == StandardStream::output ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}

bool console_handle(HANDLE handle) {
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
}

#else

int descriptor_of(StandardStream stream) {
    return stream == StandardStream::output ? STDOUT_FILENO : STDERR_FILENO;
}

#endif

} // namespace

ConsoleOutput::ConsoleOutput(StandardStream stream) : stream_(stream) {
#if defined(_WIN32)
    terminal_ = console_handle(handle_of(stream));
#else
    terminal_ = isatty(descriptor_of(stream)) == 1;
#endif
}

bool ConsoleOutput::write(std::string_view text) {
    if (text.empty()) {
        return true;
    }
    std::lock_guard lock(mutex_);
#if defined(_WIN32)
    HANDLE handle = handle_of(stream_);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (terminal_) {
        const int size =
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            size);
        const wchar_t* data = wide.data();
        DWORD remaining = static_cast<DWORD>(wide.size());
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteConsoleW(handle, data, remaining, &written, nullptr) || written == 0) {
                return false;
            }
            data += written;
            remaining -= written;
        }
        return true;
    }
    const char* data = text.data();
    DWORD remaining = static_cast<DWORD>(text.size());
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(handle, data, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        remaining -= written;
    }
    return true;
#else
    const int descriptor = descriptor_of(stream_);
    const char* data = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0) {
        const ssize_t written = ::write(descriptor, data, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
#endif
}

std::optional<int> ConsoleOutput::width() const {
    if (!terminal_) {
        return std::nullopt;
    }
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(handle_of(stream_), &info)) {
        return std::nullopt;
    }
    return info.srWindow.Right - info.srWindow.Left + 1;
#else
    winsize size{};
    if (ioctl(descriptor_of(stream_), TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
        return std::nullopt;
    }
    return static_cast<int>(size.ws_col);
#endif
}

bool StringOutput::write(std::string_view text) {
    std::lock_guard lock(mutex_);
    text_ += text;
    return true;
}

std::string StringOutput::text() const {
    std::lock_guard lock(mutex_);
    return text_;
}

bool stdin_is_terminal() {
#if defined(_WIN32)
    return console_handle(GetStdHandle(STD_INPUT_HANDLE));
#else
    return isatty(STDIN_FILENO) == 1;
#endif
}

void enable_virtual_terminal() {
#if defined(_WIN32)
    // The classic console host used by Windows PowerShell and cmd.exe does not
    // interpret escape sequences unless asked to.
    for (const DWORD id : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE handle = GetStdHandle(id);
        DWORD mode = 0;
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) {
            continue;
        }
        if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
            SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
}

bool color_enabled(const Output& output, bool force_off) {
    if (force_off || std::getenv("NO_COLOR") != nullptr) {
        return false;
    }
    if (const char* term = std::getenv("TERM");
        term != nullptr && std::string_view(term) == "dumb") {
        return false;
    }
    return output.is_terminal();
}

int progress_width(const Output& output) {
    if (!output.is_terminal()) {
        return 0;
    }
    const auto columns = output.width();
    if (!columns) {
        return 12;
    }
    if (*columns >= 72) {
        return 18;
    }
    if (*columns >= 48) {
        return 10;
    }
    return 0;
}

std::string Style::wrap(std::string_view code, std::string_view text) const {
    if (!enabled_) {
        return std::string(text);
    }
    std::string result(code);
    result += text;
    result += ansi_reset;
    return result;
}

std::string Style::bold(std::string_view text) const {
    return wrap(ansi_bold, text);
}

std::string Style::dim(std::string_view text) const {
    return wrap(ansi_dim, text);
}

std::string Style::cyan(std::string_view text) const {
    return wrap(ansi_cyan, text);
}

std::string Style::red(std::string_view text) const {
    return wrap(ansi_red, text);
}

std::string Style::green(std::string_view text) const {
    return wrap(ansi_green, text);
}

std::string Style::yellow(std::string_view text) const {
    return wrap(ansi_yellow, text);
}

std::string Style::speed(double mbps) const {
    const std::string text = text::format_fixed(mbps, 2) + " Мбит/с";
    if (mbps >= 50) {
        return wrap(ansi_green, text);
    }
    if (mbps >= 10) {
        return wrap(ansi_yellow, text);
    }
    return wrap(ansi_red, text);
}

std::string Style::latency(double milliseconds, std::string_view unit) const {
    const std::string text = text::format_fixed(milliseconds, 1) + " " + std::string(unit);
    if (milliseconds <= 30) {
        return wrap(ansi_green, text);
    }
    if (milliseconds <= 80) {
        return wrap(ansi_yellow, text);
    }
    return wrap(ansi_red, text);
}

std::string bar(int width, double fraction) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    const int filled = static_cast<int>(fraction * width + 0.5);
    return "[" + text::repeat("█", static_cast<std::size_t>(filled)) +
           text::repeat("░", static_cast<std::size_t>(width - filled)) + "]";
}

void Line::update(std::string_view text) {
    if (!live_) {
        return;
    }
    output_->write("\r\x1b[K" + std::string(text));
}

void Line::finish(std::string_view text) {
    if (live_) {
        output_->write("\r\x1b[K" + std::string(text) + "\n");
    } else {
        output_->write(std::string(text) + "\n");
    }
}

} // namespace puls::ui

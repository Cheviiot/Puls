#include "puls/ui/select.hpp"

#include "puls/core/text.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <termios.h>
#include <unistd.h>
#endif

namespace puls::ui {

const ErrorTag selection_canceled_tag{"ui.ErrCanceled"};

namespace {

// Puts standard input into raw mode for the lifetime of the object.
class RawInput {
public:
    RawInput() {
#if defined(_WIN32)
        handle_ = GetStdHandle(STD_INPUT_HANDLE);
        if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE ||
            !GetConsoleMode(handle_, &saved_)) {
            error_ = Error::make("ui: enter raw mode: стандартный ввод не является консолью");
            return;
        }
        DWORD raw = saved_ & ~(ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT);
        raw |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(handle_, raw)) {
            error_ = Error::make("ui: enter raw mode: не удалось изменить режим консоли");
            return;
        }
        active_ = true;
#else
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) {
            error_ = Error::make(std::string("ui: enter raw mode: ") + std::strerror(errno));
            return;
        }
        termios raw = saved_;
        raw.c_iflag &= ~static_cast<tcflag_t>(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                                              ICRNL | IXON);
        raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        raw.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB);
        raw.c_cflag |= CS8;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            error_ = Error::make(std::string("ui: enter raw mode: ") + std::strerror(errno));
            return;
        }
        active_ = true;
#endif
    }

    RawInput(const RawInput&) = delete;
    RawInput& operator=(const RawInput&) = delete;

    ~RawInput() {
        if (!active_) {
            return;
        }
#if defined(_WIN32)
        SetConsoleMode(handle_, saved_);
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
#endif
    }

    [[nodiscard]] const Error& error() const { return error_; }

    // Reads up to buffer.size() bytes; returns the count or an error.
    Result<std::size_t> read(std::array<unsigned char, 3>& buffer) {
#if defined(_WIN32)
        DWORD count = 0;
        if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            return Error::make("ui: чтение клавиатуры не удалось");
        }
        return static_cast<std::size_t>(count);
#else
        for (;;) {
            const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                return Error::make(std::string("ui: ") + std::strerror(errno));
            }
            if (count == 0) {
                return Error::make("EOF");
            }
            return static_cast<std::size_t>(count);
        }
#endif
    }

private:
#if defined(_WIN32)
    HANDLE handle_ = nullptr;
    DWORD saved_ = 0;
#else
    termios saved_{};
#endif
    bool active_ = false;
    Error error_;
};

} // namespace

Result<std::size_t> select(Output& output, const Style& style, std::string_view title,
                           const std::vector<Option>& options) {
    if (options.empty()) {
        return Error::make("ui: Select called with no options");
    }
    RawInput input;
    if (input.error()) {
        return input.error();
    }

    std::size_t label_width = 0;
    for (const auto& option : options) {
        label_width = std::max(label_width, text::rune_count(option.label));
    }
    // Title, blank line, one line per option, blank line and the hint.
    const std::string frame_lines = std::to_string(options.size() + 4);

    const auto render = [&](std::size_t selected, bool first) {
        std::string frame;
        if (!first) {
            frame += "\x1b[" + frame_lines + "A\r";
        }
        frame += style.bold(title) + "\r\n\r\n";
        for (std::size_t index = 0; index < options.size(); ++index) {
            std::string marker = "  ";
            std::string label = text::pad_right(options[index].label, label_width + 2);
            if (index == selected) {
                marker = style.cyan("› ");
                label = style.bold(label);
            }
            frame += marker + label + style.dim(options[index].hint) + "\r\n";
        }
        frame += "\r\n" + style.dim("↑/↓ выбор · Enter подтвердить · Esc отмена") + "\r\n";
        output.write(frame);
    };
    const auto clear = [&] { output.write("\x1b[" + frame_lines + "A\r\x1b[J"); };
    const auto confirm = [&](std::size_t index) -> Result<std::size_t> {
        clear();
        output.write(style.cyan("›") + " " + style.bold(options[index].label) + "\r\n\r\n");
        return index;
    };
    const auto cancel = [&](Error error) -> Result<std::size_t> {
        clear();
        output.write(style.dim("отменено") + "\r\n\r\n");
        return error;
    };

    std::size_t selected = 0;
    render(selected, true);
    std::array<unsigned char, 3> buffer{};
    for (;;) {
        auto count = input.read(buffer);
        if (!count) {
            return cancel(count.error());
        }
        if (*count == 0) {
            continue;
        }
        const unsigned char key = buffer[0];
        if (key == 3 || key == 'q' || key == 'Q') {
            return cancel(Error::tagged(selection_canceled_tag, "selection canceled"));
        }
        if (key == 13 || key == 10) {
            return confirm(selected);
        }
        if (key >= '1' && key <= '9') {
            const std::size_t index = static_cast<std::size_t>(key - '1');
            if (index < options.size()) {
                return confirm(index);
            }
            continue;
        }
        if (key == 27) {
            if (*count == 1) {
                return cancel(Error::tagged(selection_canceled_tag, "selection canceled"));
            }
            if (*count >= 3 && buffer[1] == '[') {
                if (buffer[2] == 'A') {
                    selected = (selected + options.size() - 1) % options.size();
                    render(selected, false);
                } else if (buffer[2] == 'B') {
                    selected = (selected + 1) % options.size();
                    render(selected, false);
                }
            }
        }
    }
}

} // namespace puls::ui

#pragma once

#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// Terminal output for Puls: ANSI colors, progress bars and live lines that
// degrade to plain one-shot printing for pipes, log files, NO_COLOR and dumb
// terminals.
namespace puls::ui {

// A text destination. Implementations are thread-safe.
class Output {
public:
    virtual ~Output() = default;
    // Writes UTF-8 text; returns false when the destination failed.
    virtual bool write(std::string_view text) = 0;
    [[nodiscard]] virtual bool is_terminal() const { return false; }
    // Terminal width in columns, when known.
    [[nodiscard]] virtual std::optional<int> width() const { return std::nullopt; }
};

enum class StandardStream { output, error };

// Standard output or error. On Windows consoles text is written as UTF-16 so
// that Cyrillic output does not depend on the console code page.
class ConsoleOutput final : public Output {
public:
    explicit ConsoleOutput(StandardStream stream);

    bool write(std::string_view text) override;
    [[nodiscard]] bool is_terminal() const override { return terminal_; }
    [[nodiscard]] std::optional<int> width() const override;

private:
    StandardStream stream_;
    bool terminal_ = false;
    std::mutex mutex_;
};

// Collects output in memory, used by tests.
class StringOutput final : public Output {
public:
    explicit StringOutput(bool terminal = false, std::optional<int> columns = std::nullopt)
        : terminal_(terminal), columns_(columns) {}

    bool write(std::string_view text) override;
    [[nodiscard]] bool is_terminal() const override { return terminal_; }
    [[nodiscard]] std::optional<int> width() const override { return columns_; }
    [[nodiscard]] std::string text() const;

private:
    mutable std::mutex mutex_;
    std::string text_;
    bool terminal_;
    std::optional<int> columns_;
};

// Reports whether standard input is an interactive terminal.
bool stdin_is_terminal();
// Enables ANSI escape processing on Windows consoles; a no-op elsewhere.
void enable_virtual_terminal();

// Colors are used only for terminals without --no-color, NO_COLOR or
// TERM=dumb (https://no-color.org).
bool color_enabled(const Output& output, bool force_off);
// Compact progress-bar width for the output: 18, 10 or 0 columns.
int progress_width(const Output& output);

// Renders text with ANSI styles when enabled; every method is safe to call
// unconditionally.
class Style {
public:
    explicit Style(bool enabled = false) : enabled_(enabled) {}

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] std::string bold(std::string_view text) const;
    [[nodiscard]] std::string dim(std::string_view text) const;
    [[nodiscard]] std::string cyan(std::string_view text) const;
    [[nodiscard]] std::string red(std::string_view text) const;
    [[nodiscard]] std::string green(std::string_view text) const;
    [[nodiscard]] std::string yellow(std::string_view text) const;
    // Mbit/s colored green >= 50, yellow >= 10, red below.
    [[nodiscard]] std::string speed(double mbps) const;
    // Milliseconds colored green <= 30, yellow <= 80, red above.
    [[nodiscard]] std::string latency(double milliseconds, std::string_view unit) const;

private:
    [[nodiscard]] std::string wrap(std::string_view code, std::string_view text) const;

    bool enabled_;
};

// A fixed-width bar like "[█████░░░░░]".
std::string bar(int width, double fraction);

// A line that redraws itself in place when live, and otherwise prints only
// its final text so that redirected output stays clean.
class Line {
public:
    Line(Output& output, bool live) : output_(&output), live_(live) {}

    void update(std::string_view text);
    void finish(std::string_view text);

private:
    Output* output_;
    bool live_;
};

} // namespace puls::ui

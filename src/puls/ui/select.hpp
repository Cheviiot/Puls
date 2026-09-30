#pragma once

#include "puls/core/error.hpp"
#include "puls/ui/terminal.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace puls::ui {

// Returned when the user backs out of a menu (Esc, q or Ctrl+C).
extern const ErrorTag selection_canceled_tag;

struct Option {
    std::string label;
    std::string hint;
};

// Shows an arrow-key menu on output and reads keys from standard input,
// which must be a terminal. ↑/↓ move, Enter confirms, digits 1-9 confirm
// directly; Esc, q and Ctrl+C cancel.
Result<std::size_t> select(Output& output, const Style& style, std::string_view title,
                           const std::vector<Option>& options);

} // namespace puls::ui

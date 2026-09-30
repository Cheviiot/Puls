#pragma once

#include "puls/core/context.hpp"

namespace puls::cli {

// Cancels scope on Ctrl+C, SIGTERM or a console close event. Later signals
// are absorbed so that cleanup can finish. Only one handler may exist.
class InterruptHandler {
public:
    explicit InterruptHandler(CancelScope& scope);
    InterruptHandler(const InterruptHandler&) = delete;
    InterruptHandler& operator=(const InterruptHandler&) = delete;
    ~InterruptHandler();
};

} // namespace puls::cli

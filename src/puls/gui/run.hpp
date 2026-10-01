#pragma once

#include "puls/core/context.hpp"
#include "puls/core/error.hpp"
#include "puls/service/service.hpp"

#include <string>

namespace puls::gui {

struct Options {
    std::string version;
    // Verbose diagnostics; may be called from worker threads.
    service::LogFunc log;
};

// Opens the dashboard and returns when its window is closed or ctx is done.
// An active measurement is canceled before returning.
Error run(const Context& ctx, const Options& options);

} // namespace puls::gui

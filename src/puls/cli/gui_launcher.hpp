#pragma once

#include "puls/cli/application.hpp"

#include <filesystem>
#include <vector>

namespace puls::cli {

// Locations of the graphical application in lookup order: next to the puls
// executable in executable_dir and, on macOS, the application bundles in
// home and /Applications. home may be empty.
std::vector<std::filesystem::path>
gui_executable_candidates(const std::filesystem::path& executable_dir,
                          const std::filesystem::path& home);

// Starts the graphical application installed with this executable. On Unix
// it replaces the current process and returns only on failure; Windows
// starts it as a separate process, like other graphical programs.
Error launch_installed_gui(const Context& ctx, const GuiOptions& options);

} // namespace puls::cli

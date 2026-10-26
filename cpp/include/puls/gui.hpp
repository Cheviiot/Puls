#pragma once

#include "puls/core.hpp"

class QWidget;

namespace puls {
// The caller owns the window. A QApplication must already exist.
QWidget* createMainWindow(const Config& initial = {}, BackendFactory factory = makeBackend);
int runGUI(int& argc, char** argv, const Config& initial);
} // namespace puls

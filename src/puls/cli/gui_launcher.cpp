#include "puls/cli/gui_launcher.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace puls::cli {

namespace {

namespace fs = std::filesystem;

std::string display_path(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// Directory of the running executable with symbolic links resolved.
Result<fs::path> executable_directory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return Error::make("не удалось определить путь к puls: " +
                               std::system_category().message(static_cast<int>(GetLastError())));
        }
        if (size < buffer.size()) {
            buffer.resize(size);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
#else
    std::error_code error;
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return Error::make("не удалось определить путь к puls");
    }
    buffer.resize(std::strlen(buffer.c_str()));
    const fs::path executable = fs::canonical(buffer, error);
#else
    const fs::path executable = fs::canonical("/proc/self/exe", error);
#endif
    if (error) {
        return Error::make("не удалось определить путь к puls: " + error.message());
    }
    return executable.parent_path();
#endif
}

bool is_executable(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        return false;
    }
#if defined(_WIN32)
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}

#if defined(_WIN32)

Error start_process(const fs::path& program, bool verbose) {
    std::wstring command_line = L"\"" + program.wstring() + L"\"";
    if (verbose) {
        command_line += L" --verbose";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    BOOL inherit = FALSE;
    if (verbose) {
        // Diagnostics go to the same destination as those of puls.
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        for (HANDLE handle : {startup.hStdInput, startup.hStdOutput, startup.hStdError}) {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            }
        }
        inherit = TRUE;
    }
    PROCESS_INFORMATION process{};
    if (CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, inherit, 0, nullptr,
                       nullptr, &startup, &process) == 0) {
        return Error::make("не удалось запустить " + display_path(program) + ": " +
                           std::system_category().message(static_cast<int>(GetLastError())));
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return {};
}

#else

Error replace_process(const fs::path& program, bool verbose) {
    std::string path = program.string();
    std::string verbose_flag = "--verbose";
    std::vector<char*> arguments{path.data()};
    if (verbose) {
        arguments.push_back(verbose_flag.data());
    }
    arguments.push_back(nullptr);
    std::fflush(nullptr);
    ::execv(path.c_str(), arguments.data());
    const int code = errno;
    return Error::make("не удалось запустить " + path + ": " +
                       std::generic_category().message(code));
}

#endif

} // namespace

std::vector<fs::path> gui_executable_candidates(const fs::path& executable_dir,
                                                [[maybe_unused]] const fs::path& home) {
#if defined(_WIN32)
    return {executable_dir / "puls-gui.exe"};
#else
    std::vector<fs::path> result{executable_dir / "puls-gui"};
#if defined(__APPLE__)
    const fs::path bundle_executable = fs::path("Puls.app") / "Contents" / "MacOS" / "Puls";
    result.push_back(executable_dir / bundle_executable);
    if (!home.empty()) {
        result.push_back(home / "Applications" / bundle_executable);
    }
    result.push_back(fs::path("/Applications") / bundle_executable);
#endif
    return result;
#endif
}

Error launch_installed_gui(const Context& ctx, const GuiOptions& options) {
    if (ctx.done()) {
        return {};
    }
    auto directory = executable_directory();
    if (!directory) {
        return std::move(directory).error();
    }
    const char* home = std::getenv("HOME");
    fs::path program;
    for (const fs::path& candidate :
         gui_executable_candidates(*directory, home != nullptr ? fs::path(home) : fs::path())) {
        if (is_executable(candidate)) {
            program = candidate;
            break;
        }
    }
    if (program.empty()) {
        return Error::tagged(gui_unavailable_tag,
                             "графический интерфейс не установлен: не найден puls-gui");
    }
    const bool verbose = static_cast<bool>(options.log);
    if (verbose) {
        options.log("графический интерфейс: " + display_path(program));
    }
#if defined(_WIN32)
    return start_process(program, verbose);
#else
    return replace_process(program, verbose);
#endif
}

} // namespace puls::cli

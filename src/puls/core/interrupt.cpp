#include "puls/core/interrupt.hpp"

#include <atomic>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#endif

namespace puls {

namespace {

std::atomic<CancelScope*> target{nullptr};

#if defined(_WIN32)

BOOL WINAPI on_console_event(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        if (CancelScope* scope = target.load()) {
            scope->cancel();
        }
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (CancelScope* scope = target.load()) {
            scope->cancel();
        }
        // The process ends when this handler returns; leave a moment for the
        // measurement to close its connections.
        Sleep(1000);
        return TRUE;
    default:
        return FALSE;
    }
}

#else

// Signal handlers may only use async-signal-safe calls, so they write to a
// pipe that a watcher thread turns into a cancellation.
int signal_pipe[2] = {-1, -1};
int stop_pipe[2] = {-1, -1};
struct sigaction previous_interrupt {};
struct sigaction previous_terminate {};
std::thread watcher;

extern "C" void on_signal(int) {
    const int saved = errno;
    const char byte = 1;
    [[maybe_unused]] const ssize_t written = ::write(signal_pipe[1], &byte, 1);
    errno = saved;
}

bool open_pipe(int descriptors[2]) {
    if (::pipe(descriptors) != 0) {
        return false;
    }
    for (int index = 0; index < 2; ++index) {
        ::fcntl(descriptors[index], F_SETFD, FD_CLOEXEC);
    }
    ::fcntl(descriptors[1], F_SETFL, ::fcntl(descriptors[1], F_GETFL) | O_NONBLOCK);
    return true;
}

void close_pipe(int descriptors[2]) {
    for (int index = 0; index < 2; ++index) {
        if (descriptors[index] >= 0) {
            ::close(descriptors[index]);
            descriptors[index] = -1;
        }
    }
}

#endif

} // namespace

InterruptHandler::InterruptHandler(CancelScope& scope) {
    target.store(&scope);
#if defined(_WIN32)
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    if (!open_pipe(signal_pipe) || !open_pipe(stop_pipe)) {
        close_pipe(signal_pipe);
        close_pipe(stop_pipe);
        return;
    }
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, &previous_interrupt);
    sigaction(SIGTERM, &action, &previous_terminate);
    watcher = std::thread([] {
        pollfd descriptors[2] = {{signal_pipe[0], POLLIN, 0}, {stop_pipe[0], POLLIN, 0}};
        for (;;) {
            if (::poll(descriptors, 2, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (descriptors[1].revents != 0) {
                return;
            }
            if ((descriptors[0].revents & POLLIN) != 0) {
                char buffer[16];
                [[maybe_unused]] const ssize_t count =
                    ::read(signal_pipe[0], buffer, sizeof buffer);
                if (CancelScope* current = target.load()) {
                    current->cancel();
                }
            }
        }
    });
#endif
}

InterruptHandler::~InterruptHandler() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(on_console_event, FALSE);
#else
    if (watcher.joinable()) {
        const char byte = 1;
        [[maybe_unused]] const ssize_t written = ::write(stop_pipe[1], &byte, 1);
        watcher.join();
        sigaction(SIGINT, &previous_interrupt, nullptr);
        sigaction(SIGTERM, &previous_terminate, nullptr);
    }
    close_pipe(signal_pipe);
    close_pipe(stop_pipe);
#endif
    target.store(nullptr);
}

} // namespace puls

#include "common/terminal.hpp"

#include <iostream>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>

namespace mtfs {

std::string readPassword(const std::string& prompt) {
    std::string password;
    bool interactive = isatty(STDIN_FILENO) != 0;
    termios saved{};

    if (interactive) {
        std::cerr << prompt << std::flush;
        tcgetattr(STDIN_FILENO, &saved);
        termios silent = saved;
        silent.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &silent);
    }
    bool ok = static_cast<bool>(std::getline(std::cin, password));
    if (interactive) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        std::cerr << '\n';
    }
    if (!ok) throw std::runtime_error("no password given");
    if (!password.empty() && password.back() == '\r') password.pop_back();
    return password;
}

}  // namespace mtfs

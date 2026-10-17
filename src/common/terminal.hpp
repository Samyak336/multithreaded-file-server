#pragma once

#include <string>

namespace mtfs {

// Reads a password. On a terminal the prompt goes to stderr and the typed
// characters are not echoed; when stdin is a pipe (scripts, CI) it simply
// reads the next line. Throws std::runtime_error at end of input.
std::string readPassword(const std::string& prompt);

}  // namespace mtfs

#pragma once

#include <optional>
#include <string>

namespace mtfs {

// Roles are ordered: each role may do everything the one before it may.
//   reader  list, stat, download
//   writer  + upload, append, copy, rename
//   admin   + delete
enum class Role { Reader = 0, Writer = 1, Admin = 2 };

enum class Action { List, Stat, Get, Put, Append, Copy, Rename, Delete };

std::optional<Role> parseRole(const std::string& text);
const char* roleName(Role role);
Role minimumRole(Action action);

inline bool isAllowed(Role role, Action action) {
    return static_cast<int>(role) >= static_cast<int>(minimumRole(action));
}

}  // namespace mtfs

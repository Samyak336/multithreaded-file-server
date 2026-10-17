#include "auth/role.hpp"

namespace mtfs {

std::optional<Role> parseRole(const std::string& text) {
    if (text == "reader") return Role::Reader;
    if (text == "writer") return Role::Writer;
    if (text == "admin") return Role::Admin;
    return std::nullopt;
}

const char* roleName(Role role) {
    switch (role) {
        case Role::Reader: return "reader";
        case Role::Writer: return "writer";
        case Role::Admin: return "admin";
    }
    return "unknown";
}

Role minimumRole(Action action) {
    switch (action) {
        case Action::List:
        case Action::Stat:
        case Action::Get:
            return Role::Reader;
        case Action::Put:
        case Action::Append:
        case Action::Copy:
        case Action::Rename:
            return Role::Writer;
        case Action::Delete:
            return Role::Admin;
    }
    return Role::Admin;  // unreachable; an unknown action is admin-only (deny by default)
}

}  // namespace mtfs

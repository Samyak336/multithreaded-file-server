// mtfs-user: manage the server's user database from the command line.
//
//   mtfs-user [--db FILE] add <user> <role>
//   mtfs-user [--db FILE] passwd <user>
//   mtfs-user [--db FILE] role <user> <role>
//   mtfs-user [--db FILE] del <user>
//   mtfs-user [--db FILE] list
//
// It edits the file directly, so the server does not need to be running.
// The server re-reads the file on every login, so changes apply immediately.

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

#include "auth/user_store.hpp"
#include "common/fd.hpp"
#include "common/terminal.hpp"

using namespace mtfs;

namespace {

void usage() {
    std::cerr << "usage: mtfs-user [--db FILE] <command>\n"
                 "  add <user> <role>     create a user (roles: reader, writer, admin)\n"
                 "  passwd <user>         set a new password\n"
                 "  role <user> <role>    change a user's role\n"
                 "  del <user>            delete a user\n"
                 "  list                  show all users and their roles\n"
                 "Passwords are read from the terminal without echo, or from stdin when piped.\n"
                 "The default database is data/users.db.\n";
}

Role requireRole(const std::string& text) {
    std::optional<Role> role = parseRole(text);
    if (!role) throw std::invalid_argument("unknown role '" + text + "' (use reader, writer or admin)");
    return *role;
}

std::string newPassword() {
    std::string password = readPassword("new password: ");
    if (isatty(STDIN_FILENO) != 0 && readPassword("repeat password: ") != password)
        throw std::invalid_argument("the passwords do not match");
    return password;
}

// Every command is load, modify, save. Two of them running at once could
// interleave and lose one update, so they take an exclusive flock() on a
// lock file first. The lock is released when the returned fd is closed.
UniqueFd lockDatabase(const std::string& db) {
    std::string lockPath = db + ".lock";
    UniqueFd fd(::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!fd.valid()) throw std::runtime_error("cannot open " + lockPath + ": " + std::strerror(errno));
    while (::flock(fd.get(), LOCK_EX) != 0) {
        if (errno != EINTR) throw std::runtime_error("cannot lock " + lockPath + ": " + std::strerror(errno));
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string db = "data/users.db";
    if (args.size() >= 2 && args[0] == "--db") {
        db = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() ? 2 : 0;
    }
    std::string cmd = args[0];
    args.erase(args.begin());

    try {
        std::filesystem::path parent = std::filesystem::path(db).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        UniqueFd lock = lockDatabase(db);
        UserStore store(db);
        store.load();

        if (cmd == "list" && args.empty()) {
            for (const UserRecord& u : store.all()) std::cout << u.name << "\t" << roleName(u.role) << "\n";
            if (store.empty()) std::cout << "(no users)\n";
            return 0;
        }

        std::string message;
        if (cmd == "add" && args.size() == 2) {
            Role role = requireRole(args[1]);
            // Validate before prompting, so nobody types a password for a doomed request.
            if (!UserStore::isValidName(args[0]))
                throw std::invalid_argument("invalid user name '" + args[0] + "' (use 1-32 letters, digits, '.', '_' or '-')");
            if (store.find(args[0]) != nullptr) throw std::invalid_argument("user '" + args[0] + "' already exists");
            store.add(args[0], newPassword(), role);
            message = "added " + args[0] + " (" + roleName(role) + ")";
        } else if (cmd == "passwd" && args.size() == 1) {
            if (store.find(args[0]) == nullptr) throw std::invalid_argument("no such user '" + args[0] + "'");
            store.setPassword(args[0], newPassword());
            message = "password changed for " + args[0];
        } else if (cmd == "role" && args.size() == 2) {
            Role role = requireRole(args[1]);
            store.setRole(args[0], role);
            message = args[0] + " is now " + roleName(role);
        } else if (cmd == "del" && args.size() == 1) {
            store.remove(args[0]);
            message = "deleted " + args[0];
        } else {
            usage();
            return 2;
        }
        store.save();
        std::cout << message << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "mtfs-user: " << e.what() << "\n";
        return 1;
    }
}

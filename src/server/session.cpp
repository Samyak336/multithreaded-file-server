#include "server/session.hpp"

#include <system_error>

#include "auth/user_store.hpp"
#include "common/log.hpp"
#include "server/file_store.hpp"
#include "server/lock_manager.hpp"

namespace mtfs {

using proto::Status;
using proto::StatusError;

namespace {

constexpr int kMaxLoginAttempts = 3;
constexpr const char* kHelp = "commands: LOGIN WHOAMI LIST STAT GET PUT APPEND COPY RENAME DELETE HELP QUIT";

void requireName(const std::string& name) {
    if (!FileStore::isValidName(name))
        throw StatusError(Status::BadRequest, "invalid file name '" + name +
                                                  "' (use letters, digits, '.', '_' or '-'; no leading '.')");
}

}  // namespace

// Every file command goes through this table. The dispatcher checks the
// argument count and the caller's role before any handler runs, so no
// handler can forget a permission check.
struct Session::Command {
    const char* verb;
    Action action;
    std::size_t argc;
    void (Session::*handler)(const Args&);
    const char* usage;
};

Session::Session(Connection& conn, ServerContext& ctx, std::string peer)
    : conn_(conn), ctx_(ctx), peer_(std::move(peer)) {}

std::string Session::who() const { return user_ ? *user_ + "@" + peer_ : peer_; }

void Session::run() {
    std::string line;
    while (!done_ && conn_.readLine(line, proto::kMaxLineLength)) {
        proto::Request req = proto::parseRequest(line);
        if (req.verb.empty()) continue;
        // Log only the verb: the full line of a LOGIN contains the password.
        Log::debug(who() + ": " + req.verb);
        try {
            dispatch(req);
        } catch (const StatusError& e) {
            Log::debug(who() + ": " + req.verb + " -> " + std::to_string(e.code()) + " " + e.what());
            reply(proto::errorLine(e.status(), e.what()));
        } catch (const std::system_error& e) {
            Log::error(who() + ": " + req.verb + " failed: " + e.what());
            reply(proto::errorLine(Status::Internal, "internal server error"));
        }
    }
}

void Session::dispatch(const proto::Request& req) {
    static const Command kCommands[] = {
        {"LIST", Action::List, 0, &Session::doList, "LIST"},
        {"STAT", Action::Stat, 1, &Session::doStat, "STAT <file>"},
        {"GET", Action::Get, 1, &Session::doGet, "GET <file>"},
        {"PUT", Action::Put, 2, &Session::doPut, "PUT <file> <bytes>"},
        {"APPEND", Action::Append, 2, &Session::doAppend, "APPEND <file> <bytes>"},
        {"COPY", Action::Copy, 2, &Session::doCopy, "COPY <source> <target>"},
        {"RENAME", Action::Rename, 2, &Session::doRename, "RENAME <source> <target>"},
        {"DELETE", Action::Delete, 1, &Session::doDelete, "DELETE <file>"},
    };

    if (req.verb == "LOGIN") return handleLogin(req);
    if (req.verb == "HELP") return reply(proto::okLine(kHelp));
    if (req.verb == "QUIT") {
        done_ = true;
        return reply(proto::okLine("bye"));
    }

    const Command* cmd = nullptr;
    for (const Command& c : kCommands) {
        if (req.verb == c.verb) cmd = &c;
    }
    if (cmd == nullptr && req.verb != "WHOAMI")
        throw StatusError(Status::BadRequest, "unknown command '" + req.verb + "' (try HELP)");
    if (!user_) throw StatusError(Status::Unauthorized, "login required");
    if (cmd == nullptr) return reply(proto::okLine(*user_ + " " + roleName(role_)));  // WHOAMI

    if (req.args.size() != cmd->argc) throw StatusError(Status::BadRequest, std::string("usage: ") + cmd->usage);
    if (!isAllowed(role_, cmd->action)) {
        throw StatusError(Status::Forbidden, std::string(cmd->verb) + " needs the " +
                                                 roleName(minimumRole(cmd->action)) + " role; you are " +
                                                 roleName(role_));
    }
    (this->*cmd->handler)(req.args);
}

void Session::handleLogin(const proto::Request& req) {
    // LOGIN <user> <password>. The password is the rest of the line, so it may contain spaces.
    std::size_t space = req.rest.find(' ');
    if (space == std::string::npos || space == 0) throw StatusError(Status::BadRequest, "usage: LOGIN <user> <password>");
    if (user_) throw StatusError(Status::BadRequest, "already logged in as " + *user_);
    std::string name = req.rest.substr(0, space);
    std::string password = req.rest.substr(space + 1);

    std::optional<Role> role;
    try {
        // Re-read on every login, so changes made with mtfs-user take effect
        // without restarting the server. The file is small and logins are rare.
        UserStore users(ctx_.usersFile);
        users.load();
        role = users.verify(name, password);
    } catch (const std::exception& e) {
        Log::error(std::string("cannot load the user database: ") + e.what());
        throw StatusError(Status::Internal, "authentication is unavailable");
    }

    if (!role) {
        ++failedLogins_;
        Log::warn(peer_ + ": failed login for '" + name + "' (attempt " + std::to_string(failedLogins_) + ")");
        if (failedLogins_ >= kMaxLoginAttempts) {
            done_ = true;  // dropping the connection slows down password guessing
            throw StatusError(Status::Unauthorized, "too many failed logins; closing the connection");
        }
        throw StatusError(Status::Unauthorized, "invalid user name or password");
    }

    user_ = name;
    role_ = *role;
    Log::info(peer_ + ": logged in as " + name + " (" + roleName(role_) + ")");
    reply(proto::okLine(roleName(role_)));
}

void Session::doList(const Args&) {
    // No file locks: a directory listing is a point-in-time snapshot.
    std::string body;
    for (const FileInfo& f : ctx_.store.list())
        body += f.name + " " + std::to_string(f.size) + " " + std::to_string(f.mtime) + "\n";
    reply(proto::dataLine(body.size()));
    conn_.writeAll(body.data(), body.size());
}

void Session::doStat(const Args& args) {
    requireName(args[0]);
    FileInfo f;
    {
        // The read lock keeps STAT from seeing a file halfway through an APPEND.
        LockManager::Guard lock = ctx_.locks.acquire(args[0], LockMode::Read);
        f = ctx_.store.info(args[0]);
    }
    reply(proto::okLine(std::to_string(f.size) + " " + std::to_string(f.mtime)));
}

void Session::doGet(const Args& args) {
    const std::string& name = args[0];
    requireName(name);
    // The read lock is held for the whole transfer. Concurrent GETs share it,
    // while writers to this file wait until the download is complete.
    LockManager::Guard lock = ctx_.locks.acquire(name, LockMode::Read);
    std::uint64_t size = 0;
    UniqueFd file = ctx_.store.openForRead(name, size);
    reply(proto::dataLine(size));
    conn_.writeFromFd(file.get(), size);
}

std::uint64_t Session::parseUploadSize(const std::string& text) const {
    std::uint64_t size = 0;
    if (!proto::parseSize(text, size)) throw StatusError(Status::BadRequest, "invalid size '" + text + "'");
    if (size > ctx_.maxUploadBytes)
        throw StatusError(Status::PayloadTooLarge, "uploads are limited to " + std::to_string(ctx_.maxUploadBytes) + " bytes");
    return size;
}

void Session::doPut(const Args& args) {
    const std::string& name = args[0];
    requireName(name);
    std::uint64_t size = parseUploadSize(args[1]);
    TempFile temp = ctx_.store.createTemp();  // before CONTINUE, so a disk error is reported up front

    reply(proto::continueLine());
    // The data goes into the temp file while no lock is held: a slow upload
    // never blocks other clients. Only the final rename needs the write lock.
    conn_.readToFd(temp.fd(), size);
    {
        LockManager::Guard lock = ctx_.locks.acquire(name, LockMode::Write);
        ctx_.store.commit(temp, name);
    }
    Log::info(who() + ": stored " + name + " (" + std::to_string(size) + " bytes)");
    reply(proto::okLine("stored " + std::to_string(size) + " bytes"));
}

void Session::doAppend(const Args& args) {
    const std::string& name = args[0];
    requireName(name);
    std::uint64_t size = parseUploadSize(args[1]);
    // Fail fast before the client sends anything. This check is only a hint;
    // appendFrom() checks again while holding the lock.
    if (!ctx_.store.exists(name)) throw StatusError(Status::NotFound, "no such file: " + name);
    TempFile temp = ctx_.store.createTemp();

    reply(proto::continueLine());
    // Buffer the data first, so the write lock covers only local disk I/O,
    // never a network transfer.
    conn_.readToFd(temp.fd(), size);
    {
        LockManager::Guard lock = ctx_.locks.acquire(name, LockMode::Write);
        ctx_.store.appendFrom(temp, name);
    }
    reply(proto::okLine("appended " + std::to_string(size) + " bytes"));
}

void Session::doCopy(const Args& args) {
    requireName(args[0]);
    requireName(args[1]);
    {
        // A read lock on the source and a write lock on the target, taken in
        // global name order. COPY a b racing COPY b a cannot deadlock.
        LockManager::Guard locks =
            ctx_.locks.acquireAll({{args[0], LockMode::Read}, {args[1], LockMode::Write}});
        ctx_.store.copy(args[0], args[1]);
    }
    reply(proto::okLine("copied " + args[0] + " to " + args[1]));
}

void Session::doRename(const Args& args) {
    requireName(args[0]);
    requireName(args[1]);
    {
        LockManager::Guard locks =
            ctx_.locks.acquireAll({{args[0], LockMode::Write}, {args[1], LockMode::Write}});
        ctx_.store.rename(args[0], args[1]);
    }
    Log::info(who() + ": renamed " + args[0] + " to " + args[1]);
    reply(proto::okLine("renamed " + args[0] + " to " + args[1]));
}

void Session::doDelete(const Args& args) {
    requireName(args[0]);
    {
        LockManager::Guard lock = ctx_.locks.acquire(args[0], LockMode::Write);
        ctx_.store.remove(args[0]);
    }
    Log::info(who() + ": deleted " + args[0]);
    reply(proto::okLine("deleted " + args[0]));
}

}  // namespace mtfs

#include "auth/user_store.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/crypto.hpp"
#include "common/fd.hpp"

namespace mtfs {

namespace {

constexpr std::size_t kMaxNameLength = 32;
constexpr std::size_t kMaxPasswordLength = 1024;

std::vector<std::string> splitFields(const std::string& line, char separator) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        std::size_t end = line.find(separator, start);
        fields.push_back(line.substr(start, end - start));
        if (end == std::string::npos) return fields;
        start = end + 1;
    }
}

bool isHex(const std::string& text, std::size_t length) {
    std::string ignored;
    return text.size() == length && crypto::fromHex(text, ignored);
}

}  // namespace

UserStore::UserStore(std::string path) : path_(std::move(path)) {}

bool UserStore::isValidName(const std::string& name) {
    if (name.empty() || name.size() > kMaxNameLength) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

void UserStore::checkPassword(const std::string& password) {
    if (password.empty()) throw std::invalid_argument("password must not be empty");
    if (password.size() > kMaxPasswordLength) throw std::invalid_argument("password is too long");
    for (char c : password) {
        if (c == '\n' || c == '\r' || c == '\0') throw std::invalid_argument("password contains a control character");
    }
}

void UserStore::load() {
    users_.clear();
    struct stat st {};
    if (::stat(path_.c_str(), &st) != 0) {
        if (errno == ENOENT) return;  // no database yet
        throw std::runtime_error("cannot read " + path_ + ": " + std::strerror(errno));
    }
    std::ifstream in(path_);
    if (!in) throw std::runtime_error("cannot open " + path_);

    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> f = splitFields(line, ':');
        std::optional<Role> role = f.size() == 4 ? parseRole(f[1]) : std::nullopt;
        bool valid = f.size() == 4 && isValidName(f[0]) && role && isHex(f[2], 2 * crypto::kSaltBytes) &&
                     isHex(f[3], 64);
        std::string where = path_ + ":" + std::to_string(lineNumber);
        if (!valid) throw std::runtime_error(where + ": malformed user record");
        if (users_.count(f[0]) != 0) throw std::runtime_error(where + ": duplicate user '" + f[0] + "'");
        users_[f[0]] = UserRecord{f[0], *role, f[2], f[3]};
    }
}

void UserStore::save() const {
    std::filesystem::path parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);

    std::string contents = "# mtfs users. Format: name:role:salt:sha256(salt || password)\n";
    for (const auto& entry : users_) {
        const UserRecord& u = entry.second;
        contents += u.name + ":" + roleName(u.role) + ":" + u.saltHex + ":" + u.hashHex + "\n";
    }

    std::string temp = path_ + ".tmp";
    {
        // 0600 from the start: the hashes are never readable by other users.
        UniqueFd fd(::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        if (!fd.valid()) throw std::runtime_error("cannot write " + temp + ": " + std::strerror(errno));
        writeFully(fd.get(), contents.data(), contents.size());
        if (::fsync(fd.get()) != 0) throw std::runtime_error("fsync " + temp + ": " + std::strerror(errno));
    }
    if (std::rename(temp.c_str(), path_.c_str()) != 0)
        throw std::runtime_error("cannot replace " + path_ + ": " + std::strerror(errno));
}

const UserRecord* UserStore::find(const std::string& name) const {
    auto it = users_.find(name);
    return it == users_.end() ? nullptr : &it->second;
}

std::vector<UserRecord> UserStore::all() const {
    std::vector<UserRecord> list;
    for (const auto& entry : users_) list.push_back(entry.second);
    return list;
}

UserRecord& UserStore::require(const std::string& name) {
    auto it = users_.find(name);
    if (it == users_.end()) throw std::invalid_argument("no such user '" + name + "'");
    return it->second;
}

void UserStore::add(const std::string& name, const std::string& password, Role role) {
    if (!isValidName(name))
        throw std::invalid_argument("invalid user name '" + name + "' (use 1-32 letters, digits, '.', '_' or '-')");
    if (users_.count(name) != 0) throw std::invalid_argument("user '" + name + "' already exists");
    checkPassword(password);
    std::string salt = crypto::randomHex(crypto::kSaltBytes);
    users_[name] = UserRecord{name, role, salt, crypto::hashPassword(salt, password)};
}

void UserStore::remove(const std::string& name) {
    require(name);
    users_.erase(name);
}

void UserStore::setPassword(const std::string& name, const std::string& password) {
    UserRecord& user = require(name);
    checkPassword(password);
    user.saltHex = crypto::randomHex(crypto::kSaltBytes);  // fresh salt for every new password
    user.hashHex = crypto::hashPassword(user.saltHex, password);
}

void UserStore::setRole(const std::string& name, Role role) { require(name).role = role; }

std::optional<Role> UserStore::verify(const std::string& name, const std::string& password) const {
    auto it = users_.find(name);
    if (it == users_.end()) {
        // Hash anyway, so an unknown user name takes as long as a wrong
        // password and response times don't reveal which names exist.
        (void)crypto::hashPassword(std::string(2 * crypto::kSaltBytes, '0'), password);
        return std::nullopt;
    }
    const UserRecord& user = it->second;
    if (!crypto::constantTimeEquals(crypto::hashPassword(user.saltHex, password), user.hashHex)) return std::nullopt;
    return user.role;
}

}  // namespace mtfs

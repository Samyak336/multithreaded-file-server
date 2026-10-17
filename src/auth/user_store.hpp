#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "auth/role.hpp"

namespace mtfs {

struct UserRecord {
    std::string name;
    Role role = Role::Reader;
    std::string saltHex;  // 16 random bytes, hex-encoded
    std::string hashHex;  // SHA-256(salt || password), hex-encoded
};

// The user database: a text file with one "name:role:salt:hash" line per
// user. Only salted hashes are stored, never passwords.
class UserStore {
public:
    explicit UserStore(std::string path);

    // A missing file is an empty database. A malformed line throws
    // std::runtime_error naming the line.
    void load();
    // Writes a temporary file, fsyncs it and renames it over the old one.
    // rename() is atomic, so a reader (the server re-reads the file on every
    // login) sees either the old database or the new one, never half of one.
    void save() const;

    const UserRecord* find(const std::string& name) const;
    std::vector<UserRecord> all() const;  // sorted by name
    bool empty() const { return users_.empty(); }

    // These throw std::invalid_argument for bad names, passwords or unknown users.
    void add(const std::string& name, const std::string& password, Role role);
    void remove(const std::string& name);
    void setPassword(const std::string& name, const std::string& password);
    void setRole(const std::string& name, Role role);

    // The user's role if the password is correct.
    std::optional<Role> verify(const std::string& name, const std::string& password) const;

    static bool isValidName(const std::string& name);  // 1-32 of [A-Za-z0-9._-]
    static void checkPassword(const std::string& password);

private:
    UserRecord& require(const std::string& name);

    std::string path_;
    std::map<std::string, UserRecord> users_;
};

}  // namespace mtfs

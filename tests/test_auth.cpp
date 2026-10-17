#include <string>

#include "auth/crypto.hpp"
#include "auth/role.hpp"
#include "auth/user_store.hpp"
#include "check.hpp"

using namespace mtfs;

namespace {

void testSha256KnownAnswers() {
    // Test vectors from FIPS 180-2.
    CHECK(crypto::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(crypto::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

void testHex() {
    const unsigned char bytes[] = {0x00, 0x7f, 0xa5, 0xff};
    CHECK(crypto::toHex(bytes, sizeof bytes) == "007fa5ff");
    std::string back;
    CHECK(crypto::fromHex("007FA5ff", back) && back == std::string("\x00\x7f\xa5\xff", 4));
    CHECK(!crypto::fromHex("abc", back));  // odd length
    CHECK(!crypto::fromHex("zz", back));
}

void testSaltedHashing() {
    std::string salt1 = crypto::randomHex(crypto::kSaltBytes);
    std::string salt2 = crypto::randomHex(crypto::kSaltBytes);
    CHECK(salt1.size() == 2 * crypto::kSaltBytes);
    CHECK(salt1 != salt2);
    // Same password, different salts: different hashes.
    CHECK(crypto::hashPassword(salt1, "hunter2") != crypto::hashPassword(salt2, "hunter2"));
    CHECK(crypto::hashPassword(salt1, "hunter2") == crypto::hashPassword(salt1, "hunter2"));
    CHECK(crypto::constantTimeEquals("abc", "abc"));
    CHECK(!crypto::constantTimeEquals("abc", "abd"));
    CHECK(!crypto::constantTimeEquals("abc", "ab"));
}

void testRoles() {
    CHECK(isAllowed(Role::Reader, Action::Get));
    CHECK(isAllowed(Role::Reader, Action::List));
    CHECK(!isAllowed(Role::Reader, Action::Put));
    CHECK(isAllowed(Role::Writer, Action::Append));
    CHECK(isAllowed(Role::Writer, Action::Rename));
    CHECK(!isAllowed(Role::Writer, Action::Delete));
    CHECK(isAllowed(Role::Admin, Action::Delete));
    CHECK(parseRole("writer") == Role::Writer);
    CHECK(!parseRole("root"));
    CHECK(std::string(roleName(Role::Admin)) == "admin");
}

void testUserStoreRoundTrip() {
    check::TempDir dir;
    std::string db = dir.path + "/nested/users.db";
    {
        UserStore store(db);
        store.load();  // missing file: empty database
        CHECK(store.empty());
        store.add("alice", "wonder land", Role::Writer);
        store.add("bob", "builder", Role::Reader);
        CHECK_THROWS(store.add("alice", "again", Role::Admin));  // duplicate
        CHECK_THROWS(store.add("bad name", "pw", Role::Reader));
        CHECK_THROWS(store.add("a:b", "pw", Role::Reader));    // ':' would break the file format
        CHECK_THROWS(store.add("carol", "", Role::Reader));     // empty password
        store.save();
    }

    UserStore store(db);
    store.load();
    CHECK(store.all().size() == 2);
    CHECK(store.verify("alice", "wonder land") == Role::Writer);
    CHECK(!store.verify("alice", "wonder"));
    CHECK(!store.verify("mallory", "wonder land"));

    std::string contents = check::readFile(db);
    CHECK(contents.find("wonder") == std::string::npos);  // only hashes on disk

    store.setRole("bob", Role::Admin);
    store.setPassword("bob", "new password");
    CHECK(store.verify("bob", "new password") == Role::Admin);
    CHECK(!store.verify("bob", "builder"));
    store.remove("alice");
    CHECK(store.find("alice") == nullptr);
    CHECK_THROWS(store.remove("alice"));
    CHECK_THROWS(store.setRole("nobody", Role::Admin));
}

void testMalformedDatabaseIsRejected() {
    check::TempDir dir;
    std::string db = dir.path + "/users.db";
    check::writeFile(db, "# comment\nalice:writer:nothex:alsonothex\n");
    UserStore store(db);
    CHECK_THROWS(store.load());

    check::writeFile(db, "alice:superuser:00000000000000000000000000000000:" + std::string(64, '0') + "\n");
    CHECK_THROWS(store.load());  // unknown role
}

}  // namespace

int main() {
    testSha256KnownAnswers();
    testHex();
    testSaltedHashing();
    testRoles();
    testUserStoreRoundTrip();
    testMalformedDatabaseIsRejected();
    return check::summary("test_auth");
}

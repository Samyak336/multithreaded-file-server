// End-to-end tests: a real server on an ephemeral port, real TCP clients.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "auth/user_store.hpp"
#include "check.hpp"
#include "client/client.hpp"
#include "common/log.hpp"
#include "server/server.hpp"

using namespace mtfs;
using namespace std::chrono_literals;
using proto::Status;

namespace {

class TestServer {
public:
    explicit TestServer(std::size_t workers = 32, std::size_t queue = 64, int idleTimeout = 30,
                        std::uint64_t maxUpload = 4u << 20) {
        std::string db = dir.path + "/users.db";
        UserStore users(db);
        users.add("rita", "reader-pw", Role::Reader);
        users.add("walt", "writer-pw", Role::Writer);
        users.add("ada", "admin-pw", Role::Admin);
        users.save();

        ServerConfig config;
        config.host = "127.0.0.1";
        config.port = 0;  // any free port
        config.root = dir.path + "/files";
        config.usersFile = db;
        config.workers = workers;
        config.queueCapacity = queue;
        config.idleTimeoutSeconds = idleTimeout;
        config.maxUploadBytes = maxUpload;
        server_ = std::make_unique<Server>(config);
        thread_ = std::thread([this] { server_->run(); });
    }
    ~TestServer() { stop(); }

    void stop() {
        if (!thread_.joinable()) return;
        server_->stop();
        thread_.join();
    }
    std::uint16_t port() const { return server_->port(); }
    Client login(const std::string& user, const std::string& password) {
        Client client("127.0.0.1", port());
        client.login(user, password);
        return client;
    }

    check::TempDir dir;  // declared first, destroyed last

private:
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

template <typename F>
Status statusOf(F&& action) {
    try {
        action();
    } catch (const proto::StatusError& e) {
        return e.status();
    }
    return Status::Ok;
}

bool startsWith(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

void testAuthenticationAndRoles() {
    TestServer srv;
    {
        Client client("127.0.0.1", srv.port());
        CHECK(statusOf([&] { client.list(); }) == Status::Unauthorized);
        CHECK(statusOf([&] { client.login("walt", "wrong"); }) == Status::Unauthorized);
        CHECK(client.login("walt", "writer-pw") == "writer");
        CHECK(client.whoami() == "walt writer");
        CHECK(statusOf([&] { client.login("ada", "admin-pw"); }) == Status::BadRequest);
    }

    Client walt = srv.login("walt", "writer-pw");
    walt.put("notes.txt", "hello\n");
    walt.append("notes.txt", "more\n");
    CHECK(statusOf([&] { walt.remove("notes.txt"); }) == Status::Forbidden);

    Client rita = srv.login("rita", "reader-pw");
    CHECK(rita.get("notes.txt") == "hello\nmore\n");
    CHECK(statusOf([&] { rita.put("x.txt", "nope"); }) == Status::Forbidden);
    CHECK(statusOf([&] { rita.append("notes.txt", "nope"); }) == Status::Forbidden);
    CHECK(rita.list().size() == 1);  // the rejected upload left nothing behind

    Client ada = srv.login("ada", "admin-pw");
    ada.remove("notes.txt");
    CHECK(ada.list().empty());

    // After three wrong passwords the server hangs up.
    Client guesser("127.0.0.1", srv.port());
    CHECK(statusOf([&] { guesser.login("ada", "a"); }) == Status::Unauthorized);
    CHECK(statusOf([&] { guesser.login("ada", "b"); }) == Status::Unauthorized);
    CHECK(statusOf([&] { guesser.login("ada", "c"); }) == Status::Unauthorized);
    CHECK_THROWS(guesser.login("ada", "admin-pw"));
}

void testBinaryRoundTrip() {
    TestServer srv;
    Client walt = srv.login("walt", "writer-pw");
    std::string data;
    for (int i = 0; i < 300000; ++i) data.push_back(static_cast<char>((i * 31 + i / 256) & 0xff));  // every byte value
    walt.put("blob.bin", data);
    CHECK(walt.get("blob.bin") == data);
    CHECK(walt.stat("blob.bin").size == data.size());

    std::string local = srv.dir.path + "/local.bin";
    std::string back = srv.dir.path + "/back.bin";
    check::writeFile(local, data);
    CHECK(walt.upload(local, "uploaded.bin") == data.size());
    CHECK(walt.download("uploaded.bin", back) == data.size());
    CHECK(check::readFile(back) == data);
    CHECK(statusOf([&] { walt.download("missing.bin", back); }) == Status::NotFound);
    CHECK(check::readFile(back) == data);  // a failed download leaves the local file alone
}

void testFileOperations() {
    TestServer srv(32, 64, 30, /*maxUpload=*/1024);
    Client walt = srv.login("walt", "writer-pw");
    walt.put("a.txt", "alpha");
    walt.put("b.txt", "beta");
    walt.copy("a.txt", "c.txt");
    CHECK(walt.get("c.txt") == "alpha");
    CHECK(statusOf([&] { walt.copy("a.txt", "b.txt"); }) == Status::Conflict);
    CHECK(statusOf([&] { walt.rename("a.txt", "b.txt"); }) == Status::Conflict);
    CHECK(statusOf([&] { walt.rename("a.txt", "a.txt"); }) == Status::Conflict);  // same lock twice: no self-deadlock
    walt.rename("a.txt", "z.txt");

    std::vector<RemoteFile> files = walt.list();
    CHECK(files.size() == 3 && files[0].name == "b.txt" && files[2].name == "z.txt");
    CHECK(statusOf([&] { walt.get("a.txt"); }) == Status::NotFound);
    CHECK(statusOf([&] { walt.get("../users.db"); }) == Status::BadRequest);  // no escaping the root
    CHECK(statusOf([&] { walt.put(".hidden", "x"); }) == Status::BadRequest);
    // Too large: refused before any payload is sent, so the connection stays usable.
    CHECK(statusOf([&] { walt.put("big.bin", std::string(2048, 'x')); }) == Status::PayloadTooLarge);
    CHECK(walt.get("b.txt") == "beta");
}

void testRawProtocol() {
    TestServer srv;
    Connection conn(connectTcp("127.0.0.1", srv.port()));
    std::string line;
    conn.writeLine("HELP");
    CHECK(conn.readLine(line, 4096) && startsWith(line, "OK "));
    conn.writeLine("FROBNICATE x");
    CHECK(conn.readLine(line, 4096) && startsWith(line, "ERR 400"));
    conn.writeLine("GET notes.txt");
    CHECK(conn.readLine(line, 4096) && startsWith(line, "ERR 401"));

    // A header longer than the 4 KiB limit gets the connection dropped.
    conn.writeLine(std::string(10000, 'A'));
    bool dropped = false;
    try {
        dropped = !conn.readLine(line, 4096) || startsWith(line, "ERR 400");
    } catch (const NetError&) {
        dropped = true;
    }
    CHECK(dropped);
    Client after("127.0.0.1", srv.port());  // and the server is still fine
    CHECK(after.login("rita", "reader-pw") == "reader");
}

constexpr int kAppenders = 16;
constexpr int kRecordsEach = 100;
constexpr std::size_t kRecordSize = 20;

std::string record(int client, int seq) {
    char buf[kRecordSize + 1];
    std::snprintf(buf, sizeof buf, "client=%02d seq=%05d\n", client, seq);
    return std::string(buf, kRecordSize);
}

// Many clients append to one file at once. Every append must land whole
// (the write lock serializes them), and each client's records must appear
// in the order it sent them.
void testConcurrentAppendsAreAtomic() {
    TestServer srv;
    srv.login("walt", "writer-pw").put("shared.log", "");

    std::vector<std::thread> threads;
    for (int c = 0; c < kAppenders; ++c) {
        threads.emplace_back([&srv, c] {
            try {
                Client client = srv.login("walt", "writer-pw");
                for (int i = 0; i < kRecordsEach; ++i) client.append("shared.log", record(c, i));
                client.quit();
            } catch (const std::exception& e) {
                check::fail(__FILE__, __LINE__, e.what());
            }
        });
    }
    for (std::thread& t : threads) t.join();

    std::string log = srv.login("rita", "reader-pw").get("shared.log");
    CHECK(log.size() == kAppenders * kRecordsEach * kRecordSize);
    std::vector<int> next(kAppenders, 0);
    bool intact = true;
    for (std::size_t off = 0; intact && off + kRecordSize <= log.size(); off += kRecordSize) {
        int c = -1, seq = -1;
        intact = std::sscanf(log.c_str() + off, "client=%d seq=%d", &c, &seq) == 2 && c >= 0 && c < kAppenders &&
                 seq == next[c] && log[off + kRecordSize - 1] == '\n';
        if (intact) ++next[c];
    }
    CHECK(intact);
}

// One worker and room for one waiting connection: a third client is turned
// away with 503 right away instead of hanging.
void testBusyServerRejectsExtraClients() {
    TestServer srv(/*workers=*/1, /*queue=*/1);
    Client first = srv.login("walt", "writer-pw");             // occupies the only worker
    Connection queued(connectTcp("127.0.0.1", srv.port()));    // waits in the queue
    Connection rejected(connectTcp("127.0.0.1", srv.port()));  // finds the queue full
    std::string line;
    CHECK(rejected.readLine(line, 4096) && startsWith(line, "ERR 503"));

    first.quit();  // frees the worker, so the queued client is served next
    queued.writeLine("LOGIN rita reader-pw");
    CHECK(queued.readLine(line, 4096) && line == "OK reader");
}

void testIdleClientsAreDisconnected() {
    TestServer srv(4, 4, /*idleTimeout=*/1);
    Connection idle(connectTcp("127.0.0.1", srv.port()));
    std::string line;
    // Say nothing: after a second the server gives up and tells us why.
    CHECK(idle.readLine(line, 4096) && startsWith(line, "ERR 408"));
}

// stop() must not hang on a session that is blocked waiting for its client.
void testStopWakesIdleSessions() {
    TestServer srv;
    Client walt = srv.login("walt", "writer-pw");
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    srv.stop();
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    CHECK_THROWS(walt.whoami());
}

}  // namespace

int main() {
    check::watchdog(150);
    std::signal(SIGPIPE, SIG_IGN);
    Log::setLevel(Log::Level::Error);
    testAuthenticationAndRoles();
    testBinaryRoundTrip();
    testFileOperations();
    testRawProtocol();
    testConcurrentAppendsAreAtomic();
    testBusyServerRejectsExtraClients();
    testIdleClientsAreDisconnected();
    testStopWakesIdleSessions();
    return check::summary("test_end_to_end");
}

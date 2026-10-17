// mtfs-stress: load generator and consistency checker.
//
// N clients hammer a few shared files with a mix of GET, APPEND, PUT, COPY
// and RENAME for a fixed time, then the tool reports throughput and latency.
//
// Correctness checks:
//  * Every file is a sequence of fixed-size 32-byte records, and every GET
//    verifies that. A read that overlapped a half-finished append or a
//    half-replaced file would contain a broken record.
//  * COPY and RENAME always run between two existing hot files, so they lock
//    two names in opposite orders all the time (COPY a b racing COPY b a),
//    the classic deadlock recipe. They are refused with 409 because the
//    target exists, but only after both locks were taken. If the lock
//    ordering were wrong, the run would hang instead of finishing.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "client/client.hpp"

using namespace mtfs;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kRecordSize = 32;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = proto::kDefaultPort;
    std::string user;
    std::string password;
    int clients = 16;
    int seconds = 10;
    int files = 4;
};

enum Op { kGet, kAppend, kPut, kCopy, kRename, kOpCount };
const char* const kOpNames[kOpCount] = {"GET", "APPEND", "PUT", "COPY", "RENAME"};

struct Stats {
    std::uint64_t ops[kOpCount] = {};
    std::uint64_t verifiedReads = 0;
    std::uint64_t corruptReads = 0;
    std::uint64_t unexpectedErrors = 0;
    std::vector<float> latencyMicros;
    std::string firstError;

    void error(const std::string& what) {
        ++unexpectedErrors;
        if (firstError.empty()) firstError = what;
    }
};

std::string fileName(int k) { return "hot-" + std::to_string(k) + ".log"; }

// "c007 s000000042 ###############\n": client id, sequence number, padding.
std::string makeRecord(int client, std::uint32_t seq) {
    char buf[kRecordSize + 1];
    std::snprintf(buf, sizeof buf, "c%03u s%09u ###############\n", static_cast<unsigned>(client) % 1000u,
                  seq % 1000000000u);
    return std::string(buf, kRecordSize);
}

bool isRecord(const char* r) {
    auto digits = [r](int from, int count) {
        for (int i = from; i < from + count; ++i) {
            if (r[i] < '0' || r[i] > '9') return false;
        }
        return true;
    };
    if (r[0] != 'c' || !digits(1, 3) || r[4] != ' ' || r[5] != 's' || !digits(6, 9) || r[15] != ' ') return false;
    for (std::size_t i = 16; i < kRecordSize - 1; ++i) {
        if (r[i] != '#') return false;
    }
    return r[kRecordSize - 1] == '\n';
}

bool isWellFormed(const std::string& data) {
    if (data.size() % kRecordSize != 0) return false;
    for (std::size_t off = 0; off < data.size(); off += kRecordSize) {
        if (!isRecord(data.data() + off)) return false;
    }
    return true;
}

void runClient(const Options& opt, int id, Clock::time_point deadline, Stats& stats) {
    try {
        Client client(opt.host, opt.port);
        client.login(opt.user, opt.password);
        std::mt19937 rng(static_cast<unsigned>(id) * 7919u + 17u);
        std::uniform_int_distribution<int> pickFile(0, opt.files - 1), pickPercent(0, 99), pickCount(0, 3);
        std::uint32_t seq = 0;

        while (Clock::now() < deadline) {
            int roll = pickPercent(rng);
            Op op = roll < 45 ? kGet : roll < 70 ? kAppend : roll < 80 ? kPut : roll < 90 ? kCopy : kRename;
            std::string a = fileName(pickFile(rng));
            std::string b = fileName(pickFile(rng));
            Clock::time_point start = Clock::now();
            try {
                switch (op) {
                    case kGet:
                        if (isWellFormed(client.get(a))) {
                            ++stats.verifiedReads;
                        } else {
                            ++stats.corruptReads;
                        }
                        break;
                    case kAppend:
                        client.append(a, makeRecord(id, seq++));
                        break;
                    case kPut: {
                        std::string body;
                        for (int n = pickCount(rng); n > 0; --n) body += makeRecord(id, seq++);
                        client.put(a, body);
                        break;
                    }
                    case kCopy:
                        client.copy(a, b);
                        break;
                    case kRename:
                        client.rename(a, b);
                        break;
                    default:
                        break;
                }
            } catch (const proto::StatusError& e) {
                bool expected = (op == kCopy || op == kRename) && e.status() == proto::Status::Conflict;
                if (!expected) stats.error(std::string(kOpNames[op]) + " " + a + ": " + e.what());
            }
            stats.latencyMicros.push_back(std::chrono::duration<float, std::micro>(Clock::now() - start).count());
            ++stats.ops[op];
        }
        client.quit();
    } catch (const std::exception& e) {
        stats.error("client " + std::to_string(id) + ": " + e.what());
    }
}

void usage() {
    std::cerr << "usage: mtfs-stress --user NAME --password PW [--host H] [--port N]\n"
                 "                   [--clients N] [--seconds N] [--files N]\n"
                 "The user needs the writer role. Start the server with --workers >= --clients:\n"
                 "each session occupies one worker, and extra clients wait in the queue.\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--host") opt.host = value;
        else if (flag == "--port") opt.port = static_cast<std::uint16_t>(std::stoi(value));
        else if (flag == "--user") opt.user = value;
        else if (flag == "--password") opt.password = value;
        else if (flag == "--clients") opt.clients = std::clamp(std::stoi(value), 1, 999);
        else if (flag == "--seconds") opt.seconds = std::max(1, std::stoi(value));
        else if (flag == "--files") opt.files = std::max(1, std::stoi(value));
        else {
            usage();
            return 2;
        }
    }
    if (opt.user.empty() || argc % 2 == 0) {
        usage();
        return 2;
    }

    try {
        Client setup(opt.host, opt.port);
        setup.login(opt.user, opt.password);
        for (int k = 0; k < opt.files; ++k) setup.put(fileName(k), "");
        setup.quit();
    } catch (const std::exception& e) {
        std::cerr << "setup failed: " << e.what() << "\n";
        return 1;
    }

    std::printf("clients=%d files=%d duration=%ds\n", opt.clients, opt.files, opt.seconds);
    std::vector<Stats> stats(static_cast<std::size_t>(opt.clients));
    std::vector<std::thread> threads;
    Clock::time_point begin = Clock::now();
    Clock::time_point deadline = begin + std::chrono::seconds(opt.seconds);
    for (int i = 0; i < opt.clients; ++i)
        threads.emplace_back(runClient, std::cref(opt), i, deadline, std::ref(stats[static_cast<std::size_t>(i)]));
    for (std::thread& t : threads) t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();

    Stats total;
    int idleClients = 0;
    for (const Stats& s : stats) {
        std::uint64_t mine = 0;
        for (int op = 0; op < kOpCount; ++op) {
            total.ops[op] += s.ops[op];
            mine += s.ops[op];
        }
        if (mine == 0) ++idleClients;
        total.verifiedReads += s.verifiedReads;
        total.corruptReads += s.corruptReads;
        total.unexpectedErrors += s.unexpectedErrors;
        total.latencyMicros.insert(total.latencyMicros.end(), s.latencyMicros.begin(), s.latencyMicros.end());
        if (total.firstError.empty()) total.firstError = s.firstError;
    }

    std::uint64_t all = 0;
    for (int op = 0; op < kOpCount; ++op) all += total.ops[op];
    std::printf("operations: %llu in %.1fs = %.0f ops/s\n", static_cast<unsigned long long>(all), elapsed,
                static_cast<double>(all) / elapsed);
    for (int op = 0; op < kOpCount; ++op)
        std::printf("  %-7s %llu\n", kOpNames[op], static_cast<unsigned long long>(total.ops[op]));

    std::vector<float>& lat = total.latencyMicros;
    if (!lat.empty()) {
        auto percentile = [&lat](double p) {
            std::size_t k = static_cast<std::size_t>(p * static_cast<double>(lat.size() - 1));
            std::nth_element(lat.begin(), lat.begin() + static_cast<std::ptrdiff_t>(k), lat.end());
            return lat[k] / 1000.0f;
        };
        float p50 = percentile(0.50), p99 = percentile(0.99);
        float worst = *std::max_element(lat.begin(), lat.end()) / 1000.0f;
        std::printf("latency: p50 %.2f ms, p99 %.2f ms, max %.2f ms\n", p50, p99, worst);
    }
    std::printf("integrity: %llu reads verified, %llu corrupt\n",
                static_cast<unsigned long long>(total.verifiedReads),
                static_cast<unsigned long long>(total.corruptReads));
    std::printf("unexpected errors: %llu\n", static_cast<unsigned long long>(total.unexpectedErrors));
    if (!total.firstError.empty()) std::printf("  first: %s\n", total.firstError.c_str());
    if (idleClients > 0)
        std::printf("note: %d client(s) never got a worker; start the server with --workers >= %d\n", idleClients,
                    opt.clients);

    return total.corruptReads == 0 && total.unexpectedErrors == 0 ? 0 : 1;
}

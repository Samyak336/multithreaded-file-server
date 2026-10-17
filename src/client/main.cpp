// Interactive command-line client. With a terminal it shows a prompt;
// with piped input it runs the commands as a script and exits non-zero if
// any of them failed.

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "client/client.hpp"
#include "common/terminal.hpp"

using namespace mtfs;

namespace {

const char* kCommands =
    "  login <user> [password]   log in (asks for the password if omitted)\n"
    "  whoami                    show the current user and role\n"
    "  ls                        list files\n"
    "  stat <file>               size and modification time\n"
    "  cat <file>                print a file\n"
    "  get <file> [local]        download a file\n"
    "  put <local> [file]        upload a file (creates or replaces)\n"
    "  append <file> <text...>   append one line of text\n"
    "  cp <src> <dst>            copy a file on the server\n"
    "  mv <src> <dst>            rename a file on the server\n"
    "  rm <file>                 delete a file (admin only)\n"
    "  help                      show this list\n"
    "  quit                      disconnect\n";

std::string formatTime(std::int64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm local{};
    localtime_r(&t, &local);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

std::string baseName(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void requireArgs(const std::vector<std::string>& words, std::size_t min, std::size_t max, const char* usage) {
    std::size_t n = words.size() - 1;
    if (n < min || n > max) throw std::invalid_argument(std::string("usage: ") + usage);
}

// Runs one command. Returns false when the session should end.
bool runCommand(Client& client, const std::vector<std::string>& w, const std::string& line) {
    const std::string& cmd = w[0];
    if (cmd == "help") {
        std::cout << kCommands;
    } else if (cmd == "quit" || cmd == "exit") {
        client.quit();
        return false;
    } else if (cmd == "login") {
        requireArgs(w, 1, 1000, "login <user> [password]");
        std::string password = w.size() > 2 ? proto::textAfterWords(line, 2) : readPassword("password: ");
        std::cout << "logged in as " << w[1] << " (" << client.login(w[1], password) << ")\n";
    } else if (cmd == "whoami") {
        std::cout << client.whoami() << "\n";
    } else if (cmd == "ls") {
        std::vector<RemoteFile> files = client.list();
        for (const RemoteFile& f : files)
            std::cout << std::left << std::setw(32) << f.name << std::right << std::setw(12) << f.size << "  "
                      << formatTime(f.mtime) << "\n";
        std::cout << files.size() << " file(s)\n";
    } else if (cmd == "stat") {
        requireArgs(w, 1, 1, "stat <file>");
        RemoteFile f = client.stat(w[1]);
        std::cout << f.name << ": " << f.size << " bytes, modified " << formatTime(f.mtime) << "\n";
    } else if (cmd == "cat") {
        requireArgs(w, 1, 1, "cat <file>");
        std::cout << client.get(w[1]) << std::flush;
    } else if (cmd == "get") {
        requireArgs(w, 1, 2, "get <file> [local]");
        std::string local = w.size() > 2 ? w[2] : w[1];
        std::cout << "downloaded " << client.download(w[1], local) << " bytes to " << local << "\n";
    } else if (cmd == "put") {
        requireArgs(w, 1, 2, "put <local> [file]");
        std::string remote = w.size() > 2 ? w[2] : baseName(w[1]);
        std::cout << "uploaded " << client.upload(w[1], remote) << " bytes as " << remote << "\n";
    } else if (cmd == "append") {
        requireArgs(w, 2, 100000, "append <file> <text...>");
        client.append(w[1], proto::textAfterWords(line, 2) + "\n");
        std::cout << "appended to " << w[1] << "\n";
    } else if (cmd == "cp") {
        requireArgs(w, 2, 2, "cp <src> <dst>");
        client.copy(w[1], w[2]);
        std::cout << "copied " << w[1] << " to " << w[2] << "\n";
    } else if (cmd == "mv") {
        requireArgs(w, 2, 2, "mv <src> <dst>");
        client.rename(w[1], w[2]);
        std::cout << "renamed " << w[1] << " to " << w[2] << "\n";
    } else if (cmd == "rm") {
        requireArgs(w, 1, 1, "rm <file>");
        client.remove(w[1]);
        std::cout << "deleted " << w[1] << "\n";
    } else {
        throw std::invalid_argument("unknown command '" + cmd + "' (try help)");
    }
    return true;
}

void usage(const char* program) {
    std::cerr << "usage: " << program << " [--host HOST] [--port N] [--user NAME]\n\ncommands:\n" << kCommands;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = proto::kDefaultPort;
    std::string user;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        if ((flag == "--host" || flag == "--port" || flag == "--user") && i + 1 < argc) {
            std::string value = argv[++i];
            if (flag == "--host") host = value;
            else if (flag == "--user") user = value;
            else port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (flag == "--help" || flag == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    bool interactive = isatty(STDIN_FILENO) != 0;
    int failures = 0;
    try {
        Client client(host, port);
        if (!user.empty())
            std::cout << "logged in as " << user << " (" << client.login(user, readPassword("password: ")) << ")\n";

        std::string line;
        for (;;) {
            if (interactive) std::cout << "mtfs> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            std::vector<std::string> words = proto::splitWords(line);
            if (words.empty() || words[0][0] == '#') continue;
            try {
                if (!runCommand(client, words, line)) return failures == 0 ? 0 : 1;
            } catch (const proto::StatusError& e) {
                std::cerr << "error " << e.code() << ": " << e.what() << "\n";
                ++failures;
            } catch (const std::invalid_argument& e) {
                std::cerr << e.what() << "\n";
                ++failures;
            } catch (const std::system_error& e) {
                std::cerr << e.what() << "\n";
                ++failures;
            }
        }
        client.quit();  // end of input: say goodbye politely
    } catch (const NetError& e) {
        std::cerr << "connection error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
}

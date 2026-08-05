#include "execution/feed_process.hpp"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

extern char** environ;

namespace el {

FeedProcess::~FeedProcess() { stop(); }

bool FeedProcess::start(int feed_port) {
    if (pid_ > 0) return true;

    const char* exe = std::getenv("EL_FEEDHANDLER");
    if (exe == nullptr || *exe == '\0') {
        err_ = "not bundled";  // dev tree: the user runs the feedhandler manually
        return false;
    }
    if (::access(exe, X_OK) != 0) {
        err_ = std::string("feedhandler not executable: ") + exe;
        return false;
    }

    const char* venue_env = std::getenv("EL_VENUE");
    const std::string venue = (venue_env && *venue_env) ? venue_env : "coinbase";
    const std::string port = std::to_string(feed_port);

    // No --symbols: the feedhandler then boots its full top-crypto universe,
    // which is what the packaged app wants out of the box.
    std::vector<char*> argv{
        const_cast<char*>(exe),
        const_cast<char*>("--venue"), const_cast<char*>(venue.c_str()),
        const_cast<char*>("--feed-port"), const_cast<char*>(port.c_str()),
        // If we're force-quit or crash, stop() never runs -- this is what keeps
        // the child from outliving us and streaming forever.
        const_cast<char*>("--exit-when-orphaned"),
        nullptr,
    };

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe, nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
        err_ = "posix_spawn failed";
        return false;
    }

    pid_ = pid;
    err_.clear();
    return true;
}

void FeedProcess::stop() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGTERM);

    // Give it a moment to close its socket cleanly, then insist. Without the
    // reap the child would outlive the terminal as a zombie (or, worse, keep
    // streaming) after the window is closed.
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_ || r < 0) { pid_ = -1; return; }
        ::usleep(20'000);
    }
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
}

}  // namespace el

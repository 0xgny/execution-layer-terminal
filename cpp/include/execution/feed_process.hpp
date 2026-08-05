// ============================================================================
// feed_process.hpp -- Supervises the bundled feedhandler child process.
//
// In the .app bundle the Python feedhandler ships alongside the binary (see
// scripts/make_dmg.sh), and the terminal launches it so the user never has to
// start anything by hand. In a dev tree it doesn't exist and you run
// `scripts/run_stack.sh` yourself -- so this is a deliberate no-op unless the
// launcher points EL_FEEDHANDLER at an executable.
//
// Locating the binary via the environment rather than by deriving it from
// argv[0] keeps this free of platform executable-path APIs: the .app launcher
// already knows where it lives and just says so.
//
//   EL_FEEDHANDLER   absolute path to the feedhandler executable
//   EL_VENUE         venue to stream (default "coinbase")
// ============================================================================
#pragma once

#include <string>

#include <sys/types.h>

namespace el {

class FeedProcess {
public:
    ~FeedProcess();
    FeedProcess(const FeedProcess&) = delete;
    FeedProcess& operator=(const FeedProcess&) = delete;
    FeedProcess() = default;

    // Launch the bundled feedhandler pointed at `feed_port`. Returns false and
    // sets last_error() if EL_FEEDHANDLER is unset/not executable (the dev-tree
    // case, which is not an error worth surfacing) or the spawn fails.
    bool start(int feed_port);

    // SIGTERM the child and reap it. Safe to call when nothing was started.
    void stop();

    bool running() const { return pid_ > 0; }
    const std::string& last_error() const { return err_; }

private:
    pid_t pid_ = -1;
    std::string err_;
};

}  // namespace el

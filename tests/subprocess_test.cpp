#include "waylaunch/subprocess.h"
#include <cassert>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Reads /proc/<pid>/status SigBlk mask (hex); empty means no blocked signals.
std::string read_sigblk(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) return "";
    char line[256];
    std::string mask;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::string(line, 7) == "SigBlk:") {
            mask = line + 7;
            break;
        }
    }
    std::fclose(file);
    return mask;
}

int main() {
    using waylaunch::Subprocess;

    auto small = Subprocess::run({"sh", "-c", "cat"}, "waylaunch\n");
    assert(small.exit_code == 0);
    assert(small.stdout == "waylaunch\n");

    // Exercise the nonblocking stdin writer while the child produces more
    // output than a pipe can hold. The old single write-before-read path could
    // deadlock here or silently short-write.
    const std::string large_input(static_cast<std::size_t>(1024) * 1024, 'x');
    auto large = Subprocess::run({"sh", "-c", "(yes x | head -c 1048576) & cat >/dev/null; wait"},
                                 large_input);
    assert(large.exit_code == 0);
    assert(large.stdout.size() == static_cast<std::size_t>(1024) * 1024);

    // spawn_tracked basics: waitable pid, missing binary dies 127, empty is -1.
    {
        pid_t pid = Subprocess::spawn_tracked({"true"});
        assert(pid > 0);
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    {
        pid_t pid = Subprocess::spawn_tracked({"waylaunch-no-such-binary-xyz"});
        assert(pid > 0);
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 127);
    }
    assert(Subprocess::spawn_tracked({}) == -1);

    // The tracked child starts a new session with an empty signal mask, even
    // when spawned with signals blocked (the daemon case): caught live as
    // unkillable terminals and deaf daemons.
    {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTERM);
        sigaddset(&blocked, SIGUSR1);
        assert(sigprocmask(SIG_BLOCK, &blocked, nullptr) == 0);
        pid_t pid = Subprocess::spawn_tracked({"sleep", "30"});
        assert(pid > 0);
        // New session: session id equals the child's own pid.
        assert(getsid(pid) == pid);
        // Empty mask: no blocked signals despite the parent's mask.
        std::string mask = read_sigblk(pid);
        assert(!mask.empty());
        assert(std::stoul(mask, nullptr, 16) == 0);
        assert(sigprocmask(SIG_UNBLOCK, &blocked, nullptr) == 0);
        assert(kill(pid, SIGTERM) == 0);
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid);
    }
    return 0;
}

#include "waylaunch/subprocess.h"
#include <cassert>
#include <string>

int main() {
    using waylaunch::Subprocess;

    auto small = Subprocess::run({"sh", "-c", "cat"}, "waylaunch\n");
    assert(small.exit_code == 0);
    assert(small.stdout == "waylaunch\n");

    // Exercise the nonblocking stdin writer while the child produces more
    // output than a pipe can hold. The old single write-before-read path could
    // deadlock here or silently short-write.
    const std::string large_input(1024 * 1024, 'x');
    auto large = Subprocess::run(
        {"sh", "-c", "(yes x | head -c 1048576) & cat >/dev/null; wait"}, large_input);
    assert(large.exit_code == 0);
    assert(large.stdout.size() == 1024 * 1024);
    return 0;
}

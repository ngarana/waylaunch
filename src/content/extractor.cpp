#include "waylaunch/content/extractor.h"
#include "waylaunch/subprocess.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBMAGIC
#include <magic.h>
#endif

namespace waylaunch::content {

namespace {

// --------------------------------------------------------------------------
// Subprocess extractor: fork/exec with resource limits, wall-clock timeout, and
// a bounded stdout capture. This is the sandbox around untrusted format parsers.
// --------------------------------------------------------------------------
struct Capped {
    bool        ok = false;
    bool        timed_out = false;
    std::string out;
};

int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// --------------------------------------------------------------------------
// cgroup v2 sandbox for extractors (docs/CONTENT_SEARCH_STORE.md §4).
//
// RLIMIT_AS is the wrong memory knob: runtimes like GHC (pandoc) reserve huge
// PROT_NONE address ranges at startup and die under any useful cap. The right
// envelope is a dedicated child cgroup with memory.max — it bounds what the
// kernel actually charges (RSS+swap), not what the process maps.
//
// When our own cgroup is delegated (systemd unit with Delegate=, or any
// user-owned scope), we split it into ./main (the daemon, moved there to
// satisfy the no-internal-process rule) and ./extract (memory.max, swap off,
// pids.max, whole-group OOM kill). Children are placed in ./extract atomically
// at fork time via clone3(CLONE_INTO_CGROUP) — no post-fork races. Everything
// is best-effort: without delegation we fall back to rlimits only.
// --------------------------------------------------------------------------
bool write_cg_file(const std::string& path, const std::string& val) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = write(fd, val.data(), val.size());
    close(fd);
    return n == static_cast<ssize_t>(val.size());
}

class ExtractCgroup {
public:
    static ExtractCgroup& instance() {
        static ExtractCgroup g;
        return g;
    }

    int fd() const { return fd_; }   // O_DIRECTORY fd of ./extract, -1 if unavailable

    // Best-effort memory cap on the extract group (0 = uncapped).
    void set_memory_max(size_t bytes) {
        if (fd_ < 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (bytes == applied_) return;
        if (write_cg_file(extract_dir_ + "/memory.max",
                          bytes ? std::to_string(bytes) : "max"))
            applied_ = bytes;
    }

private:
    ExtractCgroup() {
        std::ifstream f("/proc/self/cgroup");
        std::string line, cgpath;
        while (std::getline(f, line))
            if (line.rfind("0::", 0) == 0) { cgpath = line.substr(3); break; }
        if (cgpath.empty() || cgpath == "/") return;
        std::string base = "/sys/fs/cgroup" + cgpath;
        if (access(base.c_str(), W_OK) != 0) return;   // not delegated to us

        std::string main_dir = base + "/main";
        extract_dir_ = base + "/extract";
        if ((mkdir(main_dir.c_str(), 0755) != 0 && errno != EEXIST) ||
            (mkdir(extract_dir_.c_str(), 0755) != 0 && errno != EEXIST))
            return;
        // Leaf-ify: controllers can only be distributed to children once the
        // parent has no member processes, so the daemon moves itself to ./main.
        if (!write_cg_file(main_dir + "/cgroup.procs", std::to_string(getpid())))
            return;
        // Controller delegation is best-effort (fails when unrelated processes
        // share the parent, e.g. a terminal scope) — CLONE_INTO_CGROUP
        // placement and group-kill still work without it.
        write_cg_file(base + "/cgroup.subtree_control", "+memory");
        write_cg_file(base + "/cgroup.subtree_control", "+pids");
        write_cg_file(extract_dir_ + "/memory.oom.group", "1");  // OOM kills whole tree
        write_cg_file(extract_dir_ + "/memory.swap.max", "0");   // OOM, don't thrash
        write_cg_file(extract_dir_ + "/pids.max", "32");         // no fork bombs
        fd_ = open(extract_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    std::string extract_dir_;
    int         fd_ = -1;
    size_t      applied_ = 0;
    std::mutex  mtx_;
};

#ifndef CLONE_INTO_CGROUP
#define CLONE_INTO_CGROUP 0x200000000ULL
#endif

// Fork the extractor directly into the sandbox cgroup (Linux ≥5.7); plain
// fork() when clone3/cgroup placement is unavailable.
pid_t spawn_child(int cgroup_fd) {
#ifdef SYS_clone3
    if (cgroup_fd >= 0) {
        struct {   // struct clone_args, CLONE_ARGS_SIZE_VER2 layout
            uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
            uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
        } args{};
        args.flags = CLONE_INTO_CGROUP;
        args.exit_signal = SIGCHLD;
        args.cgroup = static_cast<uint64_t>(cgroup_fd);
        long r = syscall(SYS_clone3, &args, sizeof(args));
        if (r >= 0) return static_cast<pid_t>(r);
    }
#endif
    return fork();
}

// Resolve a bare program name to an absolute path via $PATH (deterministic exec).
std::string resolve_in_path(const std::string& prog) {
    if (prog.find('/') != std::string::npos) return prog;
    const char* penv = getenv("PATH");
    std::string paths = penv ? penv : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= paths.size()) {
        size_t colon = paths.find(':', start);
        std::string dir = paths.substr(start, colon == std::string::npos ? std::string::npos
                                                                          : colon - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + prog;
            if (access(full.c_str(), X_OK) == 0) return full;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

Capped run_capped(const std::vector<std::string>& argv, const ExtractOptions& opt,
                  size_t max_out) {
    Capped r;
    if (argv.empty()) return r;
    std::string exe = resolve_in_path(argv[0]);
    if (exe.empty()) return r;
    int outpipe[2];
    if (pipe(outpipe) != 0) return r;

    ExtractCgroup& cg = ExtractCgroup::instance();
    cg.set_memory_max(opt.mem_limit_bytes);

    pid_t pid = spawn_child(cg.fd());
    if (pid < 0) {
        close(outpipe[0]);
        close(outpipe[1]);
        return r;
    }
    if (pid == 0) {
        // --- child ---
        setpgid(0, 0);                       // own group so we can kill any tree
        prctl(PR_SET_PDEATHSIG, SIGKILL);    // die with the daemon, never orphan
        if (getppid() == 1) _exit(127);      // parent already gone before prctl
        dup2(outpipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        close(outpipe[0]);
        close(outpipe[1]);

        rlimit rl;
        rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(opt.cpu_seconds);
        setrlimit(RLIMIT_CPU, &rl);
        rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(max_out * 4 + (1u << 20));
        setrlimit(RLIMIT_FSIZE, &rl);
        rl.rlim_cur = rl.rlim_max = 0;
        setrlimit(RLIMIT_CORE, &rl);
        if (opt.mem_limit_bytes > 0) {
            // RLIMIT_DATA, not RLIMIT_AS: since Linux 4.7 it covers brk plus
            // writable private mappings (what allocators actually commit) while
            // ignoring PROT_NONE reservations, so address-space-hungry runtimes
            // (GHC/pandoc, JVMs) start fine but real memory use stays bounded.
            // Defense in depth alongside the cgroup memory.max above.
            rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(opt.mem_limit_bytes);
            setrlimit(RLIMIT_DATA, &rl);
        }
        nice(opt.nice);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        char* env[] = {const_cast<char*>("PATH=/usr/local/bin:/usr/bin:/bin"),
                       const_cast<char*>("LC_ALL=C.UTF-8"),
                       const_cast<char*>("HOME=/nonexistent"), nullptr};
        execve(exe.c_str(), cargv.data(), env);
        _exit(127);
    }

    // --- parent ---
    close(outpipe[1]);
    fcntl(outpipe[0], F_SETFL, O_NONBLOCK);
    int64_t deadline = now_ms() + opt.timeout_ms;
    char buf[8192];
    bool killed = false;
    while (true) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) {
            r.timed_out = true;
            kill(-pid, SIGKILL);
            killed = true;
            break;
        }
        pollfd pfd{outpipe[0], POLLIN, 0};
        int pr = poll(&pfd, 1, static_cast<int>(std::min<int64_t>(remaining, 250)));
        if (pr < 0) break;
        if (pr == 0) continue;
        ssize_t n = read(outpipe[0], buf, sizeof(buf));
        if (n > 0) {
            if (r.out.size() < max_out) {
                size_t take = std::min(static_cast<size_t>(n), max_out - r.out.size());
                r.out.append(buf, take);
                if (r.out.size() >= max_out) {   // enough text; stop the parser
                    kill(-pid, SIGKILL);
                    killed = true;
                    break;
                }
            }
        } else if (n == 0) {
            break;                                // EOF
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
    }
    close(outpipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    r.ok = !r.timed_out && (killed || (WIFEXITED(status) && WEXITSTATUS(status) == 0));
    return r;
}

// --------------------------------------------------------------------------
// Helpers.
// --------------------------------------------------------------------------
std::string lower_ext(const std::string& path) {
    auto dot = path.find_last_of('.');
    auto slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return e;
}

// Strip NULs / lone control bytes and clamp to a byte cap. Keeps tabs/newlines.
void sanitize(std::string& s, size_t cap) {
    if (s.size() > cap) s.resize(cap);
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == 0) continue;
        if (c < 0x09 || (c > 0x0d && c < 0x20)) { out.push_back(' '); continue; }
        out.push_back(static_cast<char>(c));
    }
    s.swap(out);
}

std::string read_file_capped(const std::string& path, size_t cap) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string data;
    data.resize(cap);
    f.read(&data[0], static_cast<std::streamsize>(cap));
    data.resize(static_cast<size_t>(f.gcount()));
    return data;
}

// --- built-in importers ---------------------------------------------------
bool looks_like_text(const std::string& s) {
    if (s.empty()) return false;
    size_t bad = 0, n = std::min<size_t>(s.size(), 8192);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == 0) return false;                       // NUL → binary
        if (c < 0x09 || (c > 0x0d && c < 0x20)) bad++;
    }
    return bad * 100 < n * 5;                            // <5% control bytes
}

std::string strip_html(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0, n = in.size();
    auto skip_block = [&](const char* close) {
        size_t end = in.find(close, i);
        i = (end == std::string::npos) ? n : end + std::strlen(close);
    };
    while (i < n) {
        if (in[i] == '<') {
            // drop <script>…</script> and <style>…</style> wholesale
            if (in.compare(i, 7, "<script") == 0) { skip_block("</script>"); continue; }
            if (in.compare(i, 6, "<style") == 0)  { skip_block("</style>");  continue; }
            size_t end = in.find('>', i);
            if (end == std::string::npos) break;
            i = end + 1;
            out.push_back(' ');
        } else {
            out.push_back(in[i++]);
        }
    }
    // decode a handful of common entities
    struct { const char* e; char c; } ents[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
        {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '}};
    for (auto& e : ents) {
        size_t p = 0, len = std::strlen(e.e);
        while ((p = out.find(e.e, p)) != std::string::npos) out.replace(p, len, 1, e.c);
    }
    return out;
}

// --------------------------------------------------------------------------
// Extension → MIME fallback (used when libmagic is unavailable or unsure).
// --------------------------------------------------------------------------
std::string ext_mime(const std::string& ext) {
    static const std::vector<std::pair<const char*, const char*>> kMap = {
        {"txt", "text/plain"}, {"md", "text/markdown"}, {"markdown", "text/markdown"},
        {"rst", "text/plain"}, {"log", "text/plain"}, {"csv", "text/csv"},
        {"json", "application/json"}, {"xml", "text/xml"}, {"yaml", "text/yaml"},
        {"yml", "text/yaml"}, {"toml", "text/plain"}, {"ini", "text/plain"},
        {"conf", "text/plain"}, {"c", "text/x-c"}, {"h", "text/x-c"},
        {"cpp", "text/x-c++"}, {"cc", "text/x-c++"}, {"hpp", "text/x-c++"},
        {"py", "text/x-python"}, {"rs", "text/x-rust"}, {"go", "text/x-go"},
        {"js", "text/javascript"}, {"ts", "text/x-typescript"}, {"sh", "text/x-shellscript"},
        {"java", "text/x-java"}, {"rb", "text/x-ruby"}, {"lua", "text/x-lua"},
        {"html", "text/html"}, {"htm", "text/html"},
        {"pdf", "application/pdf"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"odt", "application/vnd.oasis.opendocument.text"},
        {"rtf", "application/rtf"}, {"epub", "application/epub+zip"},
    };
    for (auto& kv : kMap)
        if (ext == kv.first) return kv.second;
    return "";
}

} // namespace

// ==========================================================================
std::string detect_mime(const std::string& path) {
#ifdef HAVE_LIBMAGIC
    thread_local magic_t cookie = [] {
        magic_t c = magic_open(MAGIC_MIME_TYPE | MAGIC_SYMLINK | MAGIC_ERROR);
        if (c) magic_load(c, nullptr);
        return c;
    }();
    if (cookie) {
        const char* m = magic_file(cookie, path.c_str());
        if (m && *m) {
            std::string mime(m);
            auto semi = mime.find(';');
            if (semi != std::string::npos) mime.resize(semi);
            // libmagic reports many text formats as text/plain — prefer the
            // extension when it disambiguates (e.g. .md, .html, code files).
            if (mime == "text/plain" || mime == "application/octet-stream") {
                std::string em = ext_mime(lower_ext(path));
                if (!em.empty()) return em;
            }
            return mime;
        }
    }
#endif
    std::string em = ext_mime(lower_ext(path));
    return em.empty() ? "application/octet-stream" : em;
}

Extractor::Extractor(std::vector<std::string> enabled) : enabled_(std::move(enabled)) {}

std::string Extractor::importer_for(const std::string& mime, const std::string& path) const {
    std::string ext = lower_ext(path);
    auto on = [&](const char* name) {
        return std::find(enabled_.begin(), enabled_.end(), name) != enabled_.end();
    };
    // HTML before generic text (an .html is text/* but wants tag-stripping).
    if (on("html") && (mime == "text/html" || ext == "html" || ext == "htm")) return "html";
    if (on("pdf") && (mime == "application/pdf" || ext == "pdf")) return "pdf";
    if (on("office")) {
        static const char* office_ext[] = {"docx", "odt", "rtf", "epub", "odp", "ods"};
        for (auto* e : office_ext)
            if (ext == e) return "office";
        if (mime.find("opendocument") != std::string::npos ||
            mime.find("officedocument") != std::string::npos ||
            mime == "application/rtf" || mime == "application/epub+zip")
            return "office";
    }
    if (on("text") && (mime.rfind("text/", 0) == 0 || mime == "application/json" ||
                       mime == "application/xml" || mime == "application/javascript" ||
                       mime == "application/x-shellscript" || !ext_mime(ext).empty()))
        return "text";
    return "";
}

ExtractResult Extractor::extract(const std::string& path, const ExtractOptions& opt) const {
    ExtractResult res;
    res.mime = detect_mime(path);
    res.importer = importer_for(res.mime, path);
    if (res.importer.empty()) {
        res.status = ExtractStatus::Unsupported;
        return res;
    }

    if (res.importer == "text") {
        std::string data = read_file_capped(path, opt.max_read_bytes);
        if (!looks_like_text(data)) { res.status = ExtractStatus::Unsupported; return res; }
        res.text = std::move(data);
    } else if (res.importer == "html") {
        std::string raw = read_file_capped(path, opt.max_read_bytes);
        res.text = strip_html(raw);
    } else if (res.importer == "pdf") {
        if (!Subprocess::command_exists("pdftotext")) { res.status = ExtractStatus::Unsupported; return res; }
        Capped c = run_capped({"pdftotext", "-q", "-enc", "UTF-8", "--", path, "-"},
                              opt, opt.max_text_bytes);
        if (c.timed_out) { res.status = ExtractStatus::Timeout; return res; }
        if (!c.ok) { res.status = ExtractStatus::Error; return res; }
        res.text = std::move(c.out);
    } else if (res.importer == "office") {
        std::string ext = lower_ext(path);
        std::vector<std::string> argv;
        if (ext == "odt" && !Subprocess::command_exists("pandoc") &&
            Subprocess::command_exists("odt2txt")) {
            argv = {"odt2txt", "--encoding=UTF-8", path};
        } else if (Subprocess::command_exists("pandoc")) {
            argv = {"pandoc", "-t", "plain", "--wrap=none", "--", path};
        } else {
            res.status = ExtractStatus::Unsupported;
            return res;
        }
        Capped c = run_capped(argv, opt, opt.max_text_bytes);
        if (c.timed_out) { res.status = ExtractStatus::Timeout; return res; }
        if (!c.ok) { res.status = ExtractStatus::Error; return res; }
        res.text = std::move(c.out);
    }

    sanitize(res.text, opt.max_text_bytes);
    res.status = res.text.empty() ? ExtractStatus::Empty : ExtractStatus::Ok;
    return res;
}

} // namespace waylaunch::content

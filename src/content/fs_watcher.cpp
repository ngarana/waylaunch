#include "waylaunch/content/fs_watcher.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace waylaunch::content {

namespace {
constexpr uint32_t kMask = IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                           IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_EXCL_UNLINK;
}

FsWatcher::FsWatcher(std::vector<std::string> roots,
                     std::function<bool(const std::string&)> excluded)
    : roots_(std::move(roots)), excluded_(std::move(excluded)) {}

FsWatcher::~FsWatcher() { stop(); }

int FsWatcher::add_watch(const std::string& dir) {
    int wd = inotify_add_watch(fd_, dir.c_str(), kMask);
    if (wd < 0) {
        if (errno == ENOSPC) limit_hit_.store(true);   // max_user_watches exhausted
        return -1;
    }
    std::lock_guard<std::mutex> lk(maps_mtx_);
    // inotify returns the same wd for a path already watched — keep maps consistent.
    auto old = wd_to_path_.find(wd);
    if (old != wd_to_path_.end() && old->second != dir) path_to_wd_.erase(old->second);
    wd_to_path_[wd] = dir;
    path_to_wd_[dir] = wd;
    return wd;
}

void FsWatcher::add_watches(const std::string& dir) {
    std::vector<std::string> stack{dir};
    std::error_code ec;
    while (!stack.empty()) {
        std::string d = std::move(stack.back());
        stack.pop_back();
        if (excluded_(d)) continue;
        if (add_watch(d) < 0 && limit_hit_.load()) return;   // stop piling on watches
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it->is_directory(ec) && !it->is_symlink(ec)) stack.push_back(it->path().string());
        }
    }
}

void FsWatcher::scan_subtree(const std::string& dir) {
    std::vector<std::string> stack{dir};
    std::error_code ec;
    while (!stack.empty()) {
        std::string d = std::move(stack.back());
        stack.pop_back();
        if (excluded_(d)) continue;
        add_watch(d);
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            std::string p = it->path().string();
            if (it->is_symlink(ec)) continue;
            if (it->is_directory(ec)) stack.push_back(p);
            else if (it->is_regular_file(ec) && !excluded_(p)) cb_.on_index(p);
        }
    }
}

void FsWatcher::drop_watch_subtree(const std::string& dir) {
    std::lock_guard<std::mutex> lk(maps_mtx_);
    for (auto it = path_to_wd_.begin(); it != path_to_wd_.end();) {
        const std::string& p = it->first;
        bool under = p == dir ||
                     (p.size() > dir.size() && p.compare(0, dir.size(), dir) == 0 &&
                      p[dir.size()] == '/');
        if (under) {
            inotify_rm_watch(fd_, it->second);
            wd_to_path_.erase(it->second);
            it = path_to_wd_.erase(it);
        } else {
            ++it;
        }
    }
}

bool FsWatcher::start(Callbacks cb) {
    cb_ = std::move(cb);
    fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0) return false;
    stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd_ < 0) { close(fd_); fd_ = -1; return false; }
    for (const auto& r : roots_) add_watches(r);
    stop_.store(false);
    thread_ = std::thread([this] { thread_main(); });
    return true;
}

void FsWatcher::stop() {
    if (fd_ < 0) return;
    stop_.store(true);
    if (stop_fd_ >= 0) { uint64_t one = 1; ssize_t w = write(stop_fd_, &one, sizeof(one)); (void)w; }
    if (thread_.joinable()) thread_.join();
    if (stop_fd_ >= 0) { close(stop_fd_); stop_fd_ = -1; }
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

void FsWatcher::thread_main() {
    pollfd pfds[2] = {{fd_, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
    while (!stop_.load()) {
        int pr = poll(pfds, 2, -1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pfds[1].revents & POLLIN) break;    // stop signalled
        if (pfds[0].revents & POLLIN) drain_events();
    }
}

void FsWatcher::drain_events() {
    // inotify events are variable-length; align the buffer.
    alignas(struct inotify_event) char buf[64 * 1024];
    // rename pairing within this drain: cookie → (old path, is_dir)
    std::unordered_map<uint32_t, std::pair<std::string, bool>> pending;

    auto resolve = [&](int wd) -> std::string {
        std::lock_guard<std::mutex> lk(maps_mtx_);
        auto it = wd_to_path_.find(wd);
        return it == wd_to_path_.end() ? std::string() : it->second;
    };

    while (true) {
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        for (char* ptr = buf; ptr < buf + n;) {
            auto* ev = reinterpret_cast<struct inotify_event*>(ptr);
            ptr += sizeof(struct inotify_event) + ev->len;

            if (ev->mask & IN_Q_OVERFLOW) { if (cb_.on_overflow) cb_.on_overflow(); continue; }

            std::string base = resolve(ev->wd);
            bool isdir = ev->mask & IN_ISDIR;

            if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                if (!base.empty()) {
                    std::lock_guard<std::mutex> lk(maps_mtx_);
                    wd_to_path_.erase(ev->wd);
                    path_to_wd_.erase(base);
                }
                continue;
            }
            if (base.empty()) continue;
            std::string name = ev->len ? std::string(ev->name) : std::string();
            std::string full = name.empty() ? base : base + "/" + name;
            if (!name.empty() && excluded_(full) && !(ev->mask & (IN_DELETE | IN_MOVED_FROM)))
                continue;

            if (ev->mask & IN_CREATE) {
                if (isdir) scan_subtree(full);      // new dir: watch + enqueue its files
            } else if (ev->mask & IN_CLOSE_WRITE) {
                if (!isdir) cb_.on_index(full);
            } else if (ev->mask & IN_MOVED_FROM) {
                pending[ev->cookie] = {full, isdir};
            } else if (ev->mask & IN_MOVED_TO) {
                auto it = pending.find(ev->cookie);
                if (it != pending.end()) {          // a rename we can pair
                    const std::string& old = it->second.first;
                    if (it->second.second) drop_watch_subtree(old);
                    cb_.on_remove(old);
                    pending.erase(it);
                }
                if (isdir) scan_subtree(full);
                else cb_.on_index(full);
            } else if (ev->mask & IN_DELETE) {
                if (isdir) drop_watch_subtree(full);
                else cb_.on_remove(full);
            }
        }
    }

    // Unpaired MOVED_FROM = moved out of the tree → treat as deletion.
    for (auto& [cookie, info] : pending) {
        (void)cookie;
        if (info.second) drop_watch_subtree(info.first);
        cb_.on_remove(info.first);
    }
}

size_t FsWatcher::watch_count() const {
    std::lock_guard<std::mutex> lk(maps_mtx_);
    return path_to_wd_.size();
}

} // namespace waylaunch::content

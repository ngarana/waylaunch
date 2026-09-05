#include "waylaunch/providers/file_provider.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace waylaunch;

namespace fs = std::filesystem;

namespace {

std::string unique_dir(const char* tag) {
    auto p = fs::temp_directory_path() /
             (std::string("waylaunch-fp-test-") + tag + "-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p.string();
}

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << "x";
}

ProviderQuery mkq(const std::string& text) {
    std::string lower = text;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ProviderQuery{.text = text, .lower = lower, .max_results = 6};
}

std::vector<std::string> names_of(const std::vector<ListItem>& r) {
    std::vector<std::string> out;
    out.reserve(r.size());
    for (const auto& it : r) out.push_back(it.name);
    return out;
}

} // namespace

int main() {
    std::error_code ec;
    std::string root = unique_dir("root");
    auto cleanup = [&] { fs::remove_all(root, ec); };

    touch(fs::path(root) / "apple.txt");
    touch(fs::path(root) / "banana.txt");
    touch(fs::path(root) / "APRICOT.md");
    touch(fs::path(root) / "not_a_match.dat");
    fs::create_directories(fs::path(root) / "apple_docs");
    touch(fs::path(root) / "apple_docs" / "manifest.txt");

    fs::create_directories(fs::path(root) / "node_modules");
    touch(fs::path(root) / "node_modules" / "apple_hidden.txt");

    fs::create_directories(fs::path(root) / ".git");
    touch(fs::path(root) / ".git" / "apple_also_hidden.txt");

    touch(fs::path(root) / "notes.tmp");
    touch(fs::path(root) / "apple.tmp");

    {
        FileProvider prov({root}, {".git", "node_modules", "*.tmp"}, 2, 6, nullptr);
        assert(prov.is_available());

        auto r = prov.query(mkq("apple"));
        auto names = names_of(r);
        assert(names.size() == 2);
        assert(names[0] == "apple_docs");
        assert(r[0].kind == ItemKind::Folder);
        assert(names[1] == "apple.txt");
        assert(r[1].kind == ItemKind::File);

        assert(prov.query(mkq("x")).empty());

        auto dir = prov.query(mkq("apple_docs"));
        assert(dir.size() == 1);
        assert(dir[0].kind == ItemKind::Folder);
    }

    {
        FileProvider prov({root}, {".git", "node_modules"}, 2, 100, nullptr);
        auto r = prov.query(mkq("apple"));
        auto names = names_of(r);
        assert(names.size() == 3);
        assert(names[0] == "apple_docs");
        assert(std::ranges::find(names, "apple.txt") != names.end());
        assert(std::ranges::find(names, "apple.tmp") != names.end());

        auto apr = prov.query(mkq("APR"));
        assert(apr.size() == 1);
        assert(apr[0].name == "APRICOT.md");

        auto pref = prov.query(mkq("Ban"));
        assert(pref.size() == 1);
        assert(pref[0].name == "banana.txt");
    }

    {
        FileProvider prov({root}, {}, 2, 2, nullptr);
        auto all = prov.query(mkq("ap"));
        assert(all.size() == 2);
        assert(all[0].name == "apple_docs");
    }

    {
        FileProvider prov({"/no/such/path/"}, {}, 1, 6, nullptr);
        assert(prov.query(mkq("x")).empty());
    }

    {
        FileProvider nope({}, {}, 1, 6, nullptr);
        assert(nope.query(mkq("apple")).empty());
    }

    cleanup();
    return 0;
}

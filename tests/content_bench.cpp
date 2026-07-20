// content_bench — measures the two NFRs the store notes flagged as unrealistic:
// index-size ratio (NFR5) and worst-case query latency (NFR1). Builds a
// synthetic Zipfian corpus (so a few terms are ultra-common, like real text),
// then times selective vs ultra-common queries under the default planner and
// under a "full BM25, no budget" baseline — quantifying the bounded-planner win.
//
//   ./content_bench [num_docs] [words_per_doc]     (defaults 50000 120)
#include "waylaunch/content/store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace waylaunch::content;
using clk = std::chrono::steady_clock;

static double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

// Percentile from a sorted vector (nearest-rank).
static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(p / 100.0 * (v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

int main(int argc, char** argv) {
    int    ndocs = argc > 1 ? std::atoi(argv[1]) : 50000;
    int    wpd   = argc > 2 ? std::atoi(argv[2]) : 120;
    size_t vocab = 5000;

    fs::path dir = fs::temp_directory_path() / ("wl_bench_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string db = (dir / "index.db").string();

    // Distinct alphabetic words (length 4–8, no shared prefix, no digits) so the
    // tokenizer treats each as one term — a realistic corpus, unlike "wN" tokens
    // that would all share a "w" segment and distort both size and latency.
    std::mt19937 rng(1234);
    std::vector<std::string> words(vocab);
    {
        std::uniform_int_distribution<int> len(4, 8), ch(0, 25);
        for (size_t i = 0; i < vocab; i++) {
            std::string s(len(rng), 'a');
            for (auto& c : s) c = static_cast<char>('a' + ch(rng));
            words[i] = s;
        }
    }
    // Zipfian id draw: rank-1 word appears in ~every doc, tail words are rare.
    std::vector<double> cdf(vocab);
    double h = 0;
    for (size_t i = 0; i < vocab; i++) { h += 1.0 / (i + 1); cdf[i] = h; }
    for (auto& c : cdf) c /= h;
    std::uniform_real_distribution<double> uni(0, 1);
    auto pick = [&] {
        double r = uni(rng);
        return static_cast<size_t>(
            std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
    };

    StoreOptions wo{false, MatchMode::Prefix};
    Store w;
    if (!w.open(db, wo)) { std::fprintf(stderr, "open writer failed\n"); return 1; }

    std::printf("building %d docs x %d words (vocab %zu)...\n", ndocs, wpd, vocab);
    size_t raw_text_bytes = 0;
    auto t_build = clk::now();
    w.begin();
    for (int d = 0; d < ndocs; d++) {
        std::string body;
        body.reserve(wpd * 7);
        for (int k = 0; k < wpd; k++) {
            body += words[pick()];
            body += ' ';
        }
        raw_text_bytes += body.size();
        FileRecord r;
        r.path = (dir / ("f" + std::to_string(d) + ".txt")).string();
        r.name = "f" + std::to_string(d) + ".txt";
        r.parent = dir.string();
        r.size = static_cast<int64_t>(body.size());
        r.mtime_ns = d;
        r.state = FileState::Indexed;
        w.put(r, body);
        if ((d + 1) % 4096 == 0) { w.commit(); w.begin(); }
    }
    w.commit();
    w.maintain();
    double build_s = ms_since(t_build) / 1000.0;

    int64_t db_bytes = w.stats().db_bytes;
    std::printf("built in %.1fs — index %.1f MB, raw text %.1f MB, ratio %.0f%%\n",
                build_s, db_bytes / 1e6, raw_text_bytes / 1e6,
                100.0 * db_bytes / std::max<size_t>(raw_text_bytes, 1));
    w.close();

    // Rank-1 word is ultra-common (in ~every doc); a tail word is rare.
    std::string common = words[0];
    std::string rare = words[vocab - 1];

    auto bench = [&](const char* label, const StoreOptions& ro,
                     const std::string& q, int iters) {
        Store r;
        if (!r.open(db, ro)) { std::printf("  %-28s OPEN FAILED\n", label); return; }
        std::vector<double> lat;
        lat.reserve(iters);
        int hits = 0;
        for (int i = 0; i < iters; i++) {
            auto t = clk::now();
            auto res = r.search(q, 6, "[", "]");
            lat.push_back(ms_since(t));
            hits = static_cast<int>(res.size());
        }
        std::printf("  %-28s hits=%d  p50=%.2fms  p99=%.2fms  max=%.2fms\n",
                    label, hits, pct(lat, 50), pct(lat, 99),
                    *std::max_element(lat.begin(), lat.end()));
        r.close();
    };

    StoreOptions planner{true, MatchMode::Prefix};        // defaults: bounded + deadline
    StoreOptions baseline{true, MatchMode::Prefix};
    baseline.common_term_df = 1LL << 62;                  // never take bounded path
    baseline.query_budget_ms = 0;                         // no deadline → full BM25

    std::printf("\nquery latency (%d iters each):\n", 200);
    bench("selective (rare term)", planner, rare, 200);
    bench("common — full BM25 baseline", baseline, common, 50);
    bench("common — bounded planner", planner, common, 200);

    fs::remove_all(dir);
    return 0;
}

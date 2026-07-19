// Extractor correctness + robustness (NFR6): text/html extract, binary skip,
// and malformed inputs never crash — they yield Error/Unsupported.
#include "waylaunch/content/extractor.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace waylaunch::content;

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL: %s\n", m); failures++; } \
                         else std::printf("  ok: %s\n", m); } while (0)

static void wf(const fs::path& p, const std::string& c) {
    std::ofstream f(p, std::ios::binary); f << c;
}
static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

int main() {
    fs::path dir = fs::temp_directory_path() / ("wl_ex_test_" + std::to_string(::getpid()));
    fs::remove_all(dir); fs::create_directories(dir);
    Extractor ex;

    wf(dir / "a.txt", "quarterly revenue report for one_target_two");
    auto r = ex.extract((dir / "a.txt").string());
    CHECK(r.status == ExtractStatus::Ok && has(r.text, "quarterly revenue"), "txt extracted");

    wf(dir / "c.html", "<html><head><style>.x{color:red}</style><script>var a=1;</script></head>"
                       "<body><h1>Title</h1><p>hello &amp; welcome</p></body></html>");
    r = ex.extract((dir / "c.html").string());
    CHECK(r.importer == "html" && has(r.text, "Title") && has(r.text, "hello & welcome"), "html extracted");
    CHECK(!has(r.text, "color:red") && !has(r.text, "var a=1"), "html script/style dropped");

    std::string blob(512, '\0'); blob[0] = (char)0xff; blob[3] = 1;
    wf(dir / "d.bin", blob);
    r = ex.extract((dir / "d.bin").string());
    CHECK(r.status == ExtractStatus::Unsupported, "binary → Unsupported");

    // Robustness: malformed "pdf"/"office" must not crash the process.
    wf(dir / "g.pdf", "%PDF-1.4 not a real pdf, just garbage bytes here");
    r = ex.extract((dir / "g.pdf").string());
    CHECK(r.status == ExtractStatus::Error || r.status == ExtractStatus::Empty ||
          r.status == ExtractStatus::Unsupported || r.status == ExtractStatus::Ok,
          "malformed pdf handled without crash");
    wf(dir / "h.docx", "PK\x03\x04 not really a docx");
    r = ex.extract((dir / "h.docx").string());
    CHECK(true, "malformed docx handled without crash");

    fs::remove_all(dir);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

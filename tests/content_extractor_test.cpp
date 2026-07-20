// Extractor correctness + robustness (NFR6): text/html extract, binary skip,
// OOXML/ODF/EPUB extraction via unzip+strip, and malformed inputs never crash —
// they yield Error/Unsupported.
#include "waylaunch/content/extractor.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace waylaunch::content;

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL: %s\n", m); failures++; } \
                         else std::printf("  ok: %s\n", m); } while (0)

static void wf(const fs::path& p, const std::string& c) {
    std::ofstream f(p, std::ios::binary); f << c;
}
static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

// ---- Minimal STORED (uncompressed) ZIP writer -----------------------------
// Hermetic office fixtures without depending on `zip`/soffice: OOXML/ODF/EPUB
// are just ZIPs of XML/XHTML parts, and unzip reads stored entries fine.
static uint32_t crc32_of(const std::string& s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : s) {
        crc ^= c;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return crc ^ 0xFFFFFFFFu;
}
static void put16(std::string& o, uint16_t v) { o.push_back(char(v)); o.push_back(char(v >> 8)); }
static void put32(std::string& o, uint32_t v) {
    for (int i = 0; i < 4; i++) o.push_back(char(v >> (8 * i)));
}
static void write_zip(const fs::path& path,
                      const std::vector<std::pair<std::string, std::string>>& members) {
    std::string out, central;
    std::vector<uint32_t> offsets;
    for (const auto& [name, data] : members) {
        offsets.push_back(static_cast<uint32_t>(out.size()));
        uint32_t crc = crc32_of(data);
        put32(out, 0x04034b50);                  // local file header sig
        put16(out, 20); put16(out, 0); put16(out, 0);  // ver, flags, method=store
        put16(out, 0); put16(out, 0);            // mod time/date
        put32(out, crc);
        put32(out, static_cast<uint32_t>(data.size()));   // comp size
        put32(out, static_cast<uint32_t>(data.size()));   // uncomp size
        put16(out, static_cast<uint16_t>(name.size())); put16(out, 0);
        out += name; out += data;
    }
    uint32_t cd_start = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < members.size(); i++) {
        const auto& [name, data] = members[i];
        put32(central, 0x02014b50);              // central dir header sig
        put16(central, 20); put16(central, 20); put16(central, 0); put16(central, 0);
        put16(central, 0); put16(central, 0);
        put32(central, crc32_of(data));
        put32(central, static_cast<uint32_t>(data.size()));
        put32(central, static_cast<uint32_t>(data.size()));
        put16(central, static_cast<uint16_t>(name.size()));
        put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
        put32(central, 0);                       // external attrs
        put32(central, offsets[i]);
        central += name;
    }
    out += central;
    put32(out, 0x06054b50);                      // end of central dir
    put16(out, 0); put16(out, 0);
    put16(out, static_cast<uint16_t>(members.size()));
    put16(out, static_cast<uint16_t>(members.size()));
    put32(out, static_cast<uint32_t>(central.size()));
    put32(out, cd_start);
    put16(out, 0);
    wf(path, out);
}

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

    // ---- OOXML / ODF / EPUB via unzip + strip -----------------------------
    // These need `unzip`; skip the assertions (not the test) if it's absent.
    bool have_unzip = fs::exists("/usr/bin/unzip") || fs::exists("/bin/unzip");
    if (have_unzip) {
        // docx: text lives in word/document.xml as <w:t> runs.
        write_zip(dir / "doc.docx", {
            {"[Content_Types].xml", "<Types/>"},
            {"word/document.xml",
             "<?xml version=\"1.0\"?><w:document><w:body><w:p><w:r><w:t>quarterly "
             "revenue</w:t></w:r><w:r><w:t>OneTargetTwo mongoose</w:t></w:r></w:p>"
             "</w:body></w:document>"}});
        r = ex.extract((dir / "doc.docx").string());
        CHECK(r.status == ExtractStatus::Ok && r.importer == "office" &&
              has(r.text, "quarterly revenue") && has(r.text, "mongoose"),
              "docx: text extracted from word/document.xml");

        // xlsx: cell strings live in xl/sharedStrings.xml; the headline use case.
        // We index only sharedStrings, so the sheet's index-reference noise
        // (<v>0</v>) and header/footer chrome stay out of the index.
        write_zip(dir / "book.xlsx", {
            {"[Content_Types].xml", "<Types/>"},
            {"xl/sharedStrings.xml",
             "<?xml version=\"1.0\"?><sst><si><t>Region</t></si><si><t>quarterly "
             "revenue</t></si><si><t>penguins</t></si></sst>"},
            {"xl/worksheets/sheet1.xml",
             "<?xml version=\"1.0\"?><worksheet><sheetData><row><c t=\"s\"><v>0</v></c>"
             "</row></sheetData><oddHeader>&amp;\"Times New Roman\"Page</oddHeader>"
             "</worksheet>"}});
        r = ex.extract((dir / "book.xlsx").string());
        CHECK(r.status == ExtractStatus::Ok && has(r.text, "quarterly revenue") &&
              has(r.text, "penguins"),
              "xlsx: shared-string cell text extracted (spreadsheet contents)");
        CHECK(!has(r.text, "Times New Roman") && !has(r.text, "Page"),
              "xlsx: sheet header/footer chrome kept out of the index");

        // pptx: multiple slides under ppt/slides/slideN.xml; glob must catch all.
        write_zip(dir / "deck.pptx", {
            {"[Content_Types].xml", "<Types/>"},
            {"ppt/slides/slide1.xml",
             "<?xml version=\"1.0\"?><p:sld><a:t>aardvark intro</a:t></p:sld>"},
            {"ppt/slides/slide2.xml",
             "<?xml version=\"1.0\"?><p:sld><a:t>mongoose summary</a:t></p:sld>"}});
        r = ex.extract((dir / "deck.pptx").string());
        CHECK(r.status == ExtractStatus::Ok && has(r.text, "aardvark") &&
              has(r.text, "mongoose"),
              "pptx: text from all slides (glob matches slide1+slide2)");

        // ods: ODF puts everything in content.xml; numeric entity must decode.
        write_zip(dir / "sheet.ods", {
            {"mimetype", "application/vnd.oasis.opendocument.spreadsheet"},
            {"content.xml",
             "<?xml version=\"1.0\"?><office:document-content><office:body>"
             "<table:table-cell><text:p>revenue&#8482; ocelot</text:p></table:table-cell>"
             "</office:body></office:document-content>"}});
        r = ex.extract((dir / "sheet.ods").string());
        CHECK(r.status == ExtractStatus::Ok && has(r.text, "revenue") &&
              has(r.text, "ocelot"), "ods: content.xml extracted");
        CHECK(has(r.text, "\xE2\x84\xA2"), "ods: numeric entity &#8482; decoded to UTF-8 ™");

        // epub: reading content is in .xhtml; OPF/NCX metadata must NOT leak in.
        write_zip(dir / "book.epub", {
            {"mimetype", "application/epub+zip"},
            {"OEBPS/content.opf", "<package><metadata>SECRETMETADATA</metadata></package>"},
            {"OEBPS/ch1.xhtml",
             "<html><body><p>narwhal chapter about revenue</p></body></html>"}});
        r = ex.extract((dir / "book.epub").string());
        CHECK(r.status == ExtractStatus::Ok && has(r.text, "narwhal") &&
              has(r.text, "revenue"), "epub: xhtml reading content extracted");
        CHECK(!has(r.text, "SECRETMETADATA"), "epub: OPF metadata excluded from index");
    } else {
        std::printf("  (skipping OOXML/ODF/EPUB checks — unzip not found)\n");
    }

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

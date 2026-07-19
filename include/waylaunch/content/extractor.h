#pragma once

// Text extraction — the mdimporter analog. Given a file, pick a format-specific
// importer (by MIME/extension) and pull out plain text, capped. Risky format
// parsers (PDF, Office/ODF) run in short-lived subprocesses with a wall-clock
// timeout + resource limits (the mdworker sandbox), so a malformed file can
// never hang or crash the indexer (NFR6). Trivial text/HTML is read in-process.

#include <string>
#include <vector>
#include <cstddef>

namespace waylaunch::content {

struct ExtractOptions {
    size_t max_text_bytes = 2 * 1024 * 1024;   // cap extracted text per file (NFR5)
    size_t max_read_bytes = 32 * 1024 * 1024;  // don't read more than this raw
    int    timeout_ms     = 10000;             // wall-clock per subprocess extractor
    int    cpu_seconds    = 15;                // RLIMIT_CPU for subprocess extractors
    size_t mem_limit_bytes = 0;                // RLIMIT_AS (0 = unset; avoids breaking
                                               //   mmap-heavy tools like pandoc/GHC)
    int    nice           = 10;                // background priority for extractors
};

enum class ExtractStatus { Ok, Unsupported, Empty, Timeout, Error };

struct ExtractResult {
    ExtractStatus status = ExtractStatus::Unsupported;
    std::string   text;    // extracted, sanitized, capped
    std::string   mime;    // detected MIME type
    std::string   importer;// which importer handled it ("" if none)
};

// Detect a file's MIME type: libmagic content sniff when available, else an
// extension table. Thread-safe (per-thread magic handle).
std::string detect_mime(const std::string& path);

class Extractor {
public:
    // `enabled` selects/orders importers by name: "text","pdf","office","html".
    explicit Extractor(std::vector<std::string> enabled = {"text", "pdf", "office", "html"});

    // Extract text for one file. Reentrant / thread-safe. Returns Unsupported
    // for binaries/unknown types (indexed metadata-only).
    ExtractResult extract(const std::string& path, const ExtractOptions& opt = {}) const;

    // Name of the importer that would handle (mime, path), or "" if none/disabled.
    std::string importer_for(const std::string& mime, const std::string& path) const;

private:
    std::vector<std::string> enabled_;
};

} // namespace waylaunch::content

-- schema.sql — waylaunch content index, schema v2 (reference).
--
-- This mirrors what src/content/store.cpp applies at runtime. The store is the
-- source of truth (it also registers the `wl` tokenizer and the wl_unzstd() SQL
-- function, and manages user_version migrations); this file documents the shape.
--
-- v2 changes vs v1: body text moved out of `files` into a zstd-compressed `docs`
-- table exposed through the `docs_v` view (FTS5 external content), `prefix='3'`
-- (was '2 3'), fts5vocab for the query planner, and NO `tokenchars '_'`.

PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;

-- Schema/tokenizer/format version. A mismatch triggers a rebuild (writer) or a
-- degrade-to-filename-search (reader) — the index is derived data.
PRAGMA user_version = 2;

-- Small key/value metadata (match mode, tokenizer id).
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);

-- Canonical file metadata (the minimal kMDItem* analog). No body text here, so
-- metadata scans never fault in body pages.
CREATE TABLE IF NOT EXISTS files (
    id            INTEGER PRIMARY KEY,
    path          TEXT UNIQUE NOT NULL,       -- absolute
    name          TEXT NOT NULL,
    parent        TEXT NOT NULL,
    size          INTEGER NOT NULL,
    mtime         INTEGER NOT NULL,           -- nanoseconds
    mime          TEXT,
    content_hash  BLOB,                       -- skip re-extraction when unchanged
    indexed_at    INTEGER NOT NULL,
    state         INTEGER NOT NULL            -- 0=indexed 1=pending 2=tombstone 3=error
);
CREATE INDEX IF NOT EXISTS files_state  ON files(state);
CREATE INDEX IF NOT EXISTS files_parent ON files(parent);

-- Extracted text, zstd-compressed (~3x on prose). Only live rows have a doc;
-- tombstoning deletes the doc, which (via the triggers) removes the FTS
-- postings, so the query path needs no state filter or join to files.state.
CREATE TABLE IF NOT EXISTS docs (
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL,
    body_z  BLOB                              -- zstd frame; NULL for empty bodies
);

-- The external-content source FTS5 reads through. wl_unzstd() is an app-defined
-- SQL function registered by the store; decompression is deterministic, so
-- 'delete' commands reproduce exactly the text that was indexed. This view is
-- what lets snippet()/highlight() read original text without a second
-- uncompressed copy.
CREATE VIEW IF NOT EXISTS docs_v(id, name, body) AS
    SELECT id, name, wl_unzstd(body_z) FROM docs;

-- FTS5 inverted index over docs_v.
--   * NO tokenchars '_'. unicode61 already treats '_' and '-' as separators;
--     the custom `wl` tokenizer (unicode61 + CamelCase/digit segmentation) adds
--     only the case/digit boundaries unicode61 cannot infer.
--   * prefix='3' matches the 3-char content_min_query without paying to index
--     shorter prefixes.
--   * name is weighted above body at query time via bm25(files_fts, 10.0, 1.0).
CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5(
    name,
    body,
    content='docs_v',
    content_rowid='id',
    tokenize = "wl remove_diacritics 2",
    prefix   = '3'
);

-- Term -> document-frequency access for the two-phase query planner: a query is
-- treated as "common" (bounded scan) only when every term's df exceeds a
-- threshold. See docs/CONTENT_SEARCH.md §6.1.
CREATE VIRTUAL TABLE IF NOT EXISTS files_fts_v USING fts5vocab('files_fts', 'row');

-- Keep files_fts synchronized with docs. Triggers decompress via wl_unzstd() so
-- FTS5 sees original text for both tokenizing (insert) and delete postings.
CREATE TRIGGER IF NOT EXISTS docs_ai AFTER INSERT ON docs
BEGIN
    INSERT INTO files_fts(rowid, name, body)
    VALUES (new.id, new.name, wl_unzstd(new.body_z));
END;

CREATE TRIGGER IF NOT EXISTS docs_ad AFTER DELETE ON docs
BEGIN
    INSERT INTO files_fts(files_fts, rowid, name, body)
    VALUES ('delete', old.id, old.name, wl_unzstd(old.body_z));
END;

CREATE TRIGGER IF NOT EXISTS docs_au AFTER UPDATE ON docs
BEGIN
    INSERT INTO files_fts(files_fts, rowid, name, body)
    VALUES ('delete', old.id, old.name, wl_unzstd(old.body_z));

    INSERT INTO files_fts(rowid, name, body)
    VALUES (new.id, new.name, wl_unzstd(new.body_z));
END;

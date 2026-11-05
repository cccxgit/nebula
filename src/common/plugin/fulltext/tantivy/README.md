# Embedded Tantivy bridge

This module embeds Tantivy 0.25.0 in a C++ process through a small C ABI. It opens
an mmap-backed on-disk index and requires no network service or Elasticsearch.
The dependency versions are pinned by `rust/Cargo.lock`.

## Integration

Add this directory with `add_subdirectory(tantivy)` and link the final executable
or library against the CMake target `tantivy_adapter`. Its public dependencies
include the Rust archive and the required Linux system libraries. CMake runs Cargo
with one compile job and places the archive at
`<build>/tantivy-rust-target/release/libnebula_tantivy.a`. A stable Rust toolchain
is required at build time; the deployed service does not require Rust or Cargo.

Tantivy's default columnar-zstd feature is disabled and its document store uses
Rust LZ4. The small vendored `include-flate` dependency retains upstream Rust
sources and the Apache license, with its default features changed to DEFLATE only.
This prevents Jieba's dictionary embedding helper from pulling a second native
zstd implementation into a binary that already links Nebula's libzstd. See
`rust/vendor/include-flate/NEBULA-PATCH.md` for provenance and the precise change.

`TantivyIndex.h` is independent of Folly, Thrift, and generated Nebula headers.
One instance can serve all partitions of one logical full-text index. A document
key is an arbitrary binary string; use the complete Nebula storage key as its
identity. The first four bytes are also indexed as a partition/type discriminator.
`search(..., keyPrefix)` combines this discriminator with the text query in
Tantivy's inverted index, then verifies the entire key prefix before accepting a
hit. The caller must use the same full document key when updating or deleting it.

`open(path, fields, writable, writerMemoryBytes, analyzer)` creates or opens an
index and verifies its persisted schema. Analyzer values are:

* `standard` or empty: lowercase Latin words and individual Chinese characters.
* `keyword`: the complete original field value is one case-sensitive token.
* `jieba`: dictionary-based Chinese segmentation using Jieba 0.8.1, with Latin
  text lowercased. The shared dictionary is initialized only when used.

Unknown analyzer names and mismatching persisted schemas are explicit errors.
These analyzer names do not promise identical tokenization to Elasticsearch's
similarly named or plugin-provided analyzers.

`upsert`, `remove`, and `clear` buffer changes. `commit` publishes a durable
Tantivy commit and reloads this instance's reader. Searches see only published
commits. `reload` refreshes a read-only instance. `setWritable(false)` commits
pending changes and releases the single-writer lock; `setWritable(true)` acquires
it. Destruction discards uncommitted operations. These guarantees cover this
index only, not atomicity with RocksDB or Raft; the storage integration owns
synchronization, replay, failure handling, and replica lifecycle.

## Query behavior

`SearchMode::Query` accepts terms, phrases, Boolean operators, field-qualified
queries, phrase slop, boosts, and inclusive term ranges. A compatibility layer
preserves the Tantivy grammar and replaces special query leaves for `Da*`,
`*b*`, `qu?ck`, `/qu(i|a)ck/`, and `term~0`, `term~1`, or `term~2`. A bare `term~`
defaults to two edits. Fuzzy text must produce one analyzer token. Wildcards
normalize literal case for standard/jieba and preserve case for keyword.
Regexes match complete indexed terms and preserve the supplied regex case. They
support Tantivy's regex language, not Lucene intersection/complement/numeric
interval extensions; unsupported operators explicitly fail. Internal index fields
are not queryable. Tests include the database regression fixture's main patterns.

Ordinary term and phrase scores are local BM25. Regex/wildcard/fuzzy query nodes
use Tantivy's automaton query scoring, so numeric scores do not promise ES parity.
The
indexed partition predicate contributes zero score, so partition population does
not bias relevance. This does not normalize BM25 across independently populated
storage indexes. An empty field selects all configured text fields.
This is not a complete implementation of Elasticsearch query DSL or every Lucene operator.

`Contains`, `StartsWith`, and `EndsWith` retain exact case-sensitive original
string semantics independently of the analyzer. Unicode trigrams narrow the
candidate set; stored original strings are then verified before a result counts
toward the requested limit. Queries shorter than three Unicode characters,
including an empty string, use the partition-filtered all-documents candidate
set. This fallback preserves recall but can cost more for short patterns. Up to
64 trigrams are used for long patterns; exact verification preserves correctness.

`limit=0` returns every verified hit. A positive limit is applied after both
partition and original-string verification; candidates cannot hide valid hits.
Finite-limit collectors bound their per-segment result buffers. Unlimited
results necessarily consume memory proportional to the number of true matches.
Substring scores are 1.0; they are predicates rather than relevance queries.
All wrapper calls on one instance serialize through a mutex.

## Verification

Run `cargo test --locked -j1 --manifest-path rust/Cargo.toml` for real disk-backed
Rust tests. `tests/TantivyBridgeTest.cpp` exercises the actual C++/Rust boundary,
including binary keys, Chinese and English, partition filtering, score output,
persistence, updates, deletes, and read-only writer transitions. CMake registers
it when `ENABLE_TESTING=ON`.

Sources: [Tantivy](https://github.com/quickwit-oss/tantivy),
[Tantivy 0.25 API](https://docs.rs/tantivy/0.25.0/tantivy/),
[Jieba Rust](https://github.com/messense/jieba-rs).

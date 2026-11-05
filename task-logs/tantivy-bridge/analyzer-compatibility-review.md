# standard analyzer compatibility review — read-only

Reviewed on 2026-11-05. No Rust/C++ changes, dependency download, compilation, ES request, service operation, or million-row scan was performed.

## Current implementation and likely query differences

`MixedTokenizer` in `rust/src/lib.rs` emits each CJK codepoint and splits other text at every non-alphanumeric character. It lowercases emitted tokens and assigns consecutive positions. ES 7.17 standard uses Unicode word boundaries, lowercasing and no stopwords by default.

The following are rule-based predictions, not measured differences on this million-row index. The actual comparison must collect complete VID sets before timing conclusions.

| Input | Current relevant tokens | Standard/Lucene rule consequence | Affected query |
| --- | --- | --- | --- |
| `water's`, `water’s` | `water`, `s` | Internal apostrophe remains in one word; current path can add a match | `water`, and AND if `river` also occurs |
| `water_river` | `water`, `river` | Connector underscore remains inside word | Both `water` and `water AND river` can differ |
| `water.river`, `water:river` | `water`, `river` | Single internal dot/colon between letters remains inside word | Both word and AND can differ |
| `New York's`, `New_York`, `New.York` | Includes adjacent `new`, `york` | Possessive or joined word prevents the exact two-token phrase | `"New York"` |
| `New 🗽 York` | Adjacent `new`, `york` | Lucene emits an emoji token at an intervening position | `"New York"` |
| `water` followed by a combining mark | `water` | Standard retains the combining mark in the token | `water`; mark placement matters |
| `3.14`, `1,000` | Two numeric tokens | Standard keeps numeric connectors | Usually changes token counts/BM25, not these three ASCII query sets by itself |

Spaces, ordinary hyphens, slashes and alphabetic commas generally split in both implementations. A sentence-final apostrophe/dot is not the same as an internal connector; repair must not simply retain every punctuation character.

Sources: [ES 7.17 standard analyzer](https://www.elastic.co/guide/en/elasticsearch/reference/7.17/analysis-standard-analyzer.html), [Lucene 8.11.3 standard tokenizer tests](https://raw.githubusercontent.com/apache/lucene-solr/releases/lucene-solr/8.11.3/lucene/core/src/test/org/apache/lucene/analysis/standard/TestStandardAnalyzer.java), [Lucene tokenizer grammar](https://raw.githubusercontent.com/apache/lucene-solr/releases/lucene-solr/8.11.3/lucene/core/src/java/org/apache/lucene/analysis/standard/StandardTokenizerImpl.jflex).

## unicode-segmentation assessment

Effective Cargo home is `/home/sch/.cargo`. Neither `registry/src/*/unicode-segmentation-*` nor `registry/cache/*/unicode-segmentation-*` exists. It is also absent from this bridge's Cargo.lock. Therefore a dependency change cannot currently build wholly offline from the checked cache.

`unicode_word_indices()` returns UAX29 word spans with byte offsets and filters out spans without an alphabetic or numeric character. A candidate pinned version is `=1.12.0`: it has no normal dependency tree and its generated tables explicitly use Unicode **16.0.0**. The Lucene 8.11.3 grammar uses Unicode **9.0** properties with separately maintained emoji rules/data; a shared UAX29 label is not version equivalence.

Expected improvements: internal ASCII/curly apostrophes, underscore, single letter-internal dot/colon, decimal/thousands connectors, and combining marks. Han word boundaries ordinarily retain one ideograph per token with attached Extend/Format marks; do not pre-split those spans again with the current per-char CJK loop, which would discard the attached marks.

Known remaining differences:

- Pure emoji spans are discarded by `unicode_word_indices`, whereas Lucene emits emoji tokens. Exact phrase positions can still differ. `split_word_bound_indices` plus a deliberately specified emoji policy would be needed for closer parity; an ad hoc “all symbols are tokens” policy is also wrong.
- `unicode_word_indices` has no 255-token-length policy. Lucene defaults to 255 Java character units and splits long tokens. A matching guard must account for UTF-16 code units without cutting a surrogate pair, retain original byte offsets, and advance a position for each emitted chunk. Do not silently truncate or apply a 255-byte UTF-8 limit.
- The current workload stores at most 128 Unicode characters per field; long-token behavior is not the likely cause of differences for the ASCII water/New York/river terms. Full-text production inputs still need dedicated long-token tests.
- Rust `str::to_lowercase` can expand/contextually map non-ASCII text, while Lucene uses its CharacterUtils per-codepoint lowercase path. ASCII case matches; universal Unicode case equivalence is not promised.
- New characters, scripts, emoji sequences and rules introduced after Unicode 9, and script-specific Lucene tailoring, remain outside an exact equivalence claim.

Sources: [unicode_word_indices API](https://docs.rs/unicode-segmentation/1.12.0/unicode_segmentation/trait.UnicodeSegmentation.html#tymethod.unicode_word_indices), [1.12.0 table version](https://raw.githubusercontent.com/unicode-rs/unicode-segmentation/v1.12.0/src/tables.rs), [1.12.0 Cargo manifest](https://raw.githubusercontent.com/unicode-rs/unicode-segmentation/v1.12.0/Cargo.toml), [Lucene token length implementation](https://raw.githubusercontent.com/apache/lucene-solr/releases/lucene-solr/8.11.3/lucene/core/src/java/org/apache/lucene/analysis/standard/StandardTokenizer.java), [Lucene lowercase filter](https://raw.githubusercontent.com/apache/lucene-solr/releases/lucene-solr/8.11.3/lucene/core/src/java/org/apache/lucene/analysis/LowerCaseFilter.java).

## Recommended decision

First keep the current binaries fixed and wait for actual ES/Tantivy complete VID sets. Publish missing/extra IDs and text examples for non-equivalent cases; those queries cannot support an ES speedup ratio. A same-engine transparent scan/pushdown comparison remains valid when its complete result sets match.

If measured differences are explained by common word connectors, the smallest defensible product patch is a pinned UAX29 span tokenizer using `unicode_word_indices` and the existing original-byte offsets/lowercase/positions, with disk-backed regressions for apostrophes, underscore, dot/colon, decimal, Han and combining marks. It improves the declared standard dialect; it does not make it identical to ES. Treat emoji phrase positions and long tokens as explicit remaining cases unless implemented and tested at the same time.

Any tokenizer change must rebuild all affected indexes before querying or timing. A schema/analyzer implementation version should change or an explicit derived-index rebuild must be required; keeping an old directory and silently registering a new implementation under the same tokenizer name can mix incompatible terms. Keep keyword, jieba and raw substring/trigram behavior unchanged. Rerun 9 bridge tests, 80 functional checks and full benchmark VID equivalence before accepting a new performance series; do not combine measurements from old and new analyzers.

If the corpus still differs after a patch, report only explicitly result-equivalent workloads as comparable, with all excluded cases disclosed. Alternatively run a separately labeled benchmark on a common normalized-token projection, recognizing that this is no longer the original 128-character text workload. Do not pick a query solely because the mismatching implementation happens to look faster.

//! In-process Tantivy bridge. C callers must serialize operations on each handle.
//! No background service, network client, or Elasticsearch dependency is involved.
use std::cell::RefCell;
use std::collections::HashSet;
use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::sync::OnceLock;
use tantivy::collector::{Collector, SegmentCollector};
use tantivy::query::{AllQuery, BooleanQuery, ConstScoreQuery, Occur, Query, TermQuery};
use tantivy::schema::{
    Field, IndexRecordOption, Schema, TextFieldIndexing, TextOptions, Value, INDEXED, STORED,
    STRING,
};
use tantivy::store::StoreReader;
use tantivy::tokenizer::{Token, TokenStream, Tokenizer};
use tantivy::{
    DocId, Index, IndexReader, IndexWriter, ReloadPolicy, Score, SegmentReader, TantivyDocument,
    Term,
};

type Result<T> = std::result::Result<T, String>;
mod query_compat;
fn message(e: impl std::fmt::Display) -> String {
    e.to_string()
}

// Chinese characters become separate tokens, Latin words remain words. The exact
// substring index below is independent of linguistic tokenization and normalization.
#[derive(Clone)]
struct MixedTokenizer;
struct MixedStream {
    tokens: std::vec::IntoIter<Token>,
    current: Token,
}
#[derive(Clone)]
struct JiebaTokenizer;
impl Tokenizer for JiebaTokenizer {
    type TokenStream<'a> = MixedStream;
    fn token_stream<'a>(&'a mut self, text: &'a str) -> MixedStream {
        static JIEBA: OnceLock<jieba_rs::Jieba> = OnceLock::new();
        let jieba = JIEBA.get_or_init(jieba_rs::Jieba::new);
        let mut offsets: Vec<usize> = text.char_indices().map(|(offset, _)| offset).collect();
        offsets.push(text.len());
        let tokens: Vec<Token> = jieba
            .tokenize(text, jieba_rs::TokenizeMode::Default, true)
            .into_iter()
            .filter(|token| token.word.chars().any(char::is_alphanumeric))
            .enumerate()
            .map(|(position, token)| Token {
                offset_from: offsets[token.start],
                offset_to: offsets[token.end],
                position,
                text: token.word.to_lowercase(),
                position_length: 1,
            })
            .collect();
        MixedStream {
            tokens: tokens.into_iter(),
            current: Token::default(),
        }
    }
}
fn cjk(c: char) -> bool {
    matches!(c as u32, 0x3400..=0x4dbf | 0x4e00..=0x9fff | 0xf900..=0xfaff | 0x20000..=0x323af)
}
impl Tokenizer for MixedTokenizer {
    type TokenStream<'a> = MixedStream;
    fn token_stream<'a>(&'a mut self, text: &'a str) -> MixedStream {
        let mut tokens = Vec::new();
        let mut word_start = None;
        let push = |from: usize, to: usize, tokens: &mut Vec<Token>| {
            tokens.push(Token {
                offset_from: from,
                offset_to: to,
                position: tokens.len(),
                text: text[from..to].to_lowercase(),
                position_length: 1,
            });
        };
        for (offset, ch) in text.char_indices() {
            if cjk(ch) || !ch.is_alphanumeric() {
                if let Some(start) = word_start.take() {
                    push(start, offset, &mut tokens);
                }
                if cjk(ch) {
                    push(offset, offset + ch.len_utf8(), &mut tokens);
                }
            } else if word_start.is_none() {
                word_start = Some(offset);
            }
        }
        if let Some(start) = word_start {
            push(start, text.len(), &mut tokens);
        }
        MixedStream {
            tokens: tokens.into_iter(),
            current: Token::default(),
        }
    }
}
impl TokenStream for MixedStream {
    fn advance(&mut self) -> bool {
        match self.tokens.next() {
            Some(token) => {
                self.current = token;
                true
            }
            None => false,
        }
    }
    fn token(&self) -> &Token {
        &self.current
    }
    fn token_mut(&mut self) -> &mut Token {
        &mut self.current
    }
}

fn trigrams(text: &str) -> HashSet<&str> {
    let mut offsets: Vec<usize> = text.char_indices().map(|(offset, _)| offset).collect();
    offsets.push(text.len());
    offsets
        .windows(4)
        .map(|window| &text[window[0]..window[3]])
        .collect()
}

struct Engine {
    index: Index,
    reader: IndexReader,
    writer: Option<IndexWriter>,
    key_field: Field,
    prefix_field: Field,
    fields: Vec<(String, Field, Field)>,
    memory_bytes: usize,
}
impl Engine {
    fn open(
        path: &Path,
        names: &[String],
        writable: bool,
        memory_bytes: usize,
        analyzer: &str,
    ) -> Result<Self> {
        if names.is_empty() {
            return Err("at least one text field is required".into());
        }
        let mut unique = HashSet::new();
        let mut builder = Schema::builder();
        let key_field = builder.add_bytes_field("__nebula_key", INDEXED | STORED);
        let prefix_field = builder.add_bytes_field("__nebula_prefix4", INDEXED);
        let tokenizer = match analyzer {
            "" | "standard" => "nebula_mixed",
            "keyword" => "raw",
            "jieba" => "nebula_jieba",
            _ => {
                return Err(format!(
                "unsupported Tantivy analyzer: {analyzer}; expected standard, keyword, or jieba"
            ))
            }
        };
        let text_options = TextOptions::default().set_stored().set_indexing_options(
            TextFieldIndexing::default()
                .set_tokenizer(tokenizer)
                .set_index_option(IndexRecordOption::WithFreqsAndPositions),
        );
        let mut fields = Vec::new();
        for (i, name) in names.iter().enumerate() {
            if name.is_empty() || name.starts_with("__nebula_") || !unique.insert(name) {
                return Err(format!("invalid or duplicate text field: {name}"));
            }
            let raw = builder.add_text_field(name, text_options.clone());
            let gram = builder.add_text_field(&format!("__nebula_gram_{i}"), STRING);
            fields.push((name.clone(), raw, gram));
        }
        let schema = builder.build();
        let index = if path.join("meta.json").exists() {
            let existing = Index::open_in_dir(path).map_err(message)?;
            if existing.schema() != schema {
                return Err("Tantivy schema mismatch; rebuild the index".into());
            }
            existing
        } else {
            if !writable {
                return Err("cannot open missing Tantivy index read-only".into());
            }
            std::fs::create_dir_all(path).map_err(message)?;
            Index::create_in_dir(path, schema).map_err(message)?
        };
        index.tokenizers().register("nebula_mixed", MixedTokenizer);
        index.tokenizers().register("nebula_jieba", JiebaTokenizer);
        let reader = index
            .reader_builder()
            .reload_policy(ReloadPolicy::Manual)
            .try_into()
            .map_err(message)?;
        let mut engine = Self {
            index,
            reader,
            writer: None,
            key_field,
            prefix_field,
            fields,
            memory_bytes: memory_bytes.max(15_000_000),
        };
        engine.set_writable(writable)?;
        Ok(engine)
    }
    fn set_writable(&mut self, writable: bool) -> Result<()> {
        if writable && self.writer.is_none() {
            self.writer = Some(
                self.index
                    .writer_with_num_threads(1, self.memory_bytes)
                    .map_err(message)?,
            );
        } else if !writable && self.writer.is_some() {
            self.commit()?;
            self.writer.take();
        }
        Ok(())
    }
    fn writer(&mut self) -> Result<&mut IndexWriter> {
        self.writer
            .as_mut()
            .ok_or_else(|| "Tantivy index is read-only".into())
    }
    fn upsert(&mut self, key: &[u8], values: &[String]) -> Result<()> {
        if values.len() != self.fields.len() {
            return Err("text field count does not match schema".into());
        }
        let mut doc = TantivyDocument::default();
        doc.add_bytes(self.key_field, key);
        if key.len() >= 4 {
            doc.add_bytes(self.prefix_field, &key[..4]);
        }
        for ((_, field, gram), value) in self.fields.iter().zip(values) {
            doc.add_text(*field, value);
            for token in trigrams(value) {
                doc.add_text(*gram, token);
            }
        }
        let key_term = Term::from_field_bytes(self.key_field, key);
        let writer = self.writer()?;
        writer.delete_term(key_term);
        writer.add_document(doc).map_err(message)?;
        Ok(())
    }
    fn remove(&mut self, key: &[u8]) -> Result<()> {
        let term = Term::from_field_bytes(self.key_field, key);
        self.writer()?.delete_term(term);
        Ok(())
    }
    fn clear(&mut self) -> Result<()> {
        self.writer()?.delete_all_documents().map_err(message)?;
        Ok(())
    }
    fn commit(&mut self) -> Result<()> {
        self.writer()?.commit().map_err(message)?;
        self.reader.reload().map_err(message)
    }
    fn search(
        &self,
        field: &str,
        pattern: &str,
        mode: i32,
        limit: usize,
        prefix: &[u8],
    ) -> Result<Vec<Hit>> {
        if !(0..=3).contains(&mode) {
            return Err("unknown Tantivy search mode".into());
        }
        let selected = self.fields.iter().find(|(name, _, _)| name == field);
        if selected.is_none() && !(mode == 0 && field.is_empty()) {
            return Err(format!("unknown Tantivy text field: {field}"));
        }
        let query: Box<dyn Query> = if mode == 0 {
            let defaults = selected
                .map(|(_, f, _)| vec![*f])
                .unwrap_or_else(|| self.fields.iter().map(|(_, f, _)| *f).collect());
            query_compat::parse(&self.index, defaults, &self.fields, pattern)?
        } else {
            let gram = selected.unwrap().2;
            let mut terms: Vec<&str> = trigrams(pattern).into_iter().collect();
            // A bounded subset remains a sound candidate filter for long patterns.
            terms.sort_unstable();
            terms.truncate(64);
            if terms.is_empty() {
                Box::new(AllQuery)
            } else {
                Box::new(BooleanQuery::new(
                    terms
                        .into_iter()
                        .map(|text| {
                            (
                                Occur::Must,
                                Box::new(TermQuery::new(
                                    Term::from_field_text(gram, text),
                                    IndexRecordOption::Basic,
                                )) as Box<dyn Query>,
                            )
                        })
                        .collect(),
                ))
            }
        };
        let query: Box<dyn Query> = if prefix.len() >= 4 {
            Box::new(BooleanQuery::new(vec![
                (Occur::Must, query),
                (
                    Occur::Must,
                    // Partition membership is a filter, not a relevance signal.
                    // Keep its inverted-index pruning without adding prefix IDF.
                    Box::new(ConstScoreQuery::new(
                        Box::new(TermQuery::new(
                            Term::from_field_bytes(self.prefix_field, &prefix[..4]),
                            IndexRecordOption::Basic,
                        )),
                        0.0,
                    )),
                ),
            ]))
        } else {
            query
        };
        self.reader
            .searcher()
            .search(
                &query,
                &VerifiedCollector {
                    key_field: self.key_field,
                    text_field: selected.map(|(_, f, _)| *f),
                    mode,
                    pattern: pattern.into(),
                    prefix: prefix.into(),
                    limit,
                },
            )
            .map_err(message)
    }
}

#[derive(Debug)]
struct Hit {
    key: Vec<u8>,
    score: f32,
}
fn rank_hits(hits: &mut Vec<Hit>, limit: usize) {
    hits.sort_unstable_by(|a, b| b.score.total_cmp(&a.score).then(a.key.cmp(&b.key)));
    if limit > 0 {
        hits.truncate(limit);
    }
}
#[derive(Clone)]
struct VerifiedCollector {
    key_field: Field,
    text_field: Option<Field>,
    mode: i32,
    pattern: String,
    prefix: Vec<u8>,
    limit: usize,
}
struct VerifiedSegment {
    config: VerifiedCollector,
    store: StoreReader,
    hits: Vec<Hit>,
    error: Option<tantivy::TantivyError>,
}
impl Collector for VerifiedCollector {
    type Fruit = Vec<Hit>;
    type Child = VerifiedSegment;
    fn for_segment(&self, _ordinal: u32, reader: &SegmentReader) -> tantivy::Result<Self::Child> {
        Ok(VerifiedSegment {
            config: self.clone(),
            store: reader.get_store_reader(8)?,
            hits: Vec::new(),
            error: None,
        })
    }
    fn requires_scoring(&self) -> bool {
        self.mode == 0
    }
    fn merge_fruits(&self, segments: Vec<tantivy::Result<Vec<Hit>>>) -> tantivy::Result<Vec<Hit>> {
        let mut hits = Vec::new();
        for result in segments {
            hits.extend(result?);
        }
        rank_hits(&mut hits, self.limit);
        Ok(hits)
    }
}
impl SegmentCollector for VerifiedSegment {
    type Fruit = tantivy::Result<Vec<Hit>>;
    fn collect(&mut self, doc_id: DocId, score: Score) {
        if self.error.is_some() {
            return;
        }
        let doc = match self.store.get::<TantivyDocument>(doc_id) {
            Ok(doc) => doc,
            Err(error) => {
                self.error = Some(error);
                return;
            }
        };
        let Some(key) = doc
            .get_first(self.config.key_field)
            .and_then(|v| v.as_bytes())
        else {
            return;
        };
        if !key.starts_with(&self.config.prefix) {
            return;
        }
        if self.config.mode != 0 {
            let Some(value) = doc
                .get_first(self.config.text_field.unwrap())
                .and_then(|v| v.as_str())
            else {
                return;
            };
            let pattern = self.config.pattern.as_str();
            let matches = match self.config.mode {
                1 => value.contains(pattern),
                2 => value.starts_with(pattern),
                3 => value.ends_with(pattern),
                _ => false,
            };
            if !matches {
                return;
            }
        }
        self.hits.push(Hit {
            key: key.to_vec(),
            score: if self.config.mode == 0 { score } else { 1.0 },
        });
        if self.config.limit > 0 && self.hits.len() > self.config.limit.saturating_mul(2) {
            rank_hits(&mut self.hits, self.config.limit);
        }
    }
    fn harvest(mut self) -> Self::Fruit {
        if let Some(error) = self.error {
            return Err(error);
        }
        rank_hits(&mut self.hits, self.config.limit);
        Ok(self.hits)
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ByteSlice {
    data: *const u8,
    len: usize,
}
impl ByteSlice {
    unsafe fn bytes<'a>(self) -> Result<&'a [u8]> {
        if self.len == 0 {
            return Ok(&[]);
        }
        if self.data.is_null() {
            return Err("null data pointer".into());
        }
        Ok(std::slice::from_raw_parts(self.data, self.len))
    }
    unsafe fn text<'a>(self) -> Result<&'a str> {
        std::str::from_utf8(self.bytes()?).map_err(message)
    }
}
thread_local! { static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap()); }
fn set_error(text: &str) {
    LAST_ERROR.with(|error| *error.borrow_mut() = CString::new(text.replace('\0', " ")).unwrap());
}
fn checked<T>(f: impl FnOnce() -> Result<T>) -> Result<T> {
    set_error("");
    let result = catch_unwind(AssertUnwindSafe(f))
        .unwrap_or_else(|_| Err("Rust panic contained at Tantivy C ABI boundary".into()));
    if let Err(error) = &result {
        set_error(error);
    }
    result
}
unsafe fn engine<'a>(handle: *mut c_void) -> Result<&'a mut Engine> {
    (handle as *mut Engine)
        .as_mut()
        .ok_or_else(|| "Tantivy index is not open".into())
}
unsafe fn strings(values: *const ByteSlice, count: usize) -> Result<Vec<String>> {
    if count == 0 {
        return Ok(Vec::new());
    }
    if values.is_null() {
        return Err("null string array".into());
    }
    std::slice::from_raw_parts(values, count)
        .iter()
        .map(|v| v.text().map(String::from))
        .collect()
}
fn status(f: impl FnOnce() -> Result<()>) -> i32 {
    if checked(f).is_ok() {
        0
    } else {
        -1
    }
}
#[no_mangle]
pub extern "C" fn nebula_tantivy_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_open(
    path: *const c_char,
    fields: *const ByteSlice,
    count: usize,
    writable: i32,
    memory: usize,
    analyzer: ByteSlice,
) -> *mut c_void {
    checked(|| {
        if path.is_null() {
            return Err("null index path".into());
        }
        let path = CStr::from_ptr(path).to_str().map_err(message)?;
        Engine::open(
            Path::new(path),
            &strings(fields, count)?,
            writable != 0,
            memory,
            analyzer.text()?,
        )
    })
    .map(|e| Box::into_raw(Box::new(e)) as *mut c_void)
    .unwrap_or(std::ptr::null_mut())
}
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_close(handle: *mut c_void) {
    let _ = checked(|| {
        if !handle.is_null() {
            drop(Box::from_raw(handle as *mut Engine));
        }
        Ok(())
    });
}
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_set_writable(handle: *mut c_void, writable: i32) -> i32 {
    status(|| engine(handle)?.set_writable(writable != 0))
}
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_upsert(
    handle: *mut c_void,
    key: ByteSlice,
    values: *const ByteSlice,
    count: usize,
) -> i32 {
    status(|| engine(handle)?.upsert(key.bytes()?, &strings(values, count)?))
}
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_remove(handle: *mut c_void, key: ByteSlice) -> i32 {
    status(|| engine(handle)?.remove(key.bytes()?))
}
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_clear(handle: *mut c_void) -> i32 {
    status(|| engine(handle)?.clear())
}
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_commit(handle: *mut c_void) -> i32 {
    status(|| engine(handle)?.commit())
}
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_reload(handle: *mut c_void) -> i32 {
    status(|| engine(handle)?.reader.reload().map_err(message))
}
type HitCallback = unsafe extern "C" fn(*mut c_void, *const u8, usize, f64);
#[no_mangle]
pub unsafe extern "C" fn nebula_tantivy_search(
    handle: *mut c_void,
    field: ByteSlice,
    pattern: ByteSlice,
    mode: i32,
    limit: usize,
    prefix: ByteSlice,
    callback: Option<HitCallback>,
    context: *mut c_void,
) -> i32 {
    status(|| {
        let callback = callback.ok_or("null hit callback")?;
        let hits =
            engine(handle)?.search(field.text()?, pattern.text()?, mode, limit, prefix.bytes()?)?;
        for hit in hits {
            callback(context, hit.key.as_ptr(), hit.key.len(), hit.score as f64);
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests;

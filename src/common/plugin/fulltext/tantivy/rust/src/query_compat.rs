//! Selected Lucene query-string leaves used by Nebula's native fulltext API.
//! Keep Tantivy's existing grammar for Boolean grouping, fields, phrases, ranges,
//! and boosts. Only wildcard, regexp, and fuzzy terms need alternate query nodes.
use std::collections::HashMap;

use tantivy::query::{
    AllQuery, BooleanQuery, BoostQuery, FuzzyTermQuery, Occur, Query, QueryParser, RegexQuery,
};
use tantivy::query_grammar::{self, UserInputAst, UserInputLeaf};
use tantivy::schema::{Field, FieldType};
use tantivy::tokenizer::TokenStream;
use tantivy::{Index, Term};

use crate::{message, Result};

#[derive(Clone, Debug)]
enum SpecialTerm {
    Wildcard(String),
    Regex(String),
    Fuzzy(String, u8),
}

pub(crate) fn parse(
    index: &Index,
    defaults: Vec<Field>,
    allowed_fields: &[(String, Field, Field)],
    input: &str,
) -> Result<Box<dyn Query>> {
    let (rewritten, specials) = rewrite(input)?;
    let ast = query_grammar::parse_query(&rewritten)
        .map_err(|_| format!("Invalid fulltext query syntax: {input}"))?;
    let parser = QueryParser::for_index(index, defaults.clone());
    build(index, &parser, &defaults, allowed_fields, &specials, ast)
}

fn build(
    index: &Index,
    parser: &QueryParser,
    defaults: &[Field],
    allowed: &[(String, Field, Field)],
    specials: &HashMap<String, SpecialTerm>,
    ast: UserInputAst,
) -> Result<Box<dyn Query>> {
    match ast {
        UserInputAst::Clause(children) => {
            let mut clauses = Vec::with_capacity(children.len());
            for (occur, child) in children {
                let occur = match occur {
                    Some(query_grammar::Occur::Must) => Occur::Must,
                    Some(query_grammar::Occur::MustNot) => Occur::MustNot,
                    _ => Occur::Should,
                };
                clauses.push((
                    occur,
                    build(index, parser, defaults, allowed, specials, child)?,
                ));
            }
            // Lucene query_string allows a pure negative group by adding match-all.
            if !clauses.is_empty() && clauses.iter().all(|(occur, _)| *occur == Occur::MustNot) {
                clauses.push((Occur::Must, Box::new(AllQuery)));
            }
            Ok(Box::new(BooleanQuery::new(clauses)))
        }
        UserInputAst::Boost(child, boost) => {
            if !boost.is_finite() || boost < 0.0 || boost > f32::MAX as f64 {
                return Err("Fulltext boost must be a finite nonnegative float".into());
            }
            Ok(Box::new(BoostQuery::new(
                build(index, parser, defaults, allowed, specials, *child)?,
                boost as f32,
            )))
        }
        UserInputAst::Leaf(leaf) => {
            let field_name = match leaf.as_ref() {
                UserInputLeaf::Literal(literal) => literal.field_name.as_deref(),
                UserInputLeaf::Range { field, .. } | UserInputLeaf::Set { field, .. } => {
                    field.as_deref()
                }
                UserInputLeaf::Exists { field } => Some(field.as_str()),
                UserInputLeaf::All => None,
            };
            let selected = if let Some(name) = field_name {
                vec![
                    allowed
                        .iter()
                        .find(|(field, _, _)| field == name)
                        .ok_or_else(|| format!("Unknown fulltext field: {name}"))?
                        .1,
                ]
            } else {
                defaults.to_vec()
            };
            if let UserInputLeaf::Literal(literal) = leaf.as_ref() {
                if let Some(special) = specials.get(&literal.phrase) {
                    let mut queries = Vec::new();
                    for field in selected {
                        queries.push((Occur::Should, special_query(index, field, special)?));
                    }
                    return Ok(Box::new(BooleanQuery::new(queries)));
                }
            }
            parser
                .build_query_from_user_input_ast(UserInputAst::Leaf(leaf))
                .map_err(message)
        }
    }
}

fn special_query(index: &Index, field: Field, special: &SpecialTerm) -> Result<Box<dyn Query>> {
    match special {
        SpecialTerm::Wildcard(pattern) => {
            let schema = index.schema();
            let lowercase = match schema.get_field_entry(field).field_type() {
                FieldType::Str(options) => options
                    .get_indexing_options()
                    .map(|options| options.tokenizer() != "raw")
                    .unwrap_or(false),
                _ => return Err("Wildcard queries require a text field".into()),
            };
            let regex = wildcard_regex(pattern, lowercase)?;
            RegexQuery::from_pattern(&regex, field)
                .map(|q| Box::new(q) as Box<dyn Query>)
                .map_err(message)
        }
        SpecialTerm::Regex(pattern) => {
            // Regexes operate on complete indexed terms. Do not lowercase syntax
            // such as \\D, Unicode classes, or character ranges behind the user's back.
            RegexQuery::from_pattern(pattern, field)
                .map(|q| Box::new(q) as Box<dyn Query>)
                .map_err(message)
        }
        SpecialTerm::Fuzzy(text, distance) => {
            let mut analyzer = index.tokenizer_for_field(field).map_err(message)?;
            let mut stream = analyzer.token_stream(text);
            if !stream.advance() {
                return Err("Fuzzy query has no searchable token".into());
            }
            let token = stream.token().text.clone();
            if stream.advance() {
                return Err("Fuzzy query must produce exactly one analyzer token".into());
            }
            Ok(Box::new(FuzzyTermQuery::new(
                Term::from_field_text(field, &token),
                *distance,
                true,
            )))
        }
    }
}

fn regex_literal(out: &mut String, ch: char) {
    if matches!(
        ch,
        '.' | '+' | '*' | '?' | '(' | ')' | '[' | ']' | '{' | '}' | '^' | '$' | '|' | '\\'
    ) {
        out.push('\\');
    }
    out.push(ch);
}
fn wildcard_regex(pattern: &str, lowercase: bool) -> Result<String> {
    let mut result = String::new();
    let mut chars = pattern.chars();
    while let Some(ch) = chars.next() {
        match ch {
            '*' => result.push_str(".*"),
            '?' => result.push('.'),
            _ => {
                let literal = if ch == '\\' {
                    chars.next().ok_or("Trailing wildcard escape")?
                } else {
                    ch
                };
                if lowercase {
                    for normalized in literal.to_lowercase() {
                        regex_literal(&mut result, normalized);
                    }
                } else {
                    regex_literal(&mut result, literal);
                }
            }
        }
    }
    Ok(result)
}

fn classify(word: &str) -> Result<Option<SpecialTerm>> {
    let mut escaped = false;
    let mut wildcard = false;
    let mut fuzzy_offset = None;
    for (offset, ch) in word.char_indices() {
        if escaped {
            escaped = false;
            continue;
        }
        if ch == '\\' {
            escaped = true;
            continue;
        }
        if ch == '*' || ch == '?' {
            wildcard = true;
        }
        if ch == '~' {
            if fuzzy_offset.is_some() {
                return Err("Multiple fuzzy suffixes are unsupported".into());
            }
            fuzzy_offset = Some(offset);
        }
    }
    if word == "*" {
        return Ok(None);
    }
    if let Some(offset) = fuzzy_offset {
        if wildcard {
            return Err("Combining wildcard and fuzzy syntax in one term is unsupported".into());
        }
        if offset == 0 {
            return Err("Fuzzy query is missing its term".into());
        }
        let suffix = &word[offset + 1..];
        let distance = if suffix.is_empty() {
            2
        } else {
            suffix
                .parse::<u8>()
                .map_err(|_| "Fuzzy edit distance must be 0, 1, or 2".to_string())?
        };
        if distance > 2 {
            return Err("Fuzzy edit distance must be 0, 1, or 2".into());
        }
        let mut text = String::new();
        let mut chars = word[..offset].chars();
        while let Some(ch) = chars.next() {
            text.push(if ch == '\\' {
                chars.next().ok_or("Trailing fuzzy escape")?
            } else {
                ch
            });
        }
        return Ok(Some(SpecialTerm::Fuzzy(text, distance)));
    }
    Ok(if wildcard {
        Some(SpecialTerm::Wildcard(word.into()))
    } else {
        None
    })
}

fn rewrite(input: &str) -> Result<(String, HashMap<String, SpecialTerm>)> {
    let mut prefix = "nebulacompatquerytoken".to_string();
    while input.contains(&prefix) {
        prefix.push('x');
    }
    let mut result = String::new();
    let mut specials = HashMap::new();
    let chars: Vec<(usize, char)> = input.char_indices().collect();
    let mut i = 0;
    while i < chars.len() {
        let (start, ch) = chars[i];
        // Quoted phrases and range bounds remain the native parser's responsibility.
        if matches!(ch, '"' | '\'' | '[' | '{') {
            let end_char = match ch {
                '[' => ']',
                '{' => '}',
                _ => ch,
            };
            let mut escaped = false;
            let mut quoted = None;
            let is_range = ch == '[' || ch == '{';
            i += 1;
            let mut closed = false;
            while i < chars.len() {
                let c = chars[i].1;
                if escaped {
                    escaped = false;
                    i += 1;
                    continue;
                }
                if c == '\\' {
                    escaped = true;
                    i += 1;
                    continue;
                }
                if is_range && (c == '"' || c == '\'') {
                    quoted = if quoted == Some(c) {
                        None
                    } else if quoted.is_none() {
                        Some(c)
                    } else {
                        quoted
                    };
                }
                if c == end_char && quoted.is_none() {
                    i += 1;
                    closed = true;
                    break;
                }
                i += 1;
            }
            if !closed {
                return Err("Unterminated fulltext phrase or range".into());
            }
            let end = chars
                .get(i)
                .map(|(offset, _)| *offset)
                .unwrap_or(input.len());
            result.push_str(&input[start..end]);
            // A phrase's ~N is proximity syntax, not a fuzzy term.
            if !is_range && i < chars.len() && chars[i].1 == '~' {
                result.push('~');
                i += 1;
                while i < chars.len() && chars[i].1.is_ascii_digit() {
                    result.push(chars[i].1);
                    i += 1;
                }
            }
            continue;
        }
        if ch == '/' {
            let mut pattern = String::new();
            i += 1;
            let mut closed = false;
            while i < chars.len() {
                let c = chars[i].1;
                if c == '/' {
                    i += 1;
                    closed = true;
                    break;
                }
                if c == '\\' {
                    i += 1;
                    if i >= chars.len() {
                        return Err("Trailing regex escape".into());
                    }
                    let next = chars[i].1;
                    if next != '/' {
                        pattern.push('\\');
                    }
                    pattern.push(next);
                    i += 1;
                    continue;
                }
                if matches!(c, '&' | '~' | '@' | '#' | '<' | '>') {
                    return Err("Lucene regexp intersection/complement/interval operators are not supported".into());
                }
                pattern.push(c);
                i += 1;
            }
            if !closed {
                return Err("Unterminated fulltext regexp".into());
            }
            let token = format!("{prefix}{}", specials.len());
            specials.insert(token.clone(), SpecialTerm::Regex(pattern));
            result.push_str(&token);
            continue;
        }
        if ch.is_whitespace() || matches!(ch, '(' | ')' | ':' | '^' | '+' | '-') {
            result.push(ch);
            i += 1;
            continue;
        }
        i += 1;
        while i < chars.len() {
            let c = chars[i].1;
            if c == '\\' {
                i = (i + 2).min(chars.len());
                continue;
            }
            if c.is_whitespace() || matches!(c, '(' | ')' | ':' | '^' | '"' | '\'' | '[' | '{') {
                break;
            }
            i += 1;
        }
        let end = chars
            .get(i)
            .map(|(offset, _)| *offset)
            .unwrap_or(input.len());
        let word = &input[start..end];
        // A field identifier cannot be substituted by a query leaf.
        let special = if i < chars.len() && chars[i].1 == ':' {
            None
        } else {
            classify(word)?
        };
        if let Some(special) = special {
            let token = format!("{prefix}{}", specials.len());
            specials.insert(token.clone(), special);
            result.push_str(&token);
        } else {
            result.push_str(word);
        }
    }
    Ok((result, specials))
}

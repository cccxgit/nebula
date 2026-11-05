use super::*;

fn create(path: &Path) -> Engine {
    Engine::open(
        path,
        &["title".into(), "body".into()],
        true,
        16_000_000,
        "standard",
    )
    .unwrap()
}
fn keys(engine: &Engine, field: &str, text: &str, mode: i32) -> Vec<Vec<u8>> {
    engine
        .search(field, text, mode, 0, &[])
        .unwrap()
        .into_iter()
        .map(|h| h.key)
        .collect()
}
#[test]
fn persistence_updates_deletes_and_binary_identity() {
    let dir = tempfile::tempdir().unwrap();
    let key = b"\0\x01\xff\x04vertex\0";
    {
        let mut e = create(dir.path());
        e.upsert(key, &["Hello WORLD".into(), "图数据库 Nebula Graph".into()])
            .unwrap();
        // A committed snapshot never exposes a partly indexed write.
        assert!(keys(&e, "title", "Hello", 1).is_empty());
        e.commit().unwrap();
        assert_eq!(keys(&e, "title", "Hello", 1), vec![key.to_vec()]);
        e.upsert(key, &["Replacement text".into(), "更新".into()])
            .unwrap();
        e.commit().unwrap();
        assert!(keys(&e, "title", "Hello", 1).is_empty());
        assert_eq!(keys(&e, "title", "Replacement", 1), vec![key.to_vec()]);
    }
    let mut e = create(dir.path());
    assert_eq!(keys(&e, "title", "Replacement", 1), vec![key.to_vec()]);
    e.remove(key).unwrap();
    e.commit().unwrap();
    assert!(keys(&e, "title", "", 1).is_empty());
}
#[test]
fn substring_results_equal_original_case_sensitive_unicode_semantics() {
    let dir = tempfile::tempdir().unwrap();
    let mut e = create(dir.path());
    let texts = [
        "",
        "a",
        "ab",
        "abc",
        "ABC",
        "xabcx",
        "ab cab",
        "图数据库 Nebula",
        "🔥图数据库🔥",
        "a\0b",
        "数据 数据库",
        "abcxyzabcy",
        "école",
        "e\u{301}cole",
    ];
    for (i, text) in texts.iter().enumerate() {
        e.upsert(&[i as u8], &[text.to_string(), "".into()])
            .unwrap();
    }
    e.commit().unwrap();
    for pattern in [
        "",
        "a",
        "ab",
        "abc",
        "ABC",
        "ab c",
        "图",
        "数据库",
        "🔥图",
        "\0",
        "é",
        "e\u{301}",
        "abcxyzabcy",
    ] {
        for mode in 1..=3 {
            let expected: Vec<Vec<u8>> = texts
                .iter()
                .enumerate()
                .filter(|(_, value)| match mode {
                    1 => value.contains(pattern),
                    2 => value.starts_with(pattern),
                    3 => value.ends_with(pattern),
                    _ => unreachable!(),
                })
                .map(|(i, _)| vec![i as u8])
                .collect();
            assert_eq!(
                keys(&e, "title", pattern, mode),
                expected,
                "pattern={pattern:?}, mode={mode}"
            );
        }
    }
}
#[test]
fn fulltext_phrase_boolean_fields_and_chinese() {
    let dir = tempfile::tempdir().unwrap();
    let mut e = create(dir.path());
    e.upsert(
        b"1",
        &["The old man and the sea".into(), "图数据库全文检索".into()],
    )
    .unwrap();
    e.upsert(b"2", &["A young man".into(), "sea".into()])
        .unwrap();
    e.commit().unwrap();
    assert_eq!(keys(&e, "title", "\"old man\"", 0), vec![b"1".to_vec()]);
    assert_eq!(keys(&e, "title", "man AND old", 0), vec![b"1".to_vec()]);
    assert_eq!(keys(&e, "title", "man NOT young", 0), vec![b"1".to_vec()]);
    assert_eq!(keys(&e, "body", "\"图数据库\"", 0), vec![b"1".to_vec()]);
    assert_eq!(keys(&e, "", "title:young", 0), vec![b"2".to_vec()]);
    assert!(e.search("title", "\"unterminated", 0, 0, &[]).is_err());
}
#[test]
fn filter_prefix_and_verify_before_limit() {
    let dir = tempfile::tempdir().unwrap();
    let mut e = create(dir.path());
    e.upsert(b"part1", &["abc noise bcd".into(), "".into()])
        .unwrap();
    e.upsert(b"part2", &["abcd true".into(), "".into()])
        .unwrap();
    e.upsert(b"else3", &["abcd other partition".into(), "".into()])
        .unwrap();
    e.commit().unwrap();
    let hits = e.search("title", "abcd", 1, 1, b"part").unwrap();
    assert_eq!(hits.len(), 1);
    assert_eq!(hits[0].key, b"part2");
    assert!(e
        .search("title", "abcd", 1, 1, b"part1")
        .unwrap()
        .is_empty());
}
#[test]
fn readonly_transition_clear_and_schema_validation() {
    let dir = tempfile::tempdir().unwrap();
    let mut e = create(dir.path());
    e.upsert(b"1", &["hello".into(), "".into()]).unwrap();
    e.set_writable(false).unwrap();
    assert_eq!(keys(&e, "title", "hello", 1).len(), 1);
    assert!(e.remove(b"1").is_err());
    e.set_writable(true).unwrap();
    e.clear().unwrap();
    e.commit().unwrap();
    assert!(keys(&e, "title", "", 1).is_empty());
    assert!(Engine::open(
        dir.path(),
        &["different".into()],
        false,
        16_000_000,
        "standard"
    )
    .is_err());
    assert!(Engine::open(
        dir.path(),
        &["title".into(), "body".into()],
        false,
        16_000_000,
        "unsupported"
    )
    .is_err());
}
#[test]
fn keyword_and_jieba_analyzers() {
    for analyzer in ["keyword", "jieba"] {
        let dir = tempfile::tempdir().unwrap();
        let mut e = Engine::open(dir.path(), &["body".into()], true, 16_000_000, analyzer).unwrap();
        e.upsert(b"1", &["中国科学院 Nebula".into()]).unwrap();
        e.commit().unwrap();
        let query = if analyzer == "keyword" {
            "\"中国科学院 Nebula\""
        } else {
            "中国科学院"
        };
        assert_eq!(keys(&e, "body", query, 0), vec![b"1".to_vec()]);
        assert_eq!(keys(&e, "body", "科学院", 1), vec![b"1".to_vec()]);
    }
}

#[test]
fn partition_filter_does_not_change_text_relevance_score() {
    let dir = tempfile::tempdir().unwrap();
    let mut e = create(dir.path());
    // Unequal partition populations would give the two prefix terms different
    // IDF scores if the partition constraint accidentally contributed to BM25.
    e.upsert(b"onea1", &["nebula graph".into(), "".into()])
        .unwrap();
    e.upsert(b"twob1", &["nebula graph".into(), "".into()])
        .unwrap();
    e.upsert(b"twob2", &["other words".into(), "".into()])
        .unwrap();
    e.upsert(b"twob3", &["other words".into(), "".into()])
        .unwrap();
    e.commit().unwrap();

    let first = e.search("title", "nebula", 0, 0, b"onea").unwrap();
    let second = e.search("title", "nebula", 0, 0, b"twob").unwrap();
    let unfiltered = e.search("title", "nebula", 0, 0, &[]).unwrap();
    assert_eq!(first.len(), 1);
    assert_eq!(second.len(), 1);
    assert_eq!(unfiltered.len(), 2);
    assert_eq!(first[0].key, b"onea1");
    assert_eq!(second[0].key, b"twob1");
    assert!(first[0].score > 0.0);
    assert_eq!(first[0].score, second[0].score);
    assert_eq!(first[0].score, unfiltered[0].score);
    assert_eq!(first[0].score, unfiltered[1].score);
}

#[test]
fn native_query_string_regression_matrix() {
    let dir = tempfile::tempdir().unwrap();
    let mut e = create(dir.path());
    let fixture = [
        ("r1", "quick brown fox", "new york river water"),
        ("r2", "quick blue hare", "new jersey lake"),
        ("r3", "slow brown fox", "old york river"),
        ("r4", "quack bronze bird", "new york harbor"),
        ("r5", "aquatic watercraft", "water river water"),
        ("r6", "beach", "sand dry"),
        ("r7", "New York", "quoted \"fox\" and C++"),
        ("r8", "the Quick hare", "new old york"),
    ];
    for (id, title, body) in fixture {
        e.upsert(id.as_bytes(), &[title.into(), body.into()])
            .unwrap();
    }
    e.commit().unwrap();
    // Same genuine text/expected IDs as the executable database regression fixture.
    let cases: &[(&str, &[&str])] = &[
        ("title:quick", &["r1", "r2", "r8"]),
        ("title:QUICK", &["r1", "r2", "r8"]),
        ("body:\"new york\"", &["r1", "r4"]),
        ("title:quick AND body:river", &["r1"]),
        ("title:fox OR title:hare", &["r1", "r2", "r3", "r8"]),
        ("title:quick AND NOT body:jersey", &["r1", "r8"]),
        (
            "title:(quick OR quack) AND body:new",
            &["r1", "r2", "r4", "r8"],
        ),
        ("harbor", &["r4"]),
        ("title:bro*", &["r1", "r3", "r4"]),
        ("title:*row*", &["r1", "r3"]),
        ("title:qu?ck", &["r1", "r2", "r4", "r8"]),
        ("title:/qu(i|a)ck/", &["r1", "r2", "r4", "r8"]),
        ("title:/.*row.*/", &["r1", "r3"]),
        ("title:quik~1", &["r1", "r2", "r8"]),
        ("body:\"new york\"~1", &["r1", "r4", "r8"]),
        ("title:fox^4 OR title:hare", &["r1", "r2", "r3", "r8"]),
        ("title:[blue TO brown]", &["r1", "r2", "r3", "r4"]),
        (
            "title:(BRO* OR beach) AND NOT body:harbor",
            &["r1", "r3", "r6"],
        ),
        ("title:quik~1 AND body:/new/", &["r1", "r2", "r8"]),
        ("title:quik~0", &[]),
    ];
    for (query, expected) in cases {
        let mut actual = keys(&e, "", query, 0);
        actual.sort_unstable();
        let expected: Vec<Vec<u8>> = expected.iter().map(|key| (*key).as_bytes().to_vec()).collect();
        assert_eq!(actual, expected, "query={query}");
    }
    for query in [
        "title:\"unterminated",
        "title:/unterminated",
        "title:foo~3",
        "title:foo~x",
        "title:foo*~1",
        "title:/a&b/",
        "__nebula_gram_0:foo",
        "__nebula_prefix4:foo*",
    ] {
        assert!(
            e.search("", query, 0, 0, &[]).is_err(),
            "query must reject unsupported syntax: {query}"
        );
    }
}

#[test]
fn query_compat_respects_keyword_case_and_escaped_wildcards() {
    let dir = tempfile::tempdir().unwrap();
    let mut e = Engine::open(dir.path(), &["text".into()], true, 16_000_000, "keyword").unwrap();
    e.upsert(b"1", &["Da*name".into()]).unwrap();
    e.upsert(b"2", &["Database".into()]).unwrap();
    e.upsert(b"3", &["database".into()]).unwrap();
    e.commit().unwrap();
    assert_eq!(keys(&e, "text", r"Da\**", 0), vec![b"1".to_vec()]);
    assert_eq!(keys(&e, "text", "data*", 0), vec![b"3".to_vec()]);
    assert_eq!(keys(&e, "text", "/Data.*/", 0), vec![b"2".to_vec()]);
    assert_eq!(keys(&e, "text", "Databas~1", 0), vec![b"2".to_vec()]);
}

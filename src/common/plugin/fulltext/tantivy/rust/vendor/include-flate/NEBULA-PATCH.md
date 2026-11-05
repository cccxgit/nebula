# Local dependency configuration patch

Upstream: https://github.com/SOF3/include-flate, crates.io include-flate 0.3.5.
Original published crate checksum: `f677e494e19ac4aa62375916f64b496f6b2655964c7313231c959858608cd9eb`.

All Rust source files are unchanged. The only change to upstream Cargo.toml is
to make the default feature set `deflate` instead of `deflate,zstd`. Jieba 0.8.1
uses the default DEFLATE dictionary macro, and its unconditional dependency on
include-flate would otherwise enable a second native zstd implementation inside
Nebula, conflicting with Nebula third-party libzstd. Optional zstd support remains
defined but is not enabled by this application. Original Apache-2.0 LICENSE is
retained.

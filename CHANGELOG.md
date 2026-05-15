# sakura-lsldb — Changelog

All notable changes are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).
The `[Unreleased]` section is what's on `main`; the release pipeline
promotes it to a numbered version on tag.

## [Unreleased]

## [1.0.0] — 2026-05-15

### Added
- Initial release of the Sakura LSL source-level debugger: a `gdb`-
  style REPL that attaches to a `sakura-slemu` instance over the
  `--debug` socket protocol.
- Core debugger client (`src/lsldb.c`, `src/lsldb.h`) with the
  standard command vocabulary: `break` / `delete`, `run`, `continue`,
  `step`, `next`, `finish`, `list`, `print`, `backtrace`, `info`,
  `quit`, plus state/event-aware variants for LSL programs.
- Source-line breakpoints (relies on SLBC v2 line tables from
  `sakura-lslc`) and local/global variable inspection at any stop.
- Integration test runner under `tests/run_tests.sh` driving the
  debugger end-to-end against a live slemu session.

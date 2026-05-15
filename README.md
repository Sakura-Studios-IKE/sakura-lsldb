# sakura-lsldb

**A gdb-style CLI debugger for LSL scripts — by Sakura Studios, IKE.**

`sakura-lsldb` (binary: `lsldb`) drives
[`sakura-slemu`](https://github.com/ShihoSakura/sakura-slemu)
through its `--debug` JSON line-protocol and gives you a gdb-like REPL
to step through LSL line-by-line, set breakpoints, inspect variables,
catch events, and pause/resume the entire simulated region.

```sh
$ git clone https://github.com/ShihoSakura/sakura-lsldb.git
$ cd sakura-lsldb && make
$ ../sakura-lslc/lslc -c my_script.lsl
$ ./lsldb --source my_script.lsl -- --volume /tmp/v my_script.lslbc
lsldb attached to slemu (type `help`)
[stopped] reason=entry script=Object line=0
(lsldb) break 12
  breakpoint #1 at ?:12
(lsldb) catch money
  catchpoint #1 on money
(lsldb) continue
[stopped] reason=breakpoint script=Object line=12
    10      state_entry()
    11      {
>   12          llSetText("ready", <1,1,1>, 1);
    13          llListen(0, "", NULL_KEY, "");
(lsldb) print counter
  counter : integer = 0
(lsldb) step
[stopped] reason=step script=Object line=13
(lsldb) continue
[caught] money: 1111...-> 2222... L$5
[stopped] reason=catch script=Object line=0
(lsldb) backtrace
  call frames: ...
(lsldb) quit
```

## Commands

| Command | Effect |
|---------|--------|
| `run` / `continue` / `c` | Start (or resume) execution until the next stop. |
| `step` / `s` | Single-step one source statement. |
| `break FILE:LINE` / `break LINE` | Set a source-level breakpoint. |
| `delete N` | Remove breakpoint number N. |
| `info` | List breakpoints + catchpoints. |
| `catch KIND` | Stop on the next event of KIND: `chat`, `money`, `dialog`, `state_change`, `http_out`, … |
| `uncatch N` | Remove catchpoint N. |
| `print NAME` / `p NAME` | Evaluate a variable (local → param → global → builtin constant). |
| `locals` | List the current frame's locals + params. |
| `globals` | List the script's globals. |
| `backtrace` / `bt` | Show call frames. |
| `list` / `l` | Show source around the current line. |
| `source PATH` | Register an LSL source file so `list` can show context. |
| `snapshot` | Dump the entire region state (avatars, groups, dialogs, hud). |
| `events on\|off` | Toggle live world-event echoing while paused. |
| `help` / `h` / `?` | Show the command list. |
| `quit` / `q` | Exit (also kills slemu). |

## Install

```sh
make                                        # Linux / macOS / *BSD / MinGW
cmake -B build && cmake --build build       # cross-platform
sudo make install                           # /usr/local/bin/lsldb
```

`lsldb` needs the `slemu` binary to spawn — it looks for it on `PATH`
first, then `./slemu`, `./sakura-slemu/slemu`, `../sakura-slemu/slemu`.
Override with `--slemu /path/to/slemu`.

## How it works

The slemu runtime takes a `--debug` flag that enables a small JSON
control protocol on stdin/stdout. lsldb forks slemu with that flag,
parses the events it streams, and prints them nicely. Source-level
breakpoints work because the SLBC bytecode (v2+) carries a `line_no`
on every statement, function, and event handler — slemu checks the
list of active breakpoints before executing each statement.

## Project layout

```
sakura-lsldb/
├── README.md
├── LICENSE          MIT
├── Makefile
├── src/
│   ├── lsldb.h
│   └── lsldb.c     ~600 lines C99; spawns slemu, pumps the REPL
└── tests/
```

## Author / Attribution

Authored by **Shiho Sakura**
([@ShihoSakura](https://github.com/ShihoSakura)) on behalf of
**Sakura Studios, IKE**. MIT-licensed.

Companion repos:

* [`sakura-lslc`](https://github.com/ShihoSakura/sakura-lslc) — compiler
* [`sakura-slemu`](https://github.com/ShihoSakura/sakura-slemu) — runtime
* [`sakura-lsltest`](https://github.com/ShihoSakura/sakura-lsltest) — test framework

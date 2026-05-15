/* lsldb.h — sakura-lsldb, the gdb-like front-end for sakura-slemu.
 *
 * Architecture: lsldb is a small C99 program that
 *
 *   1. spawns sakura-slemu as a child process with `--debug` (see
 *      sakura-slemu/src/dbg.c for the protocol),
 *   2. forwards every command typed at our REPL as a JSON line on
 *      slemu's stdin,
 *   3. reads slemu's stdout, separating debugger-control events
 *      (`{"dbg":...}`) from world-state events (`{"type":...}`) and
 *      pretty-printing both,
 *   4. keeps a local cache of the LSL source so it can show context
 *      around the current line on every break.
 */
#ifndef LSLDB_H
#define LSLDB_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    pid_t child;
    FILE *to_child;      /* write commands here  (becomes child stdin) */
    FILE *from_child;    /* read events here     (becomes child stdout) */
    int   color;
    int   exited;
    /* source map: file -> array of lines (NUL-terminated) for showing context */
    struct SrcFile *src_files;
    /* repl history list */
    char **history;
    int    n_history;
} DbgCtx;

void   dbg_repl(DbgCtx *c);
void   dbg_send(DbgCtx *c, const char *fmt, ...);
char  *dbg_recv_line(DbgCtx *c);   /* heap, caller frees, NULL on EOF */
void   dbg_load_source(DbgCtx *c, const char *path);
void   dbg_show_line(DbgCtx *c, const char *file, int line, int context);

void  *xmalloc(size_t n);
char  *xstrdup(const char *s);

#endif

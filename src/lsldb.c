/* lsldb.c — full sakura-lsldb implementation.
 *
 * Usage:
 *   lsldb [--slemu PATH] [--lslc PATH] [--source PATH]... -- <slemu args ...> script.lslbc
 *
 * Inside the REPL: gdb-like commands.
 *   help, h, ?                show command list
 *   run, r                    start (or restart) execution
 *   continue, c               resume until next stop
 *   step, s                   single-step one source statement
 *   break LINE [in FILE]      set a breakpoint
 *   break FILE:LINE
 *   delete N                  remove breakpoint N
 *   info breakpoints          list breakpoints
 *   catch KIND                stop on the next chat/money/dialog/state_change/...
 *   uncatch N                 remove catchpoint
 *   print NAME, p NAME        show a variable value
 *   locals                    list current frame locals
 *   globals                   list globals
 *   backtrace, bt             call stack
 *   snapshot                  full region state
 *   list, l                   show source around current line
 *   source PATH               register a .lsl file so `list` works
 *   events ON|OFF             toggle whether world events are shown live
 *   quit, q                   exit
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "lsldb.h"
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct SrcFile {
    char *path;
    char **lines;
    int n_lines;
    struct SrcFile *next;
} SrcFile;

#ifdef _WIN32
#include <io.h>
#define ISATTY _isatty
#define FILENO _fileno
#else
#define ISATTY isatty
#define FILENO fileno
#endif

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fputs("oom\n", stderr); exit(2); } return p; }
char *xstrdup(const char *s) { size_t l = strlen(s); char *r = xmalloc(l+1); memcpy(r, s, l+1); return r; }
static char *xstrndup(const char *s, size_t n) { char *r = xmalloc(n+1); memcpy(r, s, n); r[n] = '\0'; return r; }

/* --------- IO ---------- */
void dbg_send(DbgCtx *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(c->to_child, fmt, ap);
    va_end(ap);
    fputc('\n', c->to_child);
    fflush(c->to_child);
}

char *dbg_recv_line(DbgCtx *c) {
    char buf[8192];
    if (!fgets(buf, sizeof buf, c->from_child)) return NULL;
    return xstrdup(buf);
}

/* --------- Source cache ---------- */
void dbg_load_source(DbgCtx *c, const char *path) {
    for (SrcFile *f = c->src_files; f; f = f->next)
        if (strcmp(f->path, path) == 0) return;
    FILE *fp = fopen(path, "rb"); if (!fp) return;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); rewind(fp);
    if (sz < 0) { fclose(fp); return; }
    char *buf = xmalloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return; }
    buf[sz] = '\0';
    fclose(fp);
    int nl = 1; for (char *p = buf; *p; p++) if (*p == '\n') nl++;
    char **lines = xmalloc(sizeof(char*) * (size_t)nl);
    int idx = 0;
    char *p = buf;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 0 && p[l-1] == '\r') l--;
        lines[idx++] = xstrndup(p, l);
        if (!e) break;
        p = e + 1;
    }
    free(buf);
    SrcFile *f = xmalloc(sizeof *f);
    f->path = xstrdup(path);
    f->lines = lines;
    f->n_lines = idx;
    f->next = c->src_files;
    c->src_files = f;
}

static SrcFile *find_src(DbgCtx *c, const char *path) {
    if (!path) return c->src_files;
    for (SrcFile *f = c->src_files; f; f = f->next)
        if (strstr(f->path, path) || strstr(path, f->path)) return f;
    return c->src_files;
}

void dbg_show_line(DbgCtx *c, const char *file, int line, int context) {
    SrcFile *f = find_src(c, file);
    if (!f) return;
    int lo = line - context; if (lo < 1) lo = 1;
    int hi = line + context; if (hi > f->n_lines) hi = f->n_lines;
    for (int i = lo; i <= hi; i++) {
        const char *marker = (i == line) ? ">" : " ";
        if (c->color) printf("%s\x1b[2m%4d\x1b[0m  %s%s%s\n",
            i == line ? "\x1b[1;33m" : "",
            i,
            i == line ? "\x1b[0m" : "",
            f->lines[i-1],
            i == line ? "" : "");
        else printf("%s %4d  %s\n", marker, i, f->lines[i-1]);
    }
}

/* --------- JSON probing (very small) ---------- */
static int j_str(const char *line, const char *key, char *out, size_t outlen) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outlen) {
        if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; }
        else out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}
static int j_int(const char *line, const char *key, long *out) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    *out = strtol(p, NULL, 10);
    return 1;
}

/* --------- Wait for one debugger response after a command ----------
 * Drains world events (printing them) until we see a `{"dbg":...}` line.
 * Returns that line (heap). Stops on EOF, returns NULL. */
static int show_world_events = 1;
static char *drain_until_dbg(DbgCtx *c) {
    char *line;
    while ((line = dbg_recv_line(c))) {
        if (strstr(line, "\"dbg\":")) return line;
        /* World event */
        if (show_world_events) {
            /* Pretty-print common types compactly. */
            char type[64] = "";
            j_str(line, "type", type, sizeof type);
            if (*type) {
                if (c->color) printf("\x1b[2m | \x1b[0m");
                else printf(" | ");
                printf("%s", line);
            } else {
                printf("%s", line);
            }
        }
        free(line);
    }
    return NULL;
}

/* --------- Pretty-printers for dbg events ---------- */
static void print_stopped(DbgCtx *c, const char *line) {
    char reason[64] = "?", script[128] = "?";
    long ln = 0;
    j_str(line, "reason", reason, sizeof reason);
    j_str(line, "script", script, sizeof script);
    j_int(line, "line", &ln);
    if (c->color) printf("\x1b[1;33m[stopped]\x1b[0m reason=%s script=%s line=%ld\n",
        reason, script, ln);
    else printf("[stopped] reason=%s script=%s line=%ld\n", reason, script, ln);
    if (ln > 0) dbg_show_line(c, NULL, (int)ln, 2);
}

static void print_value(const char *line) {
    char name[128]="", type[32]="", val[1024]="";
    j_str(line, "name", name, sizeof name);
    j_str(line, "type", type, sizeof type);
    j_str(line, "value", val, sizeof val);
    printf("  %s : %s = %s\n", name, type, val);
}

static void print_items(const char *line, const char *kind) {
    printf("  %s:\n", kind);
    const char *p = line;
    while ((p = strstr(p, "{\"name\":"))) {
        char nm[128]="", ty[32]="", v[1024]="";
        char tmp[2048]; size_t L = 0;
        const char *q = p;
        int depth = 0;
        while (*q && L + 1 < sizeof tmp) {
            tmp[L++] = *q;
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        tmp[L] = '\0';
        j_str(tmp, "name", nm, sizeof nm);
        j_str(tmp, "type", ty, sizeof ty);
        j_str(tmp, "value", v, sizeof v);
        printf("    %-24s %-10s = %s\n", nm, ty, v);
        p = q;
    }
}

static int g_last_line = 0;
/* Decide what to do with a `{"dbg":...}` line and return whether to prompt
 * the user again (1) or wait for more events (0). */
static int handle_dbg_event(DbgCtx *c, char *line, int *exited) {
    char what[64] = "";
    j_str(line, "dbg", what, sizeof what);
    if (!strcmp(what, "stopped")) {
        long ln = 0; j_int(line, "line", &ln);
        if (ln > 0) g_last_line = (int)ln;
        print_stopped(c, line); return 1;
    }
    if (!strcmp(what, "running"))  return 0;
    if (!strcmp(what, "breakpoint")) {
        long id = 0, ln = 0;
        char file[256] = "";
        j_int(line, "id", &id); j_int(line, "line", &ln);
        j_str(line, "file", file, sizeof file);
        printf("  breakpoint #%ld at %s:%ld\n", id, *file ? file : "?", ln);
        return 1;
    }
    if (!strcmp(what, "value"))   { print_value(line); return 1; }
    if (!strcmp(what, "locals"))  { print_items(line, "locals"); return 1; }
    if (!strcmp(what, "globals")) { print_items(line, "globals"); return 1; }
    if (!strcmp(what, "frames")) {
        printf("  call frames: %s", line);
        return 1;
    }
    if (!strcmp(what, "breakpoints")) {
        printf("  %s", line);
        return 1;
    }
    if (!strcmp(what, "caught")) {
        char kind[64]="", detail[512]="";
        j_str(line, "kind", kind, sizeof kind);
        j_str(line, "detail", detail, sizeof detail);
        if (c->color) printf("\x1b[1;35m[caught]\x1b[0m %s: %s\n", kind, detail);
        else printf("[caught] %s: %s\n", kind, detail);
        return 0;
    }
    if (!strcmp(what, "error"))   { printf("  error: %s", line); return 1; }
    if (!strcmp(what, "exit"))    { *exited = 1; printf("[slemu exited]\n"); return 1; }
    if (!strcmp(what, "catchpoint")) {
        long id = 0; char k[64] = "";
        j_int(line, "id", &id); j_str(line, "kind", k, sizeof k);
        printf("  catchpoint #%ld on %s\n", id, k);
        return 1;
    }
    if (!strcmp(what, "catchpoints")) { printf("  %s", line); return 1; }
    printf("  dbg: %s", line);
    return 1;
}

/* --------- REPL ---------- */

static void show_help(void) {
    puts(
"lsldb commands:\n"
"  run                start / restart execution\n"
"  continue (c)       resume until next stop\n"
"  step (s)           step one statement\n"
"  break FILE:LINE    set a breakpoint\n"
"  break LINE         set a breakpoint in any file at LINE\n"
"  delete N           remove breakpoint N\n"
"  info               list breakpoints + catchpoints\n"
"  catch KIND         break on chat/money/dialog/state_change/http_out\n"
"  uncatch N          remove catchpoint N\n"
"  print NAME (p)     evaluate a variable\n"
"  locals             list current locals\n"
"  globals            list globals\n"
"  backtrace (bt)     show call frames\n"
"  list (l)           show source around current line\n"
"  source PATH        register an LSL source file for `list`\n"
"  snapshot           dump the entire region state\n"
"  events on|off      toggle live world-event echo\n"
"  help (h)           show this help\n"
"  quit (q)           exit\n");
}

/* Wait for stopped/exit, draining world events along the way. */
static int wait_for_stop(DbgCtx *c) {
    int exited = 0;
    for (;;) {
        char *l = drain_until_dbg(c);
        if (!l) { c->exited = 1; return 0; }
        int prompt = handle_dbg_event(c, l, &exited);
        free(l);
        if (exited) { c->exited = 1; return 0; }
        if (prompt) return 1;
    }
}

void dbg_repl(DbgCtx *c) {
    /* Wait for the initial entry-stop event */
    if (!wait_for_stop(c)) return;
    char buf[1024];
    for (;;) {
        if (c->color) fputs("\x1b[1;36m(lsldb)\x1b[0m ", stdout);
        else fputs("(lsldb) ", stdout);
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) break;
        char *line = buf;
        while (*line == ' ' || *line == '\t') line++;
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r' || line[L-1] == ' ')) line[--L] = '\0';
        if (!*line) continue;

        char cmd[64]; cmd[0] = '\0';
        sscanf(line, "%63s", cmd);
        const char *rest = line + strlen(cmd);
        while (*rest == ' ') rest++;

        if (!strcmp(cmd, "help") || !strcmp(cmd, "h") || !strcmp(cmd, "?")) {
            show_help(); continue;
        }
        if (!strcmp(cmd, "quit") || !strcmp(cmd, "q") || !strcmp(cmd, "exit")) {
            dbg_send(c, "{\"cmd\":\"quit\"}");
            (void)wait_for_stop(c);
            break;
        }
        if (!strcmp(cmd, "continue") || !strcmp(cmd, "c") || !strcmp(cmd, "run") || !strcmp(cmd, "r")) {
            dbg_send(c, "{\"cmd\":\"continue\"}");
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "step") || !strcmp(cmd, "s") || !strcmp(cmd, "next") || !strcmp(cmd, "n")) {
            dbg_send(c, "{\"cmd\":\"step\"}");
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "break") || !strcmp(cmd, "b")) {
            char file[256] = "";
            long line_no = 0;
            if (strchr(rest, ':')) {
                /* file:line */
                const char *col = strchr(rest, ':');
                size_t fl = (size_t)(col - rest);
                if (fl >= sizeof file) fl = sizeof file - 1;
                memcpy(file, rest, fl); file[fl] = '\0';
                line_no = strtol(col + 1, NULL, 10);
                dbg_send(c, "{\"cmd\":\"break\",\"file\":\"%s\",\"line\":%ld}", file, line_no);
            } else {
                line_no = strtol(rest, NULL, 10);
                dbg_send(c, "{\"cmd\":\"break\",\"line\":%ld}", line_no);
            }
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "delete") || !strcmp(cmd, "clear")) {
            long id = strtol(rest, NULL, 10);
            dbg_send(c, "{\"cmd\":\"clear\",\"id\":%ld}", id);
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "info")) {
            dbg_send(c, "{\"cmd\":\"breakpoints\"}");
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "catch")) {
            dbg_send(c, "{\"cmd\":\"catch\",\"kind\":\"%s\"}", rest);
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "uncatch")) {
            long id = strtol(rest, NULL, 10);
            dbg_send(c, "{\"cmd\":\"uncatch\",\"id\":%ld}", id);
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "print") || !strcmp(cmd, "p")) {
            dbg_send(c, "{\"cmd\":\"print\",\"name\":\"%s\"}", rest);
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "locals")) {
            dbg_send(c, "{\"cmd\":\"locals\"}");
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "globals")) {
            dbg_send(c, "{\"cmd\":\"globals\"}");
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "backtrace") || !strcmp(cmd, "bt")) {
            dbg_send(c, "{\"cmd\":\"backtrace\"}");
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "snapshot")) {
            dbg_send(c, "{\"cmd\":\"snapshot\"}");
            if (!wait_for_stop(c)) break;
            continue;
        }
        if (!strcmp(cmd, "list") || !strcmp(cmd, "l")) {
            if (g_last_line > 0) dbg_show_line(c, NULL, g_last_line, 4);
            else puts("  (no current line — set a breakpoint first)");
            continue;
        }
        if (!strcmp(cmd, "source")) {
            dbg_load_source(c, rest);
            printf("  loaded %s\n", rest);
            continue;
        }
        if (!strcmp(cmd, "events")) {
            show_world_events = !strcmp(rest, "on") || !strcmp(rest, "ON") || !strcmp(rest, "1");
            printf("  world events %s\n", show_world_events ? "ON" : "OFF");
            continue;
        }
        printf("  unknown command '%s' — try `help`\n", cmd);
    }
    if (c->child) {
        int st;
        kill(c->child, SIGTERM);
        waitpid(c->child, &st, 0);
    }
}

/* --------- main ---------- */
static void usage(const char *p) {
    fprintf(stderr,
"Usage: %s [options] -- <slemu args...> <script.lslbc>\n"
"\n"
"  --slemu PATH       path to the sakura-slemu binary (default: slemu or ../sakura-slemu/slemu)\n"
"  --source PATH      register an LSL source file (repeatable)\n"
"  -h, --help         show this help\n"
"\n"
"Inside the REPL type `help` for the command list.\n"
"\n"
"Examples:\n"
"  lsldb -- greeter.lslbc                          # simplest interactive run\n"
"  lsldb --source greeter.lsl -- greeter.lslbc     # with source listings\n"
"  lsldb --slemu ./build-debug/slemu \\\n"
"        --source club_vendor.lsl \\\n"
"        -- --config worlds/club.cfg \\\n"
"           --commands scenarios/buy_one.cmd \\\n"
"           club_vendor.lslbc                      # multi-avatar debug session\n"
"  printf 'break 4\\ncontinue\\nprint msg\\nquit\\n' \\\n"
"    | lsldb --source greeter.lsl -- greeter.lslbc # scripted via shell pipe\n"
"\n"
"See lsldb(1) for the complete manual.\n",
        p);
}

static const char *find_slemu(void) {
    static char buf[1024];
    const char *candidates[] = {
        "./slemu", "./sakura-slemu/slemu", "../sakura-slemu/slemu",
        "../../sakura-slemu/slemu", "/usr/local/bin/slemu", NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(buf, sizeof buf, "%s", candidates[i]);
            return buf;
        }
    }
    /* fallback: hope PATH has it */
    return "slemu";
}

int main(int argc, char **argv) {
    DbgCtx c; memset(&c, 0, sizeof c);
    c.color = ISATTY(FILENO(stdout));
    const char *slemu = NULL;
    int i = 1;
    char **src_paths = NULL; int n_src = 0;
    while (i < argc) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--slemu")) { slemu = argv[++i]; i++; continue; }
        if (!strcmp(argv[i], "--source")) {
            src_paths = realloc(src_paths, sizeof(char*) * (size_t)(n_src + 1));
            src_paths[n_src++] = argv[++i];
            i++; continue;
        }
        if (!strcmp(argv[i], "--")) { i++; break; }
        break;
    }
    if (i >= argc) { usage(argv[0]); return 2; }
    if (!slemu) slemu = find_slemu();

    /* Pipes */
    int to_child[2], from_child[2];
    if (pipe(to_child) < 0 || pipe(from_child) < 0) { perror("pipe"); return 2; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        dup2(to_child[0], 0);
        dup2(from_child[1], 1);
        close(to_child[1]); close(from_child[0]);
        /* Build child argv: slemu --debug <extra> */
        int n_extra = argc - i;
        char **child_argv = malloc(sizeof(char*) * (size_t)(n_extra + 3));
        child_argv[0] = (char*)slemu;
        child_argv[1] = "--debug";
        for (int k = 0; k < n_extra; k++) child_argv[2 + k] = argv[i + k];
        child_argv[2 + n_extra] = NULL;
        execvp(slemu, child_argv);
        perror(slemu);
        _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    c.child = pid;
    c.to_child = fdopen(to_child[1], "w");
    c.from_child = fdopen(from_child[0], "r");
    if (!c.to_child || !c.from_child) { perror("fdopen"); return 2; }
    /* Line-buffered to_child so commands flush quickly */
    setvbuf(c.to_child, NULL, _IOLBF, 0);

    for (int k = 0; k < n_src; k++) dbg_load_source(&c, src_paths[k]);

    if (c.color) printf("\x1b[1mlsldb\x1b[0m attached to slemu (type `help`)\n");
    else printf("lsldb attached to slemu (type `help`)\n");
    dbg_repl(&c);
    return 0;
}

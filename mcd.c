/*
 * Copyright (c) 2026 tdrse
 * SPDX-License-Identifier: MIT
 */

// #define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_QUERY 256
#define MAX_STATUS 160

/* refresh flags */
#define RF_TITLE    (1 << 0)
#define RF_INPUT    (1 << 1)
#define RF_STATUS   (1 << 2)
#define RF_LIST     (1 << 3)
#define RF_BUTTONS  (1 << 4)
#define RF_HELP     (1 << 5)

static int refresh_flags = 0;

typedef struct {
    int button;
    int x;
    int y;
    int press;
} MouseEvent;

enum {
    K_NONE,
    K_ESC,
    K_ENTER,
    K_UP,
    K_DOWN,
    K_LEFT,
    K_RIGHT,
    K_TAB,
    K_BACKSPACE,
    K_CHAR,
    K_MOUSE,
    K_CTRL_C,
    K_CTRL_P,
    K_HOME,
    K_END,
    K_PGUP,
    K_PGDN
};

enum {
    MODE_CUSTOM,
    MODE_BROWSE
};

typedef struct {
    int x1, x2;
} Button;

typedef struct {
    char **items;
    int *is_link;
    size_t n, cap;

    int *filt;
    size_t fn, fcap;

    size_t sel;
    size_t scroll;
} List;

static struct termios orig_termios;
static int raw_enabled = 0;
static int ui_active = 0;
static int subshell_mode = 0;
static int physical_mode = 0;

static int mode = MODE_CUSTOM;

static List custom_list;
static List browse_list;

static char cwd_path[PATH_MAX] = "";
static char browse_dir[PATH_MAX] = "";
static char query[MAX_QUERY] = "";

static int done = 0;
static int cancelled = 1;
static char result[PATH_MAX] = "";
static char status_msg[MAX_STATUS] = "";

static int rows = 24;
static int cols = 80;

static int list_top = 4;
static int list_height = 10;
static int btn_y = 1;

static Button b_custom, b_browse, b_parent, b_here, b_ok, b_quit;

static int path_click_y = 0;
static int path_click_x1 = 0;
static int path_click_x2 = -1;

static struct timespec last_click = {0, 0};
static size_t last_click_index = (size_t)-1;
static int last_click_mode = -1;

static int too_narrow = 0;

static volatile sig_atomic_t got_winch = 0;

static void winch_handler(int sig)
{
    (void)sig;
    got_winch = 1;
}

static volatile sig_atomic_t g_need_clear_all = 0;

// debug function
/*
#include <stdarg.h>
void debug_print(const char *format, ...) {
    static FILE *pts = NULL;
    if (pts == NULL) {
        pts = fopen("/dev/pts/1", "w");
        if (pts == NULL) return;
        setvbuf(pts, NULL, _IONBF, 0);
    }

    va_list args;
    va_start(args, format);
    vfprintf(pts, format, args);
    va_end(args);
}

// debug macro
#define log_info(fmt, ...)  debug_print("\033[1;32m[INFO]\033[0m " fmt "\n", ##__VA_ARGS__)
#define log_warn(fmt, ...)  debug_print("\033[1;33m[WARN]\033[0m " fmt "\n", ##__VA_ARGS__)
#define log_error(fmt, ...) debug_print("\033[1;31m[ERRO]\033[0m " fmt "\n", ##__VA_ARGS__)

static inline long long time_ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#define PRINT_DURATION_NS(label, start_ns, end_ns) \
    debug_print("%s: %lld ns (%.3f ms)\n", label, (end_ns - start_ns), (end_ns - start_ns) / 1000000.0)
*/

static void set_status(const char *s)
{
    if (strcmp(status_msg, s) != 0){
        snprintf(status_msg, sizeof(status_msg), "%s", s ? s : "");
        refresh_flags |= RF_STATUS;
    }
}

static void set_status_errno(const char *action)
{
    char buf[MAX_STATUS];

    snprintf(buf, sizeof(buf), "%s: %s", action, strerror(errno));
    set_status(buf);
}

static void leave_ui(void)
{
    if (ui_active) {
        fputs("\x1b[?1006l\x1b[?1000l\x1b[?7h\x1b[?25h\x1b[?1049l\x1b[6n", stderr);
        fflush(stderr);
        ui_active = 0;

        struct timeval timeout;
        fd_set fds;
        char sync_buf;

        while (1) {
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);

            timeout.tv_sec = 0;
            timeout.tv_usec = 50000;

            int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &timeout);

            if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
                if (read(STDIN_FILENO, &sync_buf, 1) > 0) {
                    if (sync_buf == 'R') {
                        break;
                    }
                }
            } else {
                break;
            }
        }
    }

    if (raw_enabled) {
        tcflush(STDIN_FILENO, TCIFLUSH);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_enabled = 0;
    }
}

static void sig_handler(int sig)
{
    leave_ui();
    _exit(128 + sig);
}

static int ci_contains(const char *hay, const char *needle)
{
    if (!needle || !*needle) return 1;

    size_t nl = strlen(needle);

    for (const char *p = hay; *p; p++) {
        size_t i = 0;

        while (i < nl &&
               p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }

        if (i == nl) return 1;
    }

    return 0;
}

static void list_clear(List *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->items[i]);
    }

    l->n = 0;
    l->fn = 0;
    l->sel = 0;
    l->scroll = 0;
}

static void list_add(List *l, const char *s)
{
    if (!s || !*s) return;

    if (l->n == l->cap) {
        size_t newcap = l->cap ? l->cap * 2 : 32;
        char **tmp = realloc(l->items, newcap * sizeof(*tmp));
        if (!tmp) return;

        l->items = tmp;
        l->cap = newcap;
    }

    l->items[l->n] = strdup(s);
    if (!l->items[l->n]) return;

    l->n++;
}

static void filt_add(List *l, size_t idx)
{
    if (l->fn == l->fcap) {
        size_t newcap = l->fcap ? l->fcap * 2 : 32;
        int *tmp = realloc(l->filt, newcap * sizeof(*tmp));
        if (!tmp) return;

        l->filt = tmp;
        l->fcap = newcap;
    }

    l->filt[l->fn++] = (int)idx;
}

static void path_join(char *out, size_t outsz, const char *base, const char *name)
{
    if (name[0] == '/') {
        snprintf(out, outsz, "%s", name);
    } else if (!base || !*base || strcmp(base, "/") == 0) {
        snprintf(out, outsz, "/%s", name);
    } else {
        snprintf(out, outsz, "%s/%s", base, name);
    }
}

static void strip_trailing_slashes(char *s)
{
    if (!s) return;

    size_t len = strlen(s);

    while (len > 1 && s[len - 1] == '/') {
        s[--len] = '\0';
    }
}

static void path_clean_slashes(char *path) {
    if (!path || !*path) return;

    int src = 0;
    int dst = 0;

    while (path[src] != '\0') {
        path[dst] = path[src];

        if (path[src] == '/') {
            while (path[src] == '/') {
                src++;
            }
        } else {
            src++;
        }
        dst++;
    }
    path[dst] = '\0';
}

static void list_update_link(List *l)
{
    if (l->n == 0) {
        if (l->is_link) { free(l->is_link); l->is_link = NULL; }
        return;
    }

    int *tmp = realloc(l->is_link, l->n * sizeof(int));
    if (!tmp) return;
    l->is_link = tmp;

    for (size_t i = 0; i < l->n; i++) {
        const char *name = l->items[i];

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            l->is_link[i] = 0;
            continue;
        }

        char full[PATH_MAX];
        if (l == &browse_list) {
            path_join(full, sizeof(full), browse_dir, name);
        } else {
            path_join(full, sizeof(full), cwd_path, name);
        }

        struct stat st;
        l->is_link[i] = (lstat(full, &st) == 0 && S_ISLNK(st.st_mode));
    }
}

static void rebuild_custom(void)
{
    custom_list.fn = 0;

    for (size_t i = 0; i < custom_list.n; i++) {
        if (ci_contains(custom_list.items[i], query)) {
            filt_add(&custom_list, i);
        }
    }

    custom_list.sel = 0;
    custom_list.scroll = 0;

    list_update_link(&custom_list);
}

static void rebuild_browse(void)
{
    browse_list.fn = 0;

    int dot = -1;
    int dotdot = -1;

    for (size_t i = 0; i < browse_list.n; i++) {
        if (strcmp(browse_list.items[i], ".") == 0) {
            dot = (int)i;
        } else if (strcmp(browse_list.items[i], "..") == 0) {
            dotdot = (int)i;
        }
    }

    if (dot >= 0) {
        filt_add(&browse_list, (size_t)dot);
    }

    if (dotdot >= 0) {
        filt_add(&browse_list, (size_t)dotdot);
    }

    for (size_t i = 0; i < browse_list.n; i++) {
        if ((int)i == dot || (int)i == dotdot) continue;

        if (ci_contains(browse_list.items[i], query)) {
            filt_add(&browse_list, i);
        }
    }

    browse_list.sel = 0;
    browse_list.scroll = 0;

    list_update_link(&browse_list);
}

static void rebuild_all(void)
{
    rebuild_custom();
    rebuild_browse();
}

static void query_clean(void)
{
    query[0] = '\0';
    refresh_flags |= RF_INPUT;
    rebuild_all();
}

static void normalize_path(char *out) {
    if (!out || *out == '\0') return;

    char *r = out;
    char *w = out;

    while (*r != '\0') {
        if (*r == '/') {
            *w++ = *r++;
            while (*r == '/') r++;
            continue;
        }

        if (*r == '.' && (r == out || *(r - 1) == '/')) {
            if (*(r + 1) == '/' || *(r + 1) == '\0') {
                r += 1;
                if (*r == '/') r++;
                continue;
            }
            if (*(r + 1) == '.' && (*(r + 2) == '/' || *(r + 2) == '\0')) {
                r += 2;
                if (*r == '/') r++;

                if (w > out + 1) {
                    w--;
                    while (w > out && *(w - 1) != '/') w--;
                }
                continue;
            }
        }
        *w++ = *r++;
    }

    if (w > out + 1 && *(w - 1) == '/') w--;
    if (w == out) *w++ = '/';
    *w = '\0';
}

static void add_custom_path(const char *s)
{
    if (!s || !*s) return;

    char buf[PATH_MAX];

    if (s[0] == '~' && (s[1] == '/' || s[1] == '\0')) {
        const char *home = getenv("HOME");

        if (home) {
            snprintf(buf, sizeof(buf), "%s%s", home, s + 1);
        } else {
            snprintf(buf, sizeof(buf), "%s", s);
        }
    } else {
        snprintf(buf, sizeof(buf), "%s", s);
    }

    path_clean_slashes(buf);
    strip_trailing_slashes(buf);

    if (buf[0] == '\0') return;
    // if (!*buf) return;

    for (size_t i = 0; i < custom_list.n; i++) {
        if (strcmp(custom_list.items[i], buf) == 0) {
            return;
        }
    }

    list_add(&custom_list, buf);
}

static void load_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[PATH_MAX];

    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        size_t l = strlen(p);
        while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\t')) {
            p[--l] = '\0';
        }

        if (*p == '\0' || *p == '#') continue;

        add_custom_path(p);
    }

    fclose(fp);
}

static int path_is_dir(const char *path)
{
    struct stat st;

    if (!path || !*path) return 0;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int canonicalize_path(const char *in, char *out, size_t outsz)
{
    if (!in || !*in || !out || outsz == 0) {
        set_status("Empty path");
        return 0;
    }

    if (physical_mode) {
        if (!realpath(in, out)) {
            set_status_errno("realpath");
            return 0;
        }

        struct stat st;

        if (stat(out, &st) != 0) {
            set_status_errno("stat");
            return 0;
        }

        if (!S_ISDIR(st.st_mode)) {
            set_status("Not a directory");
            return 0;
        }

        return 1;
    }

    int n = snprintf(out, outsz, "%s", in);

    if (n < 0 || (size_t)n >= outsz) {
        set_status("Path too long");
        return 0;
    }

    strip_trailing_slashes(out);
    normalize_path(out);

    if (!out[0]) {
        set_status("Empty path");
        return 0;
    }

    struct stat st;

    if (stat(out, &st) != 0) {
        set_status_errno("stat");
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        set_status("Not a directory");
        return 0;
    }

    return 1;
}

static void get_initial_cwd(char *out, size_t outsz)
{
    if (!out || outsz == 0) return;

    /* 使用 PWD 获取逻辑路径 */
    if (!physical_mode) {
        const char *pwd = getenv("PWD");

        if (pwd && *pwd) {
            char tmp[PATH_MAX];

            snprintf(tmp, sizeof(tmp), "%s", pwd);
            strip_trailing_slashes(tmp);

            if (path_is_dir(tmp)) {
                snprintf(out, outsz, "%s", tmp);
                return;
            }
        }
    }

    if (getcwd(out, outsz)) {
        strip_trailing_slashes(out);
        return;
    }

    snprintf(out, outsz, "/");
}

static int path_parent(char *out, size_t outsz, const char *path)
{
    if (!out || outsz == 0) {
        return 0;
    }

    if (!path || !*path) {
        snprintf(out, outsz, ".");
        return 0;
    }

    snprintf(out, outsz, "%s", path);
    strip_trailing_slashes(out);

    if (strcmp(out, "/") == 0) {
        return 0;
    }

    char *slash = strrchr(out, '/');

    if (!slash) {
        snprintf(out, outsz, ".");
        return 1;
    }

    if (slash == out) {
        out[1] = '\0';
    } else {
        *slash = '\0';
    }

    return 1;
}

static void print_clipped(const char *s, int maxw)
{
    if (!s || maxw <= 0) return;

    const unsigned char *p = (const unsigned char *)s;
    int w = 0;

    while (*p && w < maxw) {
        unsigned char c = *p;
        int len = 1;
        int cw = 1;

        if (c < 0x80) {
            len = 1;
            cw = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cw = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cw = 2;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cw = 2;
        } else {
            fputc('?', stderr);
            w++;
            p++;
            continue;
        }

        if (w + cw > maxw) break;

        int ok = 1;

        for (int i = 1; i < len; i++) {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80) {
                ok = 0;
                break;
            }
        }

        if (!ok) {
            fputc('?', stderr);
            w++;
            p++;
            continue;
        }

        for (int i = 0; i < len; i++) {
            fputc(p[i], stderr);
        }

        w += cw;
        p += len;
    }
}

static int utf8_seq_info(const unsigned char *p, int *len, int *width)
{
    unsigned char c = *p;

    if (c < 0x80) {
        *len = 1;
        *width = 1;
        return 1;
    }

    if ((c & 0xE0) == 0xC0) {
        *len = 2;
        *width = 2;
    } else if ((c & 0xF0) == 0xE0) {
        *len = 3;
        *width = 2;
    } else if ((c & 0xF8) == 0xF0) {
        *len = 4;
        *width = 2;
    } else {
        *len = 1;
        *width = 1;
        return 0;
    }

    for (int i = 1; i < *len; i++) {
        if (p[i] == 0 || (p[i] & 0xC0) != 0x80) {
            *len = 1;
            *width = 1;
            return 0;
        }
    }

    return 1;
}

static int visual_width_clipped(const char *s, int maxw)
{
    if (!s || maxw <= 0) return 0;

    const unsigned char *p = (const unsigned char *)s;
    int w = 0;

    while (*p && w < maxw) {
        int len = 1;
        int cw = 1;

        if (!utf8_seq_info(p, &len, &cw)) {
            len = 1;
            cw = 1;
        }

        if (w + cw > maxw) break;

        w += cw;
        p += len;
    }

    return w;
}

static void compose_child_path(char *out, size_t outsz, const char *base, const char *comp, size_t comp_len)
{
    if (!out || outsz == 0) return;

    if (outsz == 1) {
        out[0] = '\0';
        return;
    }

    if (!base) base = "";
    if (!comp) comp = "";

    /* 根目录: / + comp */
    if (strcmp(base, "/") == 0) {
        size_t max = outsz - 2;

        if (comp_len > max) {
            comp_len = max;
        }

        out[0] = '/';

        if (comp_len > 0) {
            memcpy(out + 1, comp, comp_len);
        }

        out[1 + comp_len] = '\0';
        return;
    }

    /* 普通目录: base + '/' + comp */
    size_t blen = strlen(base);

    if (blen > outsz - 1) {
        memcpy(out, base, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }

    memcpy(out, base, blen);
    size_t pos = blen;

    if (pos + 1 >= outsz) {
        out[pos] = '\0';
        return;
    }

    out[pos++] = '/';

    size_t max = outsz - pos - 1;

    if (comp_len > max) {
        comp_len = max;
    }

    if (comp_len > 0) {
        memcpy(out + pos, comp, comp_len);
        pos += comp_len;
    }

    out[pos] = '\0';
}

static int path_target_at_click(const char *path, int cell, char *out, size_t outsz)
{
    if (!path || !*path || cell < 0 || !out || outsz == 0) {
        return 0;
    }

    const char *p = path;
    int cur = 0;

    /* 根目录 */
    if (*p == '/') {
        if (cell == 0) {
            out[0] = '/';
            if (outsz > 1) {
                out[1] = '\0';
            }
            return 1;
        }

        cur = 1;
        p++;
    }

    char target[PATH_MAX];

    if (path[0] == '/') {
        target[0] = '/';
    } else {
        target[0] = '.';
    }

    target[1] = '\0';

    while (*p) {
        const char *comp = p;
        int comp_cells = 0;

        while (*p && *p != '/') {
            const unsigned char *up = (const unsigned char *)p;
            int len = 1;
            int cw = 1;

            if (!utf8_seq_info(up, &len, &cw)) {
                len = 1;
                cw = 1;
            }

            p += len;
            comp_cells += cw;
        }

        size_t comp_len = (size_t)(p - comp);

        int seg_cells = comp_cells;
        int has_slash = (*p == '/');

        if (has_slash) {
            seg_cells += 1;
        }

        if (cell >= cur && cell < cur + seg_cells) {
            compose_child_path(out, outsz, target, comp, comp_len);
            return 1;
        }

        cur += seg_cells;

        char next[PATH_MAX];
        compose_child_path(next, sizeof(next), target, comp, comp_len);

        size_t n = strlen(next);

        if (n >= sizeof(target)) {
            n = sizeof(target) - 1;
        }

        memcpy(target, next, n);
        target[n] = '\0';

        if (has_slash) {
            p++;
        }
    }

    /* 点击到路径显示范围之外时，回退为完整路径 */
    size_t n = strlen(path);

    if (n >= outsz) {
        n = outsz - 1;
    }

    memcpy(out, path, n);
    out[n] = '\0';

    return 1;
}

static void get_size(void)
{
    struct winsize ws;

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_row > 0 &&
        ws.ws_col > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    } else {
        rows = 24;
        cols = 80;
    }
}

static List *active_list(void)
{
    return (mode == MODE_CUSTOM) ? &custom_list : &browse_list;
}

static void ensure_visible(List *l)
{
    if (l->fn == 0) {
        l->sel = 0;
        l->scroll = 0;
        return;
    }

    if (l->sel >= l->fn) {
        l->sel = l->fn - 1;
    }

    if (l->scroll > l->fn) {
        l->scroll = 0;
    }

    if (l->sel < l->scroll) {
        l->scroll = l->sel;
    }

    if (l->sel >= l->scroll + (size_t)list_height) {
        l->scroll = l->sel - list_height + 1;
    }

    if (l->scroll + (size_t)list_height > l->fn) {
        l->scroll = (l->fn > (size_t)list_height)
                        ? l->fn - list_height
                        : 0;
    }

    set_status("");
}

static int wheel_scroll(int down)
{
    List *l = active_list();

    if (l->fn == 0) return 0;

    size_t old = l->sel;
    size_t step = 3;

    if (down) {
        if (l->sel + step < l->fn) {
            l->sel += step;
        } else {
            l->sel = l->fn - 1;
        }
    } else {
        if (l->sel > step) {
            l->sel -= step;
        } else {
            l->sel = 0;
        }
    }

    if (l->sel == old) {
        return 0;
    }

    ensure_visible(l);
    refresh_flags |= RF_LIST;
    return 1;
}

static void confirm_path(const char *path)
{
    char resolved[PATH_MAX];

    char full[PATH_MAX];
    path_join(full, sizeof(full), cwd_path, path);

    if (!canonicalize_path(full, resolved, sizeof(resolved))) {
        return;
    }

    snprintf(result, sizeof(result), "%s", resolved);
    cancelled = 0;
    done = 1;
}

static void confirm_custom_selected(void)
{
    if (custom_list.fn == 0 || custom_list.sel >= custom_list.fn) {
        set_status("No custom path selected");
        return;
    }

    confirm_path(custom_list.items[custom_list.filt[custom_list.sel]]);
}

static void confirm_browse_current(void)
{
    confirm_path(browse_dir);
}

static const char *browse_selected_name(void)
{
    if (browse_list.fn == 0 || browse_list.sel >= browse_list.fn) {
        return NULL;
    }

    return browse_list.items[browse_list.filt[browse_list.sel]];
}

static int browse_selected_path(char *out, size_t outsz)
{
    const char *name = browse_selected_name();

    if (!name) return 0;

    if (strcmp(name, ".") == 0) {
        snprintf(out, outsz, "%s", browse_dir);
        return 1;
    }

    if (strcmp(name, "..") == 0) {
        if (!path_parent(out, outsz, browse_dir)) {
            snprintf(out, outsz, "%s", browse_dir);
        }
        return 1;
    }

    path_join(out, outsz, browse_dir, name);
    return 1;
}

static void confirm_browse_selected(void)
{
    if (browse_list.fn == 0) {
        confirm_browse_current();
        return;
    }

    char full[PATH_MAX];

    if (!browse_selected_path(full, sizeof(full))) {
        confirm_browse_current();
        return;
    }

    confirm_path(full);
}

static int cmp_str(const void *a, const void *b)
{
    const char *sa = *(char *const *)a;
    const char *sb = *(char *const *)b;

    return strcmp(sa, sb);
}

static void browse_load(const char *path)
{
    char resolved[PATH_MAX];

    if (!canonicalize_path(path, resolved, sizeof(resolved))) {
        return;
    }

    refresh_flags |= RF_TITLE | RF_STATUS | RF_LIST;

    char old_dir[PATH_MAX];
    snprintf(old_dir, sizeof(old_dir), "%s", browse_dir);

    list_clear(&browse_list);
    snprintf(browse_dir, sizeof(browse_dir), "%s", resolved);

    list_add(&browse_list, ".");

    if (strcmp(resolved, "/") != 0) {
        list_add(&browse_list, "..");
    }

    DIR *d = opendir(resolved);

    if (!d) {
        set_status_errno("opendir");
        rebuild_browse();
        return;
    }

    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;

        if (e->d_type == DT_DIR) {
            list_add(&browse_list, e->d_name);
        } else if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            char full[PATH_MAX];
            path_join(full, sizeof(full), resolved, e->d_name);
            if (path_is_dir(full)) {
                list_add(&browse_list, e->d_name);
            }
        }
    }

    closedir(d);

    if (browse_list.n > 0) {
        qsort(browse_list.items, browse_list.n, sizeof(char *), cmp_str);
    }

    rebuild_browse();

    if (old_dir[0] && browse_list.fn > 0) {
        const char *rel = NULL;
        size_t res_len = strlen(resolved);

        if (strcmp(resolved, "/") == 0) {
            if (old_dir[0] == '/' && old_dir[1] != '\0') {
                rel = old_dir + 1;
            }
        } else if (strlen(old_dir) > res_len &&
                   strncmp(old_dir, resolved, res_len) == 0 &&
                   old_dir[res_len] == '/') {
            rel = old_dir + res_len + 1;
        }

        if (rel && *rel) {
            const char *slash = strchr(rel, '/');
            size_t len = slash ? (size_t)(slash - rel) : strlen(rel);

            for (size_t i = 0; i < browse_list.fn; i++) {
                const char *item = browse_list.items[browse_list.filt[i]];

                if (strlen(item) == len && strncmp(item, rel, len) == 0) {
                    browse_list.sel = i;
                    ensure_visible(&browse_list);
                    break;
                }
            }
        }
    }

    set_status("");
}

static void browse_go_parent(void)
{
    if (!browse_dir[0]) {
        set_status("No browse directory");
        return;
    }

    if (strcmp(browse_dir, "/") == 0) {
        set_status("Already root");
        return;
    }

    char parent[PATH_MAX];
    path_parent(parent, sizeof(parent), browse_dir);
    browse_load(parent);
}

static void browse_double_action(void)
{
    const char *name = browse_selected_name();

    if (!name) return;

    if (strcmp(name, ".") == 0) {
        confirm_browse_current();
        return;
    }

    char full[PATH_MAX];

    if (!browse_selected_path(full, sizeof(full))) {
        set_status("Cannot get selected path");
        return;
    }

    browse_load(full);
}

static void browse_enter_selected(void)
{
    const char *name = browse_selected_name();

    if (!name) {
        set_status("No directory to enter");
        return;
    }

    if (strcmp(name, ".") == 0) {
        set_status("Current directory");
        return;
    }

    char full[PATH_MAX];

    if (!browse_selected_path(full, sizeof(full))) {
        set_status("Cannot get selected path");
        return;
    }

    /* include .. */
    query_clean();
    browse_load(full);
}

static void browse_custom_selected(void)
{
    if (custom_list.fn == 0 || custom_list.sel >= custom_list.fn) {
        set_status("No custom path selected");
        return;
    }

    const char *path = custom_list.items[custom_list.filt[custom_list.sel]];
    char resolved[PATH_MAX];

    char full[PATH_MAX];
    path_join(full, sizeof(full), cwd_path, path);

    if (!canonicalize_path(full, resolved, sizeof(resolved))) {
        return;
    }

    mode = MODE_BROWSE;
    refresh_flags |= RF_BUTTONS;
    query_clean();
    browse_load(resolved);
}

static int enable_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0) {
        return -1;
    }

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return -1;
    }

    raw_enabled = 1;
    return 0;
}

static int read_byte_timeout(unsigned char *c, int timeout_ms)
{
    for (;;) {
        if (got_winch) return 0;

        fd_set set;
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int r = select(STDIN_FILENO + 1, &set, NULL, NULL, &tv);

        if (r == -1) {
            if (errno == EINTR) {
                if (got_winch) return 0;
                continue;
            }
            return -1;
        }

        if (r == 0) return 0;

        ssize_t n;

        do {
            n = read(STDIN_FILENO, c, 1);
        } while (n == -1 && errno == EINTR && !got_winch);

        if (got_winch) return 0;
        if (n <= 0) return -1;

        return 1;
    }
}

static void flush_stdin(void)
{
    unsigned char buf[256];
    fd_set set;
    struct timeval tv = {0, 0};

    while (1) {
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);
        if (select(STDIN_FILENO + 1, &set, NULL, NULL, &tv) <= 0) {
            break;
        }
        if (read(STDIN_FILENO, buf, sizeof(buf)) <= 0) {
            break;
        }
    }
}

static int read_key(unsigned char *ch_out, MouseEvent *m)
{
    if (ch_out) *ch_out = 0;
    if (m) memset(m, 0, sizeof(*m));

    unsigned char c;
    int r = read_byte_timeout(&c, 100);

    if (r < 0) return K_CTRL_C;
    if (r == 0) return K_NONE;

    if (c == 0x1b) {
        unsigned char c2;
        r = read_byte_timeout(&c2, 30);
        if (r <= 0) return K_ESC;

        if (c2 == '[' || c2 == 'O') {
            unsigned char c3;
            r = read_byte_timeout(&c3, 30);
            if (r <= 0) return K_ESC;

            if (c2 == '[' && c3 == '<') {
                int vals[3] = {0, 0, 0};
                int idx = 0;
                int num = 0;
                unsigned char ch;

                while (1) {
                    r = read_byte_timeout(&ch, 50);
                    if (r <= 0) return K_NONE;

                    if (ch >= '0' && ch <= '9') {
                        num = num * 10 + (ch - '0');
                    } else if (ch == ';') {
                        if (idx < 3) vals[idx++] = num;
                        num = 0;
                    } else if (ch == 'M' || ch == 'm') {
                        if (idx < 3) vals[idx++] = num;

                        m->button = vals[0];
                        m->x = vals[1];
                        m->y = vals[2];
                        m->press = (ch == 'M');

                        return K_MOUSE;
                    } else {
                        return K_NONE;
                    }
                }
            }

            if (c2 == '[') {
                if (c3 == 'A') return K_UP;
                if (c3 == 'B') return K_DOWN;
                if (c3 == 'C') return K_RIGHT;
                if (c3 == 'D') return K_LEFT;
                if (c3 == 'H') return K_HOME;
                if (c3 == 'F') return K_END;

                if (c3 >= '0' && c3 <= '9') {
                    int num = c3 - '0';
                    unsigned char ch;
                    int in_modifier = 0;
                    while (1) {
                        r = read_byte_timeout(&ch, 30);
                        if (r <= 0) return K_NONE;

                        if (ch >= '0' && ch <= '9') {
                            if (!in_modifier) {
                                num = num * 10 + (ch - '0');
                            }
                        } else if (ch == ';') {
                            in_modifier = 1;
                        } else if (ch == '~') {
                            if (in_modifier) return K_NONE;
                            switch (num) {
                            case 1: case 7: return K_HOME;
                            case 4: case 8: return K_END;
                            case 5:         return K_PGUP;
                            case 6:         return K_PGDN;
                            default:        return K_NONE;
                            }
                        } else if (ch >= '@' && ch <= '~') {
                            return K_NONE;
                        } else {
                            return K_NONE;
                        }
                    }
                }

                unsigned char ch_junk = c3;
                while (!(ch_junk >= '@' && ch_junk <= '~')) {
                    r = read_byte_timeout(&ch_junk, 20);
                    if (r <= 0) break;
                }
                return K_NONE;
            }

            if (c2 == 'O') {
                if (c3 == 'H') return K_HOME;   /* \x1bOH */
                if (c3 == 'F') return K_END;    /* \x1bOF */
                return K_NONE;
            }

            return K_NONE;
        }

        return K_ESC;
    }

    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 127 || c == 8) return K_BACKSPACE;
    if (c == 9) return K_TAB;
    if (c == 3) return K_CTRL_C;
    if (c == 16) return K_CTRL_P;

    if (c >= 32) {
        *ch_out = c;
        return K_CHAR;
    }

    return K_NONE;
}

static int in_button(const Button *b, int x, int y)
{
    return y == btn_y && x >= b->x1 && x <= b->x2;
}

static int handle_mouse(const MouseEvent *m)
{
    if (!m->press) return 0;

    /* wheel up/down */
    if (m->button == 64 || m->button == 68) {
        return wheel_scroll(0);
    }

    if (m->button == 65 || m->button == 69) {
        return wheel_scroll(1);
    }

    /* right click quit */
    if (m->button == 2) {
        done = 1;
        cancelled = 1;
        return 0;
    }

    if (m->button != 0) return 0;

    /* click path line to jump */
    if (path_click_x2 >= path_click_x1 &&
        m->y == path_click_y &&
        m->x >= path_click_x1 &&
        m->x <= path_click_x2) {

        int cell = m->x - path_click_x1;
        char target[PATH_MAX];

        if (path_target_at_click(browse_dir, cell, target, sizeof(target))) {
            if (strcmp(target, browse_dir) != 0) {
                browse_load(target);
                return 1;
            }
        }

        refresh_flags |= RF_TITLE | RF_STATUS | RF_LIST;
        return 0;
    }

    if (in_button(&b_custom, m->x, m->y)) {
        if (mode != MODE_CUSTOM) {
            mode = MODE_CUSTOM;
            refresh_flags |= RF_TITLE | RF_STATUS | RF_LIST | RF_BUTTONS;
        }
        set_status("");
        return 1;
    }

    if (in_button(&b_browse, m->x, m->y)) {
        if (mode != MODE_BROWSE) {
            mode = MODE_BROWSE;
            refresh_flags |= RF_TITLE | RF_STATUS | RF_LIST | RF_BUTTONS;
        }
        set_status("");
        return 1;
    }

    if (in_button(&b_parent, m->x, m->y)) {
        mode = MODE_BROWSE;
        refresh_flags |= RF_BUTTONS;
        browse_go_parent();
        return 1;
    }

    if (in_button(&b_here, m->x, m->y)) {
        confirm_browse_current();
        return 1;
    }

    if (in_button(&b_ok, m->x, m->y)) {
        if (mode == MODE_CUSTOM) {
            confirm_custom_selected();
        } else {
            confirm_browse_selected();
        }
        return 1;
    }

    if (in_button(&b_quit, m->x, m->y)) {
        done = 1;
        cancelled = 1;
        return 0;
    }

    if (m->y >= list_top && m->y < list_top + list_height) {
        List *l = active_list();

        size_t idx = l->scroll + (size_t)(m->y - list_top);

        if (idx >= l->fn) return 0;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        double dt =
            (now.tv_sec - last_click.tv_sec) * 1000.0 +
            (now.tv_nsec - last_click.tv_nsec) / 1000000.0;

        refresh_flags |= RF_LIST;

        if (idx == l->sel &&
            idx == last_click_index &&
            last_click_mode == mode &&
            dt < 400.0) {

            if (mode == MODE_CUSTOM) {
                confirm_custom_selected();
            } else {
                browse_double_action();
            }

            last_click_index = (size_t)-1;
            last_click_mode = -1;
            return 1;
        } else {
            int changed = (l->sel != idx);

            l->sel = idx;
            last_click = now;
            last_click_index = idx;
            last_click_mode = mode;

            return changed;
        }
    }

    return 0;
}

static void query_backspace(void)
{
    size_t len = strlen(query);
    if (len == 0) return;

    len--;

    while (len > 0 && (query[len] & 0xC0) == 0x80) {
        len--;
    }

    query[len] = '\0';

    set_status("");
    rebuild_all();
}

static int draw_button(int y, int x, const char *label, int active)
{
    fprintf(stderr, "\x1b[%d;%dH%s%s\x1b[0m",
            y, x,
            active ? "\x1b[1;7m" : "\x1b[7m",
            label);

    return x + (int)strlen(label) - 1;
}

static void draw_title(void)
{
    const char *title = (mode == MODE_CUSTOM) ? "mcd - custom" : "mcd - browse";
    fprintf(stderr, "\x1b[1;1H\x1b[1m");
    print_clipped(title, cols - 1);
    fputs("\x1b[0m", stderr);
    fprintf(stderr, "\x1b[K");
}

static void draw_input(void)
{
    fprintf(stderr, "\x1b[2;1H> ");
    print_clipped(query, cols - 3);
    fprintf(stderr, "\x1b[K");
}

static void draw_status(void)
{
    List *l = active_list();
    char info[PATH_MAX + 128];

    static size_t last_fn = 0;
    static size_t last_custom_n = 0;
    static size_t last_browse_n = 0;
    static int last_mode = -1;
    static int last_cols = 0;

    static char last_status_msg[sizeof(status_msg)] = {0};
    static char last_browse_dir[PATH_MAX] = {0};

    int state_changed = g_need_clear_all || (l->fn != last_fn) ||
                        (custom_list.n != last_custom_n) ||
                        (browse_list.n != last_browse_n) ||
                        (mode != last_mode) ||
                        (cols != last_cols) ||
                        (status_msg[0] != last_status_msg[0]) ||
                        (strcmp(status_msg, last_status_msg) != 0) ||
                        (strcmp(browse_dir, last_browse_dir) != 0);

    if (!state_changed) {
        return;
    }

    last_fn = l->fn;
    last_custom_n = custom_list.n;
    last_browse_n = browse_list.n;
    last_mode = mode;
    last_cols = cols;

    snprintf(last_status_msg, sizeof(last_status_msg), "%s", status_msg);
    snprintf(last_browse_dir, sizeof(last_browse_dir), "%s", browse_dir);

    if (status_msg[0]) {
        snprintf(info, sizeof(info), "Status: %s", status_msg);
        fprintf(stderr, "\x1b[3;1H");
        print_clipped(info, cols - 1);
        fprintf(stderr, "\x1b[K");
    } else if (mode == MODE_CUSTOM) {
        snprintf(info, sizeof(info), "Custom %zu/%zu | Tab switch view",
                 l->fn, custom_list.n);
        fprintf(stderr, "\x1b[3;1H");
        print_clipped(info, cols - 1);
        fprintf(stderr, "\x1b[K");
    } else {
        char prefix[64];
        int plen = snprintf(prefix, sizeof(prefix), "Browse %zu/%zu | ",
                            l->fn, browse_list.n);
        if (plen < 0) plen = 0;

        fprintf(stderr, "\x1b[3;1H%s", prefix);

        int max_path_w = cols - plen - 1;

        if (max_path_w > 0) {
            path_click_y = 3;
            path_click_x1 = plen + 1;
            int dispw = visual_width_clipped(browse_dir, max_path_w);
            path_click_x2 = path_click_x1 + dispw - 1;
            print_clipped(browse_dir, max_path_w);
        }

        fprintf(stderr, "\x1b[K");
    }
}

static void draw_list(void)
{
    List *l = active_list();

    static size_t last_scroll = 0;
    static size_t last_sel = 0;
    static size_t last_fn = 0;
    static int last_cols = 0;
    static int last_rows = 0;
    static int last_sel_item_idx = -1;
    static List *last_active_list_ptr = NULL;
    static char last_browse_dir[PATH_MAX] = {0};

    int current_sel_idx = (l->fn > 0 && l->sel < l->fn) ? l->filt[l->sel] : -1;

    int state_changed = g_need_clear_all ||
                    (l != last_active_list_ptr) ||
                    (l->scroll != last_scroll) ||
                    (l->sel != last_sel) ||
                    (l->fn != last_fn) ||
                    (cols != last_cols) ||
                    (rows != last_rows) ||
                    (current_sel_idx != last_sel_item_idx) ||
                    (strcmp(browse_dir, last_browse_dir) != 0);

    if (!state_changed) {
        return;
    }

    int full_redraw = g_need_clear_all ||
                    (l != last_active_list_ptr) ||
                    (l->scroll != last_scroll) ||
                    (l->fn != last_fn) ||
                    (cols != last_cols) ||
                    (rows != last_rows) ||
                    (strcmp(browse_dir, last_browse_dir) != 0);

    if (full_redraw) {
        size_t end = l->scroll + (size_t)list_height;
        if (end > l->fn) end = l->fn;

        int drawn = 0;
        for (size_t i = l->scroll; i < end; i++, drawn++) {
            int y = list_top + drawn;
            int selected = (i == l->sel);

            int is_link = (l->is_link != NULL) ? l->is_link[l->filt[i]] : 0;

            fprintf(stderr, "\x1b[%d;1H", y);
            if (selected) fputs("\x1b[7m", stderr);
            fputc(selected ? '>' : ' ', stderr);

            if (is_link) {
                fputc('^', stderr);
            } else {
                fputc(' ', stderr);
            }

            print_clipped(l->items[l->filt[i]], cols - 3);
            if (selected) fputs("\x1b[0m", stderr);
            fprintf(stderr, "\x1b[K");
        }

        int bottom = rows - 3;
        for (int y = list_top + drawn; y <= bottom; y++) {
            fprintf(stderr, "\x1b[%d;1H\x1b[2K", y);
        }
    } else {
        size_t old_idx = last_sel;
        if (old_idx < l->fn && old_idx >= l->scroll && old_idx < l->scroll + (size_t)list_height) {
            int y = list_top + (int)(old_idx - l->scroll);

            int is_link = (l->is_link != NULL) ? l->is_link[l->filt[old_idx]] : 0;

            fprintf(stderr, "\x1b[%d;1H ", y);

            if (is_link) {
                fputc('^', stderr);
            } else {
                fputc(' ', stderr);
            }

            print_clipped(l->items[l->filt[old_idx]], cols - 3);
            fprintf(stderr, "\x1b[K");
        }

        size_t new_idx = l->sel;
        if (new_idx < l->fn && new_idx >= l->scroll && new_idx < l->scroll + (size_t)list_height) {
            int y = list_top + (int)(new_idx - l->scroll);

            int is_link = (l->is_link != NULL) ? l->is_link[l->filt[new_idx]] : 0;

            fprintf(stderr, "\x1b[%d;1H\x1b[7m>", y);

            if (is_link) {
                fputc('^', stderr);
            } else {
                fputc(' ', stderr);
            }

            print_clipped(l->items[l->filt[new_idx]], cols - 3);
            fprintf(stderr, "\x1b[0m\x1b[K");
        }
    }

    last_active_list_ptr = l;
    last_scroll = l->scroll;
    last_sel = l->sel;
    last_fn = l->fn;
    last_cols = cols;
    last_rows = rows;
    last_sel_item_idx = current_sel_idx;
    snprintf(last_browse_dir, sizeof(last_browse_dir), "%s", browse_dir);
}

static void draw_buttons(void)
{
    btn_y = rows - 2;
    fprintf(stderr, "\x1b[%d;1H", btn_y);

    int x = 2;

    b_custom.x1 = x;
    b_custom.x2 = draw_button(btn_y, x, "[C]", mode == MODE_CUSTOM);
    x = b_custom.x2 + 2;

    b_browse.x1 = x;
    b_browse.x2 = draw_button(btn_y, x, "[B]", mode == MODE_BROWSE);
    x = b_browse.x2 + 2;

    b_parent.x1 = x;
    b_parent.x2 = draw_button(btn_y, x, "[..]", 0);
    x = b_parent.x2 + 2;

    b_here.x1 = x;
    b_here.x2 = draw_button(btn_y, x, "[.]", 0);
    x = b_here.x2 + 2;

    b_ok.x1 = x;
    b_ok.x2 = draw_button(btn_y, x, "[OK]", 0);
    x = b_ok.x2 + 2;

    b_quit.x1 = x;
    b_quit.x2 = draw_button(btn_y, x, "[X]", 0);

    fprintf(stderr, "\x1b[K");
}

static void draw_help(void)
{
    const char *help =
        "Tab view | Left parent | Right enter | Enter choose | Ctrl+P mode | Esc quit";
    fprintf(stderr, "\x1b[%d;1H", rows);
    print_clipped(help, cols - 1);
    fprintf(stderr, "\x1b[K");
}

static void draw_cursor(void)
{
    int query_display_width = visual_width_clipped(query, cols - 3);
    int cursor_x = 3 + query_display_width;
    fprintf(stderr, "\x1b[2;%dH", cursor_x);
}

static void apply_refresh(void)
{
    if (refresh_flags == 0) {
        draw_cursor();
        fflush(stderr);
        return;
    }

    if (refresh_flags & RF_TITLE) draw_title();
    if (refresh_flags & RF_INPUT) draw_input();
    if (refresh_flags & RF_STATUS) draw_status();

    if (refresh_flags & RF_LIST) draw_list();

    if (refresh_flags & RF_BUTTONS) draw_buttons();
    if (refresh_flags & RF_HELP) draw_help();

    draw_cursor();
    fflush(stderr);

    g_need_clear_all = 0;

    refresh_flags = 0;
}

static void draw_full(void)
{
    static int last_rows = -1;
    static int last_cols = -1;

    get_size();

    path_click_y = 0;
    path_click_x1 = 0;
    path_click_x2 = -1;

    if (rows != last_rows || cols != last_cols) {
        fputs("\x1b[H\x1b[2J", stderr);
        g_need_clear_all = 1;
        last_rows = rows;
        last_cols = cols;
    } else {
        fputs("\x1b[H", stderr);
    }

    if (rows < 8 || cols < 50) {
        fprintf(stderr, "\x1b[1;1H\x1b[2KTerminal too small.\r\n");
        too_narrow = 1;
        return;
    } else if (too_narrow) {
        too_narrow = 0;
    }

    list_top = 4;
    int bottom = rows - 3;
    list_height = bottom - list_top + 1;
    if (list_height < 1) list_height = 1;

    List *l = active_list();
    ensure_visible(l);

    draw_title();
    draw_input();
    draw_status();
    draw_list();
    draw_buttons();
    draw_help();
    draw_cursor();

    fflush(stderr);
}

static void init_custom(int have_arg_dirs)
{
    if (have_arg_dirs) return;

    add_custom_path(cwd_path);

    int had_extra = 0;

    const char *env_file = getenv("MCD_FILE");

    if (env_file && *env_file) {
        size_t before = custom_list.n;
        load_file(env_file);
        if (custom_list.n > before) had_extra = 1;
    } else {
        const char *home = getenv("HOME");

        if (home && *home) {
            char file[PATH_MAX];
            snprintf(file, sizeof(file), "%s/.mcd_dirs", home);

            size_t before = custom_list.n;
            load_file(file);
            if (custom_list.n > before) had_extra = 1;
        }
    }

    const char *env_paths = getenv("MCD_PATHS");

    if (env_paths && *env_paths) {
        char *copy = strdup(env_paths);

        if (copy) {
            char *save = NULL;

            for (char *tok = strtok_r(copy, ":", &save);
                 tok;
                 tok = strtok_r(NULL, ":", &save)) {
                add_custom_path(tok);
            }

            free(copy);
        }

        had_extra = 1;
    }

    if (!had_extra) {
        add_custom_path(getenv("HOME"));
        add_custom_path("/");
        add_custom_path("/tmp");
        add_custom_path("/usr");
        add_custom_path("/etc");
    }
}

static void usage(const char *prog)
{
    printf("mcd: mouse-driven TUI quick cd tool v3.6.1\n\n");
    printf("Usage:\n");
    printf("  %s                 use ~/.mcd_dirs / MCD_FILE / MCD_PATHS / defaults\n", prog);
    printf("  %s [dirs...]       use only given custom dirs\n", prog);
    printf("  %s -s [dirs...]    choose then start subshell\n", prog);
    printf("  %s -P [dirs...]    physical mode: resolve symlinks\n", prog);
    printf("  %s -h              print help\n", prog);
    printf("\n");
    printf("Keys:\n");
    printf("  Tab     switch custom/browse view\n");
    printf("  Left    parent dir in browse view\n");
    printf("  Right   enter selected dir / browse selected custom dir\n");
    printf("  Ctrl+P  change physical / logical mode\n");
    printf("  Enter   choose path\n");
    printf("  Esc     quit\n");
}

int main(int argc, char **argv)
{
    int have_arg_dirs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-s") == 0) {
            subshell_mode = 1;
        } else if (strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--physical") == 0) {
            physical_mode = 1;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            while (i < argc) {
                add_custom_path(argv[i++]);
                have_arg_dirs = 1;
            }
            break;
        } else {
            add_custom_path(argv[i]);
            have_arg_dirs = 1;
        }
    }

    get_initial_cwd(cwd_path, sizeof(cwd_path));

    init_custom(have_arg_dirs);
    browse_load(cwd_path);

    rebuild_custom();

    if (custom_list.n == 0) {
        mode = MODE_BROWSE;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) {
        fprintf(stderr, "mcd: need a TTY on stdin/stderr\n");
        return 1;
    }

    if (enable_raw() != 0) {
        fprintf(stderr, "mcd: cannot set raw mode\n");
        return 1;
    }

    fputs("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1006h", stderr);
    fflush(stderr);

    ui_active = 1;

    atexit(leave_ui);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = winch_handler;
    sigaction(SIGWINCH, &sa, NULL);

    char old_status[MAX_STATUS];
    struct timespec status_time_count;
    clock_gettime(CLOCK_MONOTONIC, &status_time_count);

    draw_full();

    while (!done) {
        if (got_winch) {
            got_winch = 0;
            // flush_stdin();
            draw_full();
            continue;
        }

        unsigned char ch = 0;
        MouseEvent m;

        memset(&m, 0, sizeof(m));

        int k = read_key(&ch, &m);

        if (got_winch) {
            got_winch = 0;
            flush_stdin();
            draw_full();
            continue;
        }

        if (too_narrow && k != K_ESC && k != K_CTRL_C) continue;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        double elapsed = (now.tv_sec - status_time_count.tv_sec) +
                         (now.tv_nsec - status_time_count.tv_nsec) * 1e-9;

        if (status_msg[0] != '\0' && elapsed >= 2.0) {
            set_status("");
            apply_refresh();
        }

        if (k == K_NONE) continue;
        if (k == K_MOUSE && !m.press) continue;

        snprintf(old_status, sizeof(old_status), "%s", status_msg);

        refresh_flags = 0;

        switch (k) {
        case K_MOUSE: {
            handle_mouse(&m);

            if (status_msg[0]) {
                refresh_flags |= RF_STATUS;
            }
            break;
        }

        case K_ENTER: {
            if (mode == MODE_CUSTOM) {
                confirm_custom_selected();
            } else {
                confirm_browse_selected();
            }
            if (!done && status_msg[0]) {
                refresh_flags |= RF_STATUS;
            }
            break;
        }

        case K_ESC:
        case K_CTRL_C:
            done = 1;
            cancelled = 1;
            break;

        case K_UP: {
            List *l = active_list();
            if (l->sel > 0) {
                l->sel--;
                ensure_visible(l);
                refresh_flags |= RF_LIST;
            }
            break;
        }

        case K_DOWN: {
            List *l = active_list();
            if (l->fn > 0 && l->sel + 1 < l->fn) {
                l->sel++;
                ensure_visible(l);
                refresh_flags |= RF_LIST;
            }
            break;
        }

        case K_LEFT: {
            if (mode == MODE_BROWSE) {
                browse_go_parent();
            } else {
                mode = MODE_BROWSE;
                query_clean();
                set_status("");
                refresh_flags |= RF_TITLE | RF_STATUS | RF_LIST | RF_BUTTONS;
            }
            break;
        }

        case K_RIGHT: {
            if (mode == MODE_CUSTOM) {
                browse_custom_selected();
            } else {
                browse_enter_selected();
            }
            break;
        }

        case K_TAB: {
            mode = (mode == MODE_CUSTOM) ? MODE_BROWSE : MODE_CUSTOM;
            set_status("");
            refresh_flags |= RF_TITLE | RF_STATUS | RF_LIST | RF_BUTTONS;
            break;
        }

        case K_CTRL_P: {
            physical_mode = !physical_mode;

            if (physical_mode) {
                char real[PATH_MAX];
                if (realpath(browse_dir, real)) {
                    snprintf(browse_dir, sizeof(browse_dir), "%s", real);
                }
                set_status("Mode: physical (-P)");
            } else {
                strip_trailing_slashes(browse_dir);
                set_status("Mode: logical");
            }
            break;
        }

        case K_HOME: {
            List *l = active_list();
            if (l->fn > 0 && l->sel > 0) {
                l->sel = 0;
                ensure_visible(l);
                refresh_flags |= RF_LIST;
            }
            break;
        }

        case K_END: {
            List *l = active_list();
            if (l->fn > 0 && l->sel + 1 < l->fn) {
                l->sel = l->fn - 1;
                ensure_visible(l);
                refresh_flags |= RF_LIST;
            }
            break;
        }

        case K_PGUP: {
            List *l = active_list();
            if (l->fn > 0 && l->sel > 0 && list_height > 0) {
                size_t step = (size_t)list_height;
                l->sel = (l->sel >= step) ? l->sel - step : 0;
                ensure_visible(l);
                refresh_flags |= RF_LIST;
            }
            break;
        }

        case K_PGDN: {
            List *l = active_list();
            if (l->fn > 0 && l->sel + 1 < l->fn && list_height > 0) {
                size_t step = (size_t)list_height;
                size_t max_idx = l->fn - 1;
                l->sel = (l->sel + step < max_idx) ? l->sel + step : max_idx;
                ensure_visible(l);
                refresh_flags |= RF_LIST;
            }
            break;
        }

        case K_BACKSPACE: {
            size_t len = strlen(query);
            if (len > 0) {
                query_backspace();
                refresh_flags |= RF_INPUT | RF_STATUS | RF_LIST;
            }
            break;
        }

        case K_CHAR: {
            size_t len = strlen(query);
            if (len < MAX_QUERY - 1) {
                query[len] = (char)ch;
                query[len + 1] = '\0';
                set_status("");
                rebuild_all();
                refresh_flags |= RF_INPUT | RF_STATUS | RF_LIST;
            }
            break;
        }

        default:
            break;
        }

        if (strcmp(status_msg, old_status) != 0) {
            clock_gettime(CLOCK_MONOTONIC, &status_time_count);
            refresh_flags |= RF_STATUS;
        }

        if (!done) {
            apply_refresh();
        }
    }

    leave_ui();

    if (cancelled) {
        return 1;
    }

    if (subshell_mode) {
        if (chdir(result) != 0) {
            fprintf(stderr, "chdir: %s: %s\n", result, strerror(errno));
            return 1;
        }

        const char *sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/sh";

        execlp(sh, sh, (char *)NULL);
        fprintf(stderr, "exec: %s: %s\n", sh, strerror(errno));
        return 1;
    }

    printf("%s\n", result);
    return 0;
}

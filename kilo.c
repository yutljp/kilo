#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#define KILO_VERSION "0.0.3"
#define TAB_SIZE 4
#define QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)
#define CTRL2CHAR(k) ((k) | 0x40)


struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}


enum editor_key {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};


struct editor_row {
    int size;
    int rsize;
    char *chars;
    char *render;
};


struct editor_config {
    int cx, cy;
    int rx;
    int row_off;
    int col_off;
    int screen_rows;
    int screen_cols;
    int num_rows;
    struct editor_row *row;
    int dirty;
    char *filename;
    char status_msg[80];
    time_t status_msg_time;
    struct termios orig_termios;
};

struct editor_config E;


void ab_append(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) {
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void ab_free(struct abuf *ab) {
    free(ab->b);
}

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disable_raw_mode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;

        return 0;
    }
}

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.row_off = 0;
    E.col_off = 0;
    E.num_rows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.status_msg[0] = '\0';
    E.status_msg_time = 0;
    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
        die("get_window_size");
    }
    E.screen_rows -= 2;
}

void editor_update_row(struct editor_row *row) {
    int tabs = 0;
    int ctrls = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
        /* control chars */
        if (CTRL_KEY('@') <= row->chars[j] && row->chars[j] <= CTRL_KEY('_')) {
            ctrls++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + (tabs*TAB_SIZE + 1) + ctrls);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_SIZE != 0) {
                row->render[idx++] = ' ';
            }
        } else if (CTRL_KEY('@') <= row->chars[j] && row->chars[j] <= CTRL_KEY('?')) {
            row->render[idx++] = '^';
            row->render[idx++] = CTRL2CHAR(row->chars[j]);
        } else {
                row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editor_insert_row(int at, char *s, size_t len) {
    if (at < 0 || at > E.num_rows) {
        return;
    }

    E.row = realloc(E.row, sizeof(struct editor_row) * (E.num_rows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(struct editor_row) * (E.num_rows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editor_update_row(&E.row[at]);

    E.num_rows++;
    E.dirty++;
}

void editor_free_raw(struct editor_row *row) {
    free(row->render);
    free(row->chars);
}

void editor_del_row(int at) {
    if (at < 0 || at >= E.num_rows) return;
    editor_free_raw(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(struct editor_row) * (E.num_rows - at - 1));
    E.num_rows--;
    E.dirty++;
}

void editor_row_insert_char(struct editor_row *row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    E.dirty++;
}

void editor_insert_char(int c) {
    if (E.cy == E.num_rows) {
        editor_insert_row(E.num_rows, "", 0);
    }
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editor_insert_newline() {
    if (E.cx == 0) {
        editor_insert_row(E.cy, "", 0);
    } else {
        struct editor_row *row = &E.row[E.cy];
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    E.cy++;
    E.cx = 0;
}

void editor_row_append_string(struct editor_row *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    E.dirty++;
}

void editor_row_del_char(struct editor_row *row, int at) {
    if (at < 0 || at >= row->size) {
        return;
    }
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

void editor_del_char() {
    if (E.cy == E.num_rows) return;
    if (E.cx == 0 && E.cy == 0) {
        return;
    }
    struct editor_row *row = &E.row[E.cy];
    if (E.cx > 0) {
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editor_row_append_string(&E.row[E.cy - 1], row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}

void editor_draw_rows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screen_rows; y++) {
        int file_row = y + E.row_off;
        if (y >= E.num_rows) {
            if (E.num_rows == 0 && y == E.screen_rows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- version %s", KILO_VERSION);
                if (welcome_len > E.screen_cols) {
                    welcome_len = E.screen_cols;
                }
                int padding = (E.screen_cols - welcome_len) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    ab_append(ab, " ", 1);
                }
                ab_append(ab, welcome, welcome_len);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            int len = E.row[file_row].rsize - E.col_off;
            if (len < 0) {
                len = 0;
            }
            if (len > E.screen_cols) {
                len = E.screen_cols;
            }
            ab_append(ab, &E.row[file_row].render[E.col_off], len);
        }

        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 4);
    char status[80];
    char rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.num_rows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cy + 1, E.num_rows);
    if (len > E.screen_cols) {
        len = E.screen_cols;
    }
    ab_append(ab, status, len);
    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        } else {
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[K", 3);
    int msglen = strlen(E.status_msg);
    if (msglen > E.screen_cols) msglen = E.screen_cols;
    if (msglen && time(NULL) - E.status_msg_time < 5) {
        ab_append(ab, E.status_msg, msglen);
    }
}

void editor_set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
    va_end(ap);
    E.status_msg_time = time(NULL);
}

int editor_row_cx2rx(struct editor_row *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (TAB_SIZE - 1) - (rx % TAB_SIZE);
        } else if (CTRL_KEY('@') <= row->chars[j] && row->chars[j] <= CTRL_KEY('?')) {
            rx ++;
        }
        rx++;
    }

    return rx;
}

int editor_row_rx2cx(struct editor_row *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (TAB_SIZE - 1) - (cur_rx % TAB_SIZE);
        } else if (CTRL_KEY('@') <= row->chars[cx] && row->chars[cx] <= CTRL_KEY('?')) {
            cur_rx++;
        }
        cur_rx++;

        if (cur_rx > rx) {
            return cx;
        }
    }

    return cx;
}

void editor_scroll() {
    E.rx = 0;
    if (E.cy < E.num_rows) {
        E.rx = editor_row_cx2rx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.row_off) {
        E.row_off = E.cy;
    }
    if (E.cy >= E.row_off + E.screen_rows) {
        E.row_off = E.cy - E.screen_rows + 1;
    }
    if (E.rx < E.col_off) {
        E.col_off = E.rx;
    }
    if (E.rx >= E.col_off + E.screen_cols) {
        E.col_off = E.rx - E.screen_cols + 1;
    }
}

void editor_refresh_screen() {
    editor_scroll();

    struct abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_off) + 1,
                                              (E.rx - E.col_off) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

int editor_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
    size_t buf_size = 128;
    char *buf = malloc(buf_size);

    size_t buf_len = 0;
    buf[0] = '\0';

    while (1) {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buf_len != 0) {
                buf[--buf_len] = '\0';
            }
        } else if (c == '\x1b') {
            editor_set_status_message("");
            if (callback) {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buf_len != 0) {
                editor_set_status_message("");
                if (callback) {
                    callback(buf, c);
                }
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buf_len == buf_size - 1) {
                buf_size *= 2;
                buf = realloc(buf, buf_size);
            }
            buf[buf_len++] = c;
            buf[buf_len] = '\0';
        }

        if (callback) {
            callback(buf, c);
        }
    }
}

void editor_move_cursor(int key) {
    struct editor_row *row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.num_rows) {
                E.cy++;
            }
            break;
    }
    row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
    int row_len = row ? row->size : 0;
    if (E.cx > row_len) {
        E.cx = row_len;
    }
}

char *editor_rows2string(int *buf_len) {
    int len = 0;
    int j;
    for (j = 0; j < E.num_rows; j++) {
        len += E.row[j].size + 1;
    }
    *buf_len = len;

    char *buf = malloc(len);
    char *p = buf;
    for (j = 0; j < E.num_rows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_open(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' ||
                                line[line_len - 1] == '\r')) {
            line_len--;
        }
        editor_insert_row(E.num_rows, line, line_len);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save() {
    if (E.filename == NULL) {
        E.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editor_set_status_message("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editor_rows2string(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editor_set_status_message("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

void editor_find_callback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) {
        direction = 1;
    }
    int current = last_match;

    int i;
    for (i = 0; i < E.num_rows; i++) {
        current += direction;
        if (current == -1) {
            current = E.num_rows - 1;
        } else if (current == E.num_rows) {
            current = 0;
        }

        struct editor_row *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editor_row_rx2cx(row, match - row->render);
            E.row_off = E.num_rows;
            break;
        }
    }
}

void editor_find() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_col_off = E.col_off;
    int saved_row_off = E.row_off;

    char *query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);

    if (query == NULL) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.col_off = saved_col_off;
        E.row_off = saved_row_off;
    }
}

void editor_process_key_press() {
    static int quit_times = QUIT_TIMES;
    int c = editor_read_key();

    switch (c) {
        case '\r':
            editor_insert_newline();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editor_set_status_message("WARNING!!! File has unsaved changes. "
                                          "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editor_save();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if (E.cy < E.num_rows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case CTRL_KEY('f'):
            editor_find();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editor_move_cursor(ARROW_RIGHT);
            editor_del_char();
            break;

        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP) {
                E.cy = E.row_off;
            } else if (c == PAGE_DOWN) {
                E.cy = E.row_off + E.screen_rows - 1;
                if (E.cy > E.num_rows) {
                    E.cy = E.num_rows;
                }
            }
            int times = E.screen_rows;
            while (times--) {
                editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editor_move_cursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editor_insert_char(c);
            break;
    }

    quit_times = QUIT_TIMES;
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) {
        editor_refresh_screen();
        editor_process_key_press();
    }

    return 0;
}
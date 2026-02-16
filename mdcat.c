/*
 * mdcat.c — minimal Markdown renderer for the terminal
 *
 * Renders a subset of Markdown using ANSI escape codes:
 *   - Headings (#, ##, ###)
 *   - Bold (**text** or __text__)
 *   - Italic (*text* or _text_)
 *   - Bold+italic (***text***)
 *   - Inline code (`code`)
 *   - Fenced code blocks (``` ... ```)
 *   - Bullet lists (-, *, +)
 *   - Numbered lists (1. 2. ...)
 *   - Horizontal rules (--- / ***)
 *   - Block quotes (> text)
 *   - Tables (GFM pipe syntax, with alignment)
 *
 * Usage: mdcat [file]   (reads stdin if no file given)
 *
 * ANSI codes are suppressed automatically when stdout is not a TTY.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>   /* isatty() */

/* ── ANSI escape sequences ───────────────────────────────────────────────── */

#define A_RESET     "\033[0m"
#define A_BOLD      "\033[1m"
#define A_DIM       "\033[2m"
#define A_ITALIC    "\033[3m"
#define A_UNDER     "\033[4m"

/* Foreground colours */
#define A_FG_RED    "\033[31m"
#define A_FG_GREEN  "\033[32m"
#define A_FG_YELLOW "\033[33m"
#define A_FG_BLUE   "\033[34m"
#define A_FG_MAGENTA "\033[35m"
#define A_FG_CYAN   "\033[36m"
#define A_FG_WHITE  "\033[37m"

/* Background for inline code */
#define A_BG_CODE   "\033[48;5;236m"  /* dark grey cell */
#define A_FG_CODE   "\033[38;5;215m"  /* soft orange */

static int g_color = 1;   /* set to 0 when stdout is not a TTY */

/* Emit an ANSI sequence only when color is enabled */
static void ansi(const char *seq)
{
    if (g_color) fputs(seq, stdout);
}

/* ── Inline span renderer ────────────────────────────────────────────────── */
/*
 * State machine that walks a line and handles:
 *   `code`   bold+italic (***), bold (**  or __), italic (* or _)
 *
 * Limitations (by design - single-pass, no backtracking):
 *   - Markers must be balanced on the same line.
 *   - Nesting bold inside italic is not supported.
 */

#define SPAN_NONE   0
#define SPAN_BOLD   1
#define SPAN_ITALIC 2
#define SPAN_BI     3   /* bold + italic */

static void render_inline(const char *s, int len)
{
    int i = 0;
    int state = SPAN_NONE;   /* current active span */
    int in_code = 0;

    while (i < len) {
        unsigned char c = (unsigned char)s[i];

        /* ── backtick: inline code ───────────────────────────────────────── */
        if (c == '`' && !in_code) {
            /* find closing backtick */
            int j = i + 1;
            while (j < len && s[j] != '`') j++;
            if (j < len) {
                ansi(A_BG_CODE);
                ansi(A_FG_CODE);
                putchar(' ');
                for (int k = i + 1; k < j; k++) putchar(s[k]);
                putchar(' ');
                ansi(A_RESET);
                /* restore active span */
                if (state == SPAN_BOLD)        { ansi(A_BOLD); }
                else if (state == SPAN_ITALIC)  { ansi(A_ITALIC); }
                else if (state == SPAN_BI)      { ansi(A_BOLD); ansi(A_ITALIC); }
                i = j + 1;
                continue;
            }
            /* no closing backtick — fall through and print literal */
        }

        /* ── asterisk / underscore: bold / italic ───────────────────────── */
        if ((c == '*' || c == '_') && !in_code) {
            /* count run length (max 3) */
            int run = 0;
            char marker = (char)c;
            while (i + run < len && s[i + run] == marker && run < 3) run++;

            if (run == 3) {
                if (state == SPAN_BI) { ansi(A_RESET); state = SPAN_NONE; }
                else                  { ansi(A_BOLD); ansi(A_ITALIC); state = SPAN_BI; }
                i += 3;
                continue;
            }
            if (run == 2) {
                if (state == SPAN_BOLD) { ansi(A_RESET); state = SPAN_NONE; }
                else                    { ansi(A_BOLD); state = SPAN_BOLD; }
                i += 2;
                continue;
            }
            if (run == 1) {
                if (state == SPAN_ITALIC) { ansi(A_RESET); state = SPAN_NONE; }
                else                      { ansi(A_ITALIC); state = SPAN_ITALIC; }
                i += 1;
                continue;
            }
        }

        /* ── ordinary character ──────────────────────────────────────────── */
        putchar(c);
        i++;
    }

    /* close any unclosed span */
    if (state != SPAN_NONE) ansi(A_RESET);
}

/* ── Line-level renderer ─────────────────────────────────────────────────── */

#define MAX_LINE 4096

static void render_inline(const char *s, int len);   /* forward decl */

/* ── Table support ───────────────────────────────────────────────────────── */

#define MAX_COLS  16
#define MAX_CELL  128

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } Align;

/*
 * Split a pipe-delimited row into cells.
 * Leading/trailing pipes and whitespace around cells are stripped.
 * Returns number of cells found (<= MAX_COLS).
 */
static int split_row(const char *line, char cells[MAX_COLS][MAX_CELL])
{
    int ncols = 0;
    const char *p = line;

    /* skip optional leading pipe */
    if (*p == '|') p++;

    while (*p && ncols < MAX_COLS) {
        /* find next unescaped '|' or end-of-string */
        const char *start = p;
        while (*p && *p != '|') p++;

        /* copy and trim whitespace */
        int clen = (int)(p - start);
        while (clen > 0 && start[0] == ' ')  { start++; clen--; }
        while (clen > 0 && start[clen-1] == ' ') clen--;

        if (clen >= MAX_CELL) clen = MAX_CELL - 1;
        memcpy(cells[ncols], start, (size_t)clen);
        cells[ncols][clen] = '\0';
        ncols++;

        if (*p == '|') p++;   /* consume the pipe */
    }
    return ncols;
}

/*
 * Inspect a separator row like | --- | :---: | ---: |
 * Returns 1 if it looks like a valid separator, 0 otherwise.
 * Fills align[] for each column.
 */
static int parse_sep(const char *line, Align align[], int ncols)
{
    char cells[MAX_COLS][MAX_CELL];
    int n = split_row(line, cells);
    if (n != ncols) return 0;

    for (int i = 0; i < n; i++) {
        const char *c = cells[i];
        int len = (int)strlen(c);
        if (len == 0) return 0;

        /* must be only '-', ':', or space */
        for (int k = 0; k < len; k++)
            if (c[k] != '-' && c[k] != ':') return 0;

        int left  = (c[0] == ':');
        int right = (c[len-1] == ':');

        if (left && right) align[i] = ALIGN_CENTER;
        else if (right)    align[i] = ALIGN_RIGHT;
        else               align[i] = ALIGN_LEFT;
    }
    return 1;
}

/*
 * Count the number of visible terminal columns that render_inline() would
 * occupy for the given string.
 *
 * Two sources of invisible/miscounted bytes:
 *   1. Markdown markers (* _ ` and combinations) - not printed at all.
 *   2. Multi-byte UTF-8 sequences - count as one character, not N bytes.
 *
 * We do not attempt full Unicode width (CJK wide chars etc.) - that would
 * require wcwidth() which drags in locale machinery.  ASCII + Latin + the
 * Unicode box/symbol characters found in typical Markdown docs are all
 * single-column, so counting codepoints is sufficient here.
 */
static int visible_len(const char *s, int len)
{
    int vis = 0;
    int i   = 0;

    while (i < len) {
        unsigned char c = (unsigned char)s[i];

        /* backtick span: skip markers, count content codepoints + 2 spaces */
        if (c == '`') {
            int j = i + 1;
            while (j < len && s[j] != '`') j++;
            if (j < len) {
                vis += 2;   /* padding spaces around inline code */
                /* count codepoints between the backticks */
                for (int k = i + 1; k < j; ) {
                    unsigned char b = (unsigned char)s[k];
                    if      (b < 0x80) k += 1;
                    else if (b < 0xE0) k += 2;
                    else if (b < 0xF0) k += 3;
                    else               k += 4;
                    vis++;
                }
                i = j + 1;
                continue;
            }
            /* no closing backtick — literal char */
            vis++; i++;
            continue;
        }

        /* bold/italic markers: skip the run of * or _ (ASCII, 1 byte each) */
        if (c == '*' || c == '_') {
            int run = 0;
            char mk = (char)c;
            while (i + run < len && s[i + run] == mk && run < 3) run++;
            i += run;
            continue;
        }

        /* multi-byte UTF-8: advance past the whole codepoint, count once */
        if      (c < 0x80) i += 1;
        else if (c < 0xE0) i += 2;
        else if (c < 0xF0) i += 3;
        else               i += 4;
        vis++;
    }
    return vis;
}

/* Print exactly `w` visible characters of `text`, padding with spaces. */
static void print_cell(const char *text, int w, Align align)
{
    int vlen = visible_len(text, (int)strlen(text));
    int pad  = w - vlen;
    if (pad < 0) pad = 0;

    int lpad = 0, rpad = pad;
    if (align == ALIGN_CENTER) { lpad = pad / 2; rpad = pad - lpad; }
    else if (align == ALIGN_RIGHT) { lpad = pad; rpad = 0; }

    for (int i = 0; i < lpad; i++) putchar(' ');
    render_inline(text, (int)strlen(text));
    for (int i = 0; i < rpad; i++) putchar(' ');
}

/* Horizontal rule for table borders using box-drawing chars */
static void table_hline(const int widths[], int ncols)
{
    ansi(A_DIM);
    /* left corner or T-junction */
    fputs("\xe2\x94\x9c", stdout);   /* ├ */
    for (int c = 0; c < ncols; c++) {
        for (int i = 0; i < widths[c] + 2; i++)
            fputs("\xe2\x94\x80", stdout);   /* ─ */
        if (c < ncols - 1) fputs("\xe2\x94\xbc", stdout);  /* ┼ */
        else               fputs("\xe2\x94\xa4", stdout);  /* ┤ */
    }
    ansi(A_RESET);
    putchar('\n');
}

static void table_topline(const int widths[], int ncols)
{
    ansi(A_DIM);
    fputs("\xe2\x94\x8c", stdout);   /* ┌ */
    for (int c = 0; c < ncols; c++) {
        for (int i = 0; i < widths[c] + 2; i++)
            fputs("\xe2\x94\x80", stdout);
        if (c < ncols - 1) fputs("\xe2\x94\xac", stdout);  /* ┬ */
        else               fputs("\xe2\x94\x90", stdout);  /* ┐ */
    }
    ansi(A_RESET);
    putchar('\n');
}

static void table_botline(const int widths[], int ncols)
{
    ansi(A_DIM);
    fputs("\xe2\x94\x94", stdout);   /* └ */
    for (int c = 0; c < ncols; c++) {
        for (int i = 0; i < widths[c] + 2; i++)
            fputs("\xe2\x94\x80", stdout);
        if (c < ncols - 1) fputs("\xe2\x94\xb4", stdout);  /* ┴ */
        else               fputs("\xe2\x94\x98", stdout);  /* ┘ */
    }
    ansi(A_RESET);
    putchar('\n');
}

static void render_row(char cells[MAX_COLS][MAX_CELL],
                       int ncols, const int widths[], const Align align[],
                       int is_header)
{
    ansi(A_DIM); fputs("\xe2\x94\x82", stdout); ansi(A_RESET);  /* │ */
    for (int c = 0; c < ncols; c++) {
        putchar(' ');
        if (is_header) { ansi(A_BOLD); ansi(A_FG_CYAN); }
        print_cell(cells[c], widths[c], align[c]);
        if (is_header) ansi(A_RESET);
        putchar(' ');
        ansi(A_DIM); fputs("\xe2\x94\x82", stdout); ansi(A_RESET);  /* │ */
    }
    putchar('\n');
}

/*
 * Render a complete table.
 * header_cells / sep_line are already read; body rows are read from fp.
 * Stops when a non-pipe line (or EOF) is encountered; that line is written
 * into `leftover` (MAX_LINE bytes) for the caller to process.
 */
static void render_table(FILE *fp,
                         char header_cells[MAX_COLS][MAX_CELL],
                         int ncols,
                         const Align align[],
                         char leftover[MAX_LINE])
{
    /* Collect all body rows first so we can compute column widths. */
#define MAX_ROWS 256
    char body[MAX_ROWS][MAX_COLS][MAX_CELL];
    int  nrows = 0;

    leftover[0] = '\0';

    while (nrows < MAX_ROWS && fgets(leftover, MAX_LINE, fp)) {
        int ll = (int)strlen(leftover);
        if (ll > 0 && leftover[ll-1] == '\n') leftover[--ll] = '\0';

        if (leftover[0] != '|') break;   /* end of table */

        split_row(leftover, body[nrows]);
        nrows++;
        leftover[0] = '\0';
    }

    /* Compute column widths based on visible (rendered) character count */
    int widths[MAX_COLS];
    for (int c = 0; c < ncols; c++) {
        widths[c] = visible_len(header_cells[c], (int)strlen(header_cells[c]));
        if (widths[c] < 3) widths[c] = 3;
        for (int r = 0; r < nrows; r++) {
            int cw = visible_len(body[r][c], (int)strlen(body[r][c]));
            if (cw > widths[c]) widths[c] = cw;
        }
    }

    /* Render */
    table_topline(widths, ncols);
    render_row(header_cells, ncols, widths, align, 1);
    table_hline(widths, ncols);
    for (int r = 0; r < nrows; r++)
        render_row(body[r], ncols, widths, align, 0);
    table_botline(widths, ncols);

#undef MAX_ROWS
}

static void render_hr(void)
{
    ansi(A_DIM);
    for (int i = 0; i < 60; i++) putchar('\xe2'), putchar('\x94'), putchar('\x80'); /* UTF-8 ─ */
    ansi(A_RESET);
    putchar('\n');
}

static void render_file(FILE *fp)
{
    char line[MAX_LINE];
    char peek[MAX_LINE];   /* one-line lookahead */
    int  have_peek = 0;
    int  in_fence  = 0;

    /* Helper: get next line into `line`, using peek buffer if available */
#define NEXTLINE() ( have_peek \
        ? (memcpy(line, peek, MAX_LINE), have_peek = 0, 1) \
        : (fgets(line, MAX_LINE, fp) != NULL) )

    while (NEXTLINE()) {
        int len = (int)strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        /* ── fenced code block ──────────────────────────────────────────── */
        if (strncmp(line, "```", 3) == 0) {
            in_fence = !in_fence;
            if (in_fence) {
                ansi(A_DIM);
                if (line[3] != '\0') {
                    ansi(A_FG_GREEN);
                    printf("[%s]\n", line + 3);
                    ansi(A_RESET);
                } else {
                    putchar('\n');
                }
            } else {
                ansi(A_RESET);
                putchar('\n');
            }
            continue;
        }

        if (in_fence) {
            ansi(A_FG_CODE);
            printf("  %s\n", line);
            ansi(A_RESET);
            continue;
        }

        /* ── blank line ─────────────────────────────────────────────────── */
        if (len == 0) { putchar('\n'); continue; }

        /* ── horizontal rule: ---, ***, === (3+ chars, all same) ────────── */
        {
            int hr = 1;
            char fc = line[0];
            if (fc == '-' || fc == '*' || fc == '=') {
                for (int i = 0; i < len; i++)
                    if (line[i] != fc) { hr = 0; break; }
                if (hr && len >= 3) { render_hr(); continue; }
            }
        }

        /* ── table: pipe-prefixed line followed by separator ────────────── */
        if (line[0] == '|') {
            /* peek at next line to check for separator row */
            if (!have_peek && fgets(peek, MAX_LINE, fp)) {
                int pl = (int)strlen(peek);
                if (pl > 0 && peek[pl-1] == '\n') peek[--pl] = '\0';
                have_peek = 1;
            }

            if (have_peek && peek[0] == '|') {
                char header[MAX_COLS][MAX_CELL];
                int  ncols = split_row(line, header);
                Align align[MAX_COLS];

                if (parse_sep(peek, align, ncols)) {
                    /* consume the separator line from lookahead */
                    have_peek = 0;
                    /* render_table reads body rows and fills leftover */
                    char leftover[MAX_LINE];
                    render_table(fp, header, ncols, align, leftover);
                    /* if a non-table line was read, re-queue it */
                    if (leftover[0] != '\0') {
                        memcpy(peek, leftover, MAX_LINE);
                        have_peek = 1;
                    }
                    continue;
                }
                /* not a table — fall through, peek stays queued */
            }
        }

        /* ── headings ───────────────────────────────────────────────────── */
        if (line[0] == '#') {
            int level = 0;
            while (level < len && line[level] == '#') level++;
            if (level <= 6 && line[level] == ' ') {
                const char *text = line + level + 1;
                putchar('\n');
                if (level == 1) {
                    ansi(A_BOLD); ansi(A_FG_CYAN); ansi(A_UNDER);
                    render_inline(text, (int)strlen(text));
                    ansi(A_RESET); putchar('\n');
                    ansi(A_FG_CYAN); ansi(A_DIM);
                    int tlen = (int)strlen(text);
                    for (int i = 0; i < tlen + 2; i++)
                        putchar('\xe2'), putchar('\x95'), putchar('\x90');
                    ansi(A_RESET); putchar('\n');
                } else if (level == 2) {
                    ansi(A_BOLD); ansi(A_FG_YELLOW);
                    render_inline(text, (int)strlen(text));
                    ansi(A_RESET); putchar('\n');
                } else {
                    ansi(A_BOLD); ansi(A_FG_MAGENTA);
                    render_inline(text, (int)strlen(text));
                    ansi(A_RESET); putchar('\n');
                }
                continue;
            }
        }

        /* ── block quote ────────────────────────────────────────────────── */
        if (line[0] == '>' && (line[1] == ' ' || line[1] == '\0')) {
            const char *text = (line[1] == ' ') ? line + 2 : "";
            ansi(A_FG_GREEN); ansi(A_DIM);
            fputs("\xe2\x94\x82 ", stdout);
            ansi(A_RESET); ansi(A_ITALIC); ansi(A_FG_GREEN);
            render_inline(text, (int)strlen(text));
            ansi(A_RESET); putchar('\n');
            continue;
        }

        /* ── bullet list: -, *, + ───────────────────────────────────────── */
        if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
            const char *text = line + 2;
            fputs("  ", stdout);
            ansi(A_FG_YELLOW); ansi(A_BOLD);
            fputs("\xe2\x80\xa2 ", stdout);
            ansi(A_RESET);
            render_inline(text, (int)strlen(text));
            putchar('\n');
            continue;
        }

        /* ── numbered list: digit(s) followed by ". " ───────────────────── */
        {
            int di = 0;
            while (di < len && isdigit((unsigned char)line[di])) di++;
            if (di > 0 && di < len - 1 && line[di] == '.' && line[di+1] == ' ') {
                const char *text = line + di + 2;
                fputs("  ", stdout);
                ansi(A_FG_YELLOW); ansi(A_BOLD);
                for (int k = 0; k < di; k++) putchar(line[k]);
                fputs(". ", stdout);
                ansi(A_RESET);
                render_inline(text, (int)strlen(text));
                putchar('\n');
                continue;
            }
        }

        /* ── ordinary paragraph line ────────────────────────────────────── */
        render_inline(line, len);
        putchar('\n');
    }

#undef NEXTLINE

    if (in_fence) ansi(A_RESET);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    g_color = isatty(STDOUT_FILENO);

    if (argc == 1) {
        render_file(stdin);
    } else {
        for (int i = 1; i < argc; i++) {
            FILE *fp = fopen(argv[i], "r");
            if (!fp) {
                fprintf(stderr, "mdcat: cannot open '%s': ", argv[i]);
                perror(NULL);
                return 1;
            }
            render_file(fp);
            fclose(fp);
        }
    }
    return 0;
}

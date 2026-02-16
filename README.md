# mdcat

A minimal Markdown renderer for the terminal, written in C99.
Formats Markdown with ANSI colours and box-drawing characters.
No dependencies beyond a C99 compiler and a POSIX system.

## Build

```bash
make        # produces ./mdcat
make clean
```

Requires gcc (or any C99-compliant compiler).  Edit `CC` in the Makefile to
switch compilers.

## Usage

```bash
mdcat file.md           # render a file
mdcat file1.md file2.md # render multiple files in sequence
cat file.md | mdcat     # read from stdin
```

ANSI colour codes are suppressed automatically when stdout is not a TTY
(i.e. when piping to a file or another program), so `mdcat` is safe to use
in pipelines.

## Supported syntax

| Element              | Syntax                          |
| -------------------- | ------------------------------- |
| Headings             | `#`, `##`, `###`                |
| Bold                 | `**text**` or `__text__`        |
| Italic               | `*text*` or `_text_`            |
| Bold + italic        | `***text***`                    |
| Inline code          | `` `code` ``                    |
| Fenced code block    | ```` ``` ```` … ```` ``` ````   |
| Bullet list          | `-`, `*`, or `+` prefix         |
| Numbered list        | `1.`, `2.`, … prefix            |
| Block quote          | `>` prefix                      |
| Horizontal rule      | `---`, `===`, or `***`          |
| Table (GFM)          | pipe syntax with separator row  |

Table columns support left, right, and centre alignment via the separator
row (`:---`, `---:`, `:---:`).  Inline markup works inside table cells.
Column widths are computed from visible character counts, so bold, italic,
inline code, and multi-byte UTF-8 in cells do not break border alignment.

## Known limitations

- Inline spans (bold/italic) must open and close on the same line.
- Nested spans (bold inside italic) are not supported.
- Indented code blocks (4-space) are not rendered; use fenced blocks.
- CJK double-width characters are counted as single-width (no `wcwidth`).
- Table column count is capped at 16; row count at 256.

#!/usr/bin/env python3
"""Terminal client for the CYD board's Note page (page 6).

Talks to the same localhost endpoint note.html uses -- POST /api/note on
usage_server.py -- so it needs the usage server running, and nothing else. No
browser, no page load. Editing opens $VISUAL/$EDITOR on a temp file, so vim or
nano does the actual editing rather than this script reinventing it.

    python3 note.py                     # open $EDITOR on the current note
    python3 note.py --show              # board-accurate preview, no editing
    python3 note.py "buy milk"          # set the note directly
    python3 note.py --append "- TODO x" # add a line
    pbpaste | python3 note.py -         # set from stdin
    python3 note.py --size 2            # board text size only
    python3 note.py --clear             # empty it

Handy as an alias, which is why this file is chmod +x and named for the noun
rather than the repo's usual verb_noun.py:

    alias note='~/cyd/note.py'
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile
import urllib.request

# ── ENDPOINT ──────────────────────────────────────────────
# Localhost-only by server policy (writes never come from the LAN). urllib
# sends no Origin header, which the server's NOTE_ALLOWED_ORIGINS allowlist
# permits -- that is the curl/CLI path.
HOST = "127.0.0.1:8787"
NOTE_URL = "http://%s/api/note" % HOST
TIMEOUT_SEC = 3

SERVER_DOWN_HELP = (
    "Error: cannot reach the usage server at %s\n"
    "       it may be stopped, or paused on battery (the battery guard closes\n"
    "       the socket below 50%% so macOS can sleep)\n"
    "       try: launchctl kickstart -k gui/$UID/com.corner.cydusage" % HOST
)

# ── NOTE TOKENIZER + WRAP ─────────────────────────────────
# FOURTH copy of the board's syntax rules and wrap walk. The others live in
# firmware/cyd_dashboard/pages.cpp (drawNotePane, the authority),
# simulator.html (drawNotePane) and note.html (noteFits + highlightHtml). All
# four must agree; `--selftest` pins the row boundaries, and the practical
# check is that `--show` wraps a given note identically to the simulator's
# page 6. See CLAUDE.md's Note page notes.
#
# Tokenizing is two-level -- source line, then word -- plus a one-character
# backtick toggle. There is deliberately no per-character classification:
# colouring digits individually renders "3.5" as yellow-white-yellow, which
# reads as broken rather than highlighted.
#
# Precedence, first match wins:
#   1. inside a `backtick span`      -> COL_BLUE (delimiters included)
#   2. word is TODO/FIXME/BUG        -> COL_WARN
#      word is DONE/OK               -> COL_GOOD
#      word is numeric               -> COL_YELLOW
#   3. line begins '#'               -> COL_ACCENT (whole line)
#      line begins '>'               -> COL_TEXT2  (whole line)
#   4. leading "- ", "* ", "+ "      -> COL_ACCENT (the marker char only)
#   5. otherwise                     -> COL_TEXT
#
# Keywords are matched case-SENSITIVE uppercase-only: it keeps "ok" in prose
# quiet, and case folding is itself a parity hazard across four languages.

NOTE_TY = 18       # first glyph row on the board
NOTE_TY_MAX = 212  # exclusive bottom limit: a row is drawn while y + 8*size <= this

# xterm-256 indices, not 24-bit truecolor: only seven fixed colours are needed
# and macOS Terminal.app mishandles 38;2;R;G;B while every terminal handles
# 38;5;N. The comment carries the firmware's RGB565 and its RGB888 twin, the
# same convention the other three copies use.
COL_TEXT = 231    # 0xFFFF rgb(255,255,255)
COL_TEXT2 = 247   # 0x9CD3 rgb(156,154,156)
COL_ACCENT = 209  # 0xFB08 rgb(255,97,66)
COL_GOOD = 77     # 0x2668 rgb(33,206,66)
COL_WARN = 196    # 0xF8C6 rgb(255,24,49)
COL_BLUE = 69     # 0x3C1E rgb(58,130,247)
COL_YELLOW = 226  # 0xFFE0 rgb(255,255,0)

# Rows the pane fits per size, and the column count. 24 divides evenly by all
# three sizes, which is why the board's usable width is 24 glyphs at size 1.
NOTE_ROWS = {1: 19, 2: 10, 3: 7}


def note_punct_open(c):
    return c in "([{\"'"


def note_punct_close(c):
    return c in ")]}:;,.!?\"'"


def note_is_digit(c):
    return "0" <= c <= "9"


def note_is_num_char(c):
    # Characters allowed inside a numeric token alongside the digits, so
    # "12:30", "$1,200", "2026-08-26" and "50%" each colour as one unit.
    return note_is_digit(c) or c in ".,:/%$+-"


def note_word_color(s, a, b):
    """Word-level colour for s[a:b), or None. Wrapping punctuation is trimmed
    for the comparison only -- the colour still applies to the whole untrimmed
    word, so "TODO:" and "(TODO)" both light up including the punctuation."""
    while a < b and note_punct_open(s[a]):
        a += 1
    while b > a and note_punct_close(s[b - 1]):
        b -= 1
    if b == a:
        return None
    w = s[a:b]
    if w in ("TODO", "FIXME", "BUG"):
        return COL_WARN
    if w in ("DONE", "OK"):
        return COL_GOOD
    has_digit = False
    for ch in w:
        if not note_is_num_char(ch):
            return None
        if note_is_digit(ch):
            has_digit = True
    return COL_YELLOW if has_digit else None


def note_line_context(s, i, n):
    """(line_end, line_color, marker_idx) for the source line starting at i."""
    line_end = i
    while line_end < n and s[line_end] != "\n":
        line_end += 1
    p = i
    while p < line_end and s[p] == " ":
        p += 1  # allow indented markers
    line_color = None
    marker_idx = -1
    if p < line_end:
        if s[p] == "#":
            line_color = COL_ACCENT
        elif s[p] == ">":
            line_color = COL_TEXT2
        elif s[p] in "-*+" and (p + 1 == line_end or s[p + 1] == " "):
            marker_idx = p
    return line_end, line_color, marker_idx


def note_wrap(s, size):
    """Walk the note exactly as the board's drawNotePane does.

    Returns (cells, clipped) where cells is a list of (row, col, char, colour)
    for every glyph the board would draw, and clipped says whether text ran
    past the bottom of the pane. Word-wrapped, with a hard break for words
    longer than one row; overflow is simply not emitted -- the wrap + clip
    contract.

    One deliberate difference from drawNotePane: the row advance at
    end-of-line is skipped once the text is exhausted. The firmware reaches
    the same point and falls out of its loop, so no glyph differs -- but here
    the advance would report `clipped` for a note that fills the pane exactly.
    (note.html's noteFits carries the same guard, for the same reason.)
    """
    glyph_h = 8 * size
    line_h = glyph_h + 2
    cols = 24 // size
    n = len(s)
    y = NOTE_TY
    col = 0
    i = 0
    cells = []

    while i < n:
        line_end, line_color, marker_idx = note_line_context(s, i, n)
        in_code = False  # an unmatched backtick stains only its own line
        col = 0

        while i < line_end:
            if s[i] == " ":
                # Leading spaces on a wrapped row collapse; a source line's own
                # indentation collapses too, which on a 24-column pane is worth
                # more than preserving it. Indented bullets still work -- the
                # `p` scan in note_line_context skips spaces before classifying.
                if col > 0:
                    col += 1
                i += 1
                if col >= cols:
                    y += line_h
                    col = 0
                    if y + glyph_h > NOTE_TY_MAX:
                        return cells, True
                continue

            w_end = i
            while w_end < line_end and s[w_end] != " ":
                w_end += 1
            w_len = w_end - i

            # Wrap before a word that doesn't fit here but would fit on a fresh
            # row. A word longer than a whole row is hard-broken below instead.
            if col > 0 and col + w_len > cols and w_len <= cols:
                y += line_h
                col = 0
                if y + glyph_h > NOTE_TY_MAX:
                    return cells, True
                continue  # re-test on the new row

            word_color = note_word_color(s, i, w_end)

            while i < w_end:
                if col >= cols:  # hard break inside an over-long word
                    y += line_h
                    col = 0
                    if y + glyph_h > NOTE_TY_MAX:
                        return cells, True
                if s[i] == "`":
                    in_code = not in_code
                    c = COL_BLUE  # colour the tick itself, so a stray one shows
                elif in_code:
                    c = COL_BLUE
                elif word_color:
                    c = word_color
                elif line_color:
                    c = line_color
                elif i == marker_idx:
                    c = COL_ACCENT
                else:
                    c = COL_TEXT
                cells.append(((y - NOTE_TY) // line_h, col, s[i], c))
                col += 1
                i += 1

        i = line_end + 1
        if i >= n:
            break  # see the docstring: no content left to clip
        y += line_h
        col = 0
        if y + glyph_h > NOTE_TY_MAX:
            return cells, True

    return cells, False


def note_fits(s, size):
    """Convenience wrapper used by --selftest."""
    return not note_wrap(s, size)[1]


# ── PREVIEW ───────────────────────────────────────────────

def paint(text, color, use_color):
    if not use_color or color is None:
        return text
    return "\033[38;5;%dm%s\033[0m" % (color, text)


def render_preview(text, size, use_color):
    """The note as the board will draw it: real geometry, real colours, and an
    explicit marker where the pane cuts the text off."""
    cols = 24 // size
    rows = NOTE_ROWS[size]
    cells, clipped = note_wrap(text, size)

    grid = [[(" ", None) for _ in range(cols)] for _ in range(rows)]
    for row, col, ch, color in cells:
        if 0 <= row < rows and 0 <= col < cols:
            grid[row][col] = (ch, color)

    out = []
    inner = cols + 2
    out.append("┌" + "─" * inner + "┐")
    out.append("│ " + paint("NOTE".ljust(cols), COL_TEXT2, use_color) + " │")
    for row in grid:
        # Coalesce runs of one colour so a row is a handful of escapes, not one
        # per character.
        line = ""
        run = ""
        run_color = None
        for ch, color in row:
            if color != run_color:
                line += paint(run, run_color, use_color)
                run = ""
                run_color = color
            run += ch
        line += paint(run, run_color, use_color)
        out.append("│ " + line + " │")
    if clipped:
        label = "─ clipped "
        out.append("└" + paint(label, COL_WARN, use_color) +
                   "─" * (inner - len(label)) + "┘")
    else:
        out.append("└" + "─" * inner + "┘")
    return "\n".join(out)


# ── HTTP ──────────────────────────────────────────────────

def get_note():
    """(ok, message, note_dict)."""
    try:
        with urllib.request.urlopen(NOTE_URL, timeout=TIMEOUT_SEC) as resp:
            return True, "", json.loads(resp.read())
    except Exception as exc:
        return False, str(exc), None


def post_note(text, size):
    """(ok, message, note_dict) -- note_dict is what the server actually
    stored, after its own sanitize_note(). Previewing the returned text rather
    than the sent text is why this script needs no copy of that sanitizer."""
    req = urllib.request.Request(
        NOTE_URL,
        data=json.dumps({"text": text, "size": size}).encode("utf-8"),
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_SEC) as resp:
            doc = json.loads(resp.read())
    except Exception as exc:
        return False, str(exc), None
    if doc.get("ok"):
        return True, "", doc.get("note")
    return False, doc.get("detail") or "server rejected the note", None


# ── EDITOR ────────────────────────────────────────────────

def edit_in_editor(initial):
    """Open $VISUAL/$EDITOR on the text, return what came back (or None if the
    editor failed). The .md suffix gets vim/nano/VS Code to highlight
    headings, bullets and backtick spans -- roughly the board's own rules."""
    editor = os.environ.get("VISUAL") or os.environ.get("EDITOR") or "vi"
    fd, path = tempfile.mkstemp(prefix="cyd-note-", suffix=".md")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(initial)
        try:
            # shlex.split so EDITOR="code -w" works. Inherits the tty, which a
            # terminal editor needs.
            rc = subprocess.call(shlex.split(editor) + [path])
        except OSError as exc:
            print("Error: could not launch editor %r: %s" % (editor, exc))
            return None
        if rc != 0:
            print("Error: editor %r exited %d -- note unchanged" % (editor, rc))
            return None
        with open(path, "r") as f:
            return f.read()
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


# ── SELFTEST ──────────────────────────────────────────────

def selftest():
    """Pin the wrap boundaries. These are the numbers that caught the JS
    version's off-by-ones, so they are worth asserting cheaply on every run."""
    checks = []

    def rows_of(count, width):
        return "\n".join(["x" * width] * count)

    for size, rows, cols in ((1, 19, 24), (2, 10, 12), (3, 7, 8)):
        checks.append(("size %d: %d rows fit" % (size, rows),
                       note_fits(rows_of(rows, cols), size), True))
        checks.append(("size %d: %d rows clipped" % (size, rows + 1),
                       note_fits(rows_of(rows + 1, cols), size), False))
        # Hard-break path: one unbroken word filling the pane exactly.
        checks.append(("size %d: %d-char word fits" % (size, rows * cols),
                       note_fits("y" * (rows * cols), size), True))
        checks.append(("size %d: %d-char word clipped" % (size, rows * cols + 1),
                       note_fits("y" * (rows * cols + 1), size), False))

    # Tokenizer spot checks: the rules most likely to rot.
    def color_of(text, idx):
        for row, col, ch, color in note_wrap(text, 1)[0]:
            if idx == 0:
                return color
            idx -= 1
        return None

    checks.append(("TODO is warn", color_of("TODO x", 0), COL_WARN))
    checks.append(("TODO: keeps colon", color_of("TODO: x", 4), COL_WARN))
    checks.append(("lowercase todo is plain", color_of("todo x", 0), COL_TEXT))
    checks.append(("TODOS is not a keyword", color_of("TODOS x", 0), COL_TEXT))
    checks.append(("numeric token", color_of("3.5 x", 1), COL_YELLOW))
    checks.append(("v2 is not numeric", color_of("v2 x", 1), COL_TEXT))
    checks.append(("heading", color_of("# hi", 1), COL_ACCENT))
    checks.append(("quote", color_of("> hi", 1), COL_TEXT2))
    checks.append(("bullet marker only", color_of("- hi", 0), COL_ACCENT))
    checks.append(("bullet text is plain", color_of("- hi", 1), COL_TEXT))
    checks.append(("-5 is not a bullet", color_of("-5 x", 0), COL_YELLOW))
    checks.append(("code span", color_of("`a` x", 1), COL_BLUE))

    failed = 0
    for name, got, want in checks:
        if got != want:
            failed += 1
            print("FAIL  %-32s got %r want %r" % (name, got, want))
    print("%d checks, %d failed" % (len(checks), failed))
    return 0 if failed == 0 else 1


# ── MAIN ──────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="Edit the CYD board's Note page from the terminal.")
    ap.add_argument("text", nargs="?", default=None,
                    help="new note text, or '-' to read it from stdin "
                         "(default: open $EDITOR instead)")
    ap.add_argument("--show", action="store_true",
                    help="print the board preview without editing")
    ap.add_argument("--raw", action="store_true",
                    help="print the stored text verbatim, no box and no colour")
    ap.add_argument("--append", default=None, metavar="TEXT",
                    help="append a line to the existing note")
    ap.add_argument("--clear", action="store_true",
                    help="empty the note")
    ap.add_argument("--size", type=int, default=None, choices=(1, 2, 3),
                    help="board text size 1-3 (default: leave unchanged)")
    ap.add_argument("--no-color", action="store_true",
                    help="never emit ANSI colour (also honours NO_COLOR)")
    ap.add_argument("--selftest", action="store_true",
                    help="check the wrap and tokenizer rules, then exit")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest())

    writers = [args.text is not None, args.append is not None, args.clear]
    if sum(1 for w in writers if w) > 1:
        print("Error: use only one of TEXT, --append or --clear")
        sys.exit(2)

    use_color = (not args.no_color
                 and not os.environ.get("NO_COLOR")
                 and sys.stdout.isatty())

    ok, msg, note = get_note()
    if not ok:
        print(SERVER_DOWN_HELP)
        print("       (%s)" % msg)
        sys.exit(1)
    current = note.get("text") or ""
    size = note.get("size") or 1

    if args.raw:
        sys.stdout.write(current)
        if current and not current.endswith("\n"):
            sys.stdout.write("\n")
        sys.exit(0)

    new_text = None
    if args.clear:
        new_text = ""
    elif args.append is not None:
        new_text = (current + "\n" + args.append) if current else args.append
    elif args.text == "-":
        new_text = sys.stdin.read()
    elif args.text is not None:
        new_text = args.text
    elif not args.show and args.size is None:
        new_text = edit_in_editor(current)
        if new_text is None:
            sys.exit(1)
        if new_text == current:
            print("unchanged")
            print(render_preview(current, size, use_color))
            sys.exit(0)

    new_size = args.size if args.size is not None else size

    if new_text is None and new_size == size:
        # Nothing to write -- --show, or --size repeating the current value.
        print(render_preview(current, new_size, use_color))
        sys.exit(0)

    sent = current if new_text is None else new_text
    ok, msg, stored = post_note(sent, new_size)
    if not ok:
        print("Error: %s" % msg)
        sys.exit(1)

    stored_text = stored.get("text") or ""
    print(render_preview(stored_text, stored.get("size") or 1, use_color))

    # Report what the server's sanitize_note() changed, derived by comparing
    # what we sent with what came back rather than re-deriving its rules.
    normalized = sent.replace("\r\n", "\n").replace("\r", "\n").replace("\t", " ")
    if len(stored_text) < len(normalized):
        print("truncated: %d of %d characters kept (the board's pane is the limit)"
              % (len(stored_text), len(normalized)))
    replaced = sum(1 for a, b in zip(normalized, stored_text) if a != b)
    if replaced:
        print("%d character%s replaced with '?' -- the board's font is ASCII-only"
              % (replaced, "" if replaced == 1 else "s"))
    print("saved -- the board picks it up on its next poll")


if __name__ == "__main__":
    main()

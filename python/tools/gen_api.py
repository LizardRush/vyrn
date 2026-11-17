#!/usr/bin/env python3
"""Make the editor's help from the vyrn library's own docstrings.

    python3 python/tools/gen_api.py --out-dir app/resources [--check]

writes

    <out-dir>/vyrn.api        autocompletion and call tips for the editor (QScintilla API format)
    <out-dir>/docs/api.html   the library reference for the Docs tab, with an anchor per name (#screen)

The public names are vyrn.__all__ plus the drawing methods of the pictures vyrn.canvas() makes (dot,
box). Each needs a docstring whose first line is a one-line summary; a plain value such as vyrn.tick is
documented by "#:" comment lines right above it (or a string right below it).

Exit status: 0 = files written (with --check: they are up to date), 1 = a public name has no docstring
(nothing is written) or --check found stale files, 2 = usage problem.
"""

import argparse
import ast
import dataclasses
import html
import inspect
import io
import keyword
import os
import re
import sys
import textwrap
import tokenize

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PYTHON_DIR = os.path.dirname(TOOLS_DIR)                       # the folder that holds the vyrn package
DEFAULT_OUT_DIR = os.path.join(os.path.dirname(PYTHON_DIR), "app", "resources")


@dataclasses.dataclass
class Entry:
    name: str           # plain name: "screen", "tick", "dot"
    api_name: str       # what the editor completes: "vyrn.screen", "Canvas.dot"
    shown: str          # what the reference shows: "vyrn.screen", "picture.dot"
    kind: str           # "function", "class", "value" or "method"
    signature: str      # "(x=None, y=None)"; "" for values
    doc: str            # cleaned docstring ("" when missing)
    anchor: str = ""    # HTML id: "screen"

    @property
    def summary(self):
        return self.doc.splitlines()[0].strip() if self.doc else ""


# ---- reading the library --------------------------------------------------------------------------

def load_vyrn():
    """Import the vyrn package from python/ (never an installed copy)."""
    sys.dont_write_bytecode = True                            # no __pycache__ in the source tree
    sys.path.insert(0, PYTHON_DIR)
    import vyrn
    found = os.path.dirname(os.path.abspath(vyrn.__file__))
    if os.path.dirname(found) != PYTHON_DIR:
        raise ImportError(f"imported vyrn from {found}, expected it in {PYTHON_DIR}")
    return vyrn


def own_doc(obj):
    """The object's own docstring, cleaned ("" when it has none; nothing is inherited).

    Read from the source when possible: Python 3.13 strips the common indentation from __doc__, so a
    docstring whose only body is an indented example would lose the indent that marks it as code.
    Here, only the indentation of the function body itself is removed.
    """
    try:
        node = ast.parse(textwrap.dedent(inspect.getsource(obj))).body[0]
        raw = ast.get_docstring(node, clean=False)
        indent = node.body[0].col_offset
    except (OSError, TypeError, SyntaxError, IndexError, AttributeError):
        raw = None
    if raw is None:
        doc = obj.__doc__ if isinstance(getattr(obj, "__doc__", None), str) else ""
        return inspect.cleandoc(doc) if doc.strip() else ""
    first, *rest = raw.expandtabs().splitlines() or [""]
    rest = [line[indent:] if line[:indent].isspace() else line.lstrip() for line in rest]
    return "\n".join([first.strip(), *rest]).strip()


def value_docs(module):
    """Docs of a module's plain values (numbers, None...), which can't carry a docstring themselves.

    For `name = value`, the doc is the run of "#:" comment lines right above it, else a string literal
    right below it, else a "#:" comment at the end of its line.
    """
    with open(inspect.getsourcefile(module), encoding="utf-8") as file:
        source = file.read()
    comments, code_rows = {}, set()
    for token in tokenize.generate_tokens(io.StringIO(source).readline):
        if token.type == tokenize.COMMENT:
            comments[token.start[0]] = token.string
        elif token.type not in (tokenize.NL, tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT,
                                tokenize.ENDMARKER):
            code_rows.update(range(token.start[0], token.end[0] + 1))

    def doc_comment(row):
        text = comments.get(row, "")
        return text[2:].removeprefix(" ") if text.startswith("#:") else None

    docs = {}
    body = ast.parse(source).body
    for index, node in enumerate(body):
        if isinstance(node, ast.Assign):
            names = [target.id for target in node.targets if isinstance(target, ast.Name)]
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            names = [node.target.id]
        else:
            continue
        above, row = [], node.lineno - 1
        while row > 0 and row not in code_rows and doc_comment(row) is not None:
            above.insert(0, doc_comment(row))
            row -= 1
        after = body[index + 1] if index + 1 < len(body) else None
        if above:
            doc = "\n".join(above)
        elif (isinstance(after, ast.Expr) and isinstance(after.value, ast.Constant)
              and isinstance(after.value.value, str)):
            doc = after.value.value
        else:
            doc = doc_comment(node.end_lineno) or ""
        for name in names:
            docs[name] = inspect.cleandoc(doc) if doc.strip() else ""
    return docs


def format_default(value):
    if isinstance(value, str):      # beginners see "double quotes" everywhere else in Vyrn
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return repr(value)


def format_signature(func, drop_first=False):
    """(x, y, color="white"): parameter names and defaults only, no annotations."""
    params = list(inspect.signature(func).parameters.values())
    if drop_first:
        params = params[1:]
    parts, star_done = [], False
    for index, param in enumerate(params):
        if param.kind == param.KEYWORD_ONLY and not star_done:
            parts.append("*")
        if param.kind in (param.VAR_POSITIONAL, param.KEYWORD_ONLY):
            star_done = True
        text = {param.VAR_POSITIONAL: "*", param.VAR_KEYWORD: "**"}.get(param.kind, "") + param.name
        if param.default is not param.empty:
            text += "=" + format_default(param.default)
        parts.append(text)
        if param.kind == param.POSITIONAL_ONLY and (index + 1 == len(params)
                                                     or params[index + 1].kind != param.POSITIONAL_ONLY):
            parts.append("/")
    return "(" + ", ".join(parts) + ")"


def collect(vyrn):
    """All documented entries, in order: vyrn.__all__ (as listed), then the canvas methods."""
    names = list(getattr(vyrn, "__all__", None) or [])
    if not names:
        raise SystemExit("gen_api.py: the vyrn package has no __all__ list of public names.")
    values = value_docs(vyrn)
    entries = []
    for name in names:
        if not hasattr(vyrn, name):
            raise SystemExit(f"gen_api.py: vyrn.__all__ lists {name!r}, but vyrn has no such name.")
        obj = getattr(vyrn, name)
        if inspect.isclass(obj) or callable(obj):
            kind = "class" if inspect.isclass(obj) else "function"
            entries.append(Entry(name, f"vyrn.{name}", f"vyrn.{name}", kind, format_signature(obj), own_doc(obj)))
        else:
            entries.append(Entry(name, f"vyrn.{name}", f"vyrn.{name}", "value", "", values.get(name, "")))

    # The pictures vyrn.canvas() makes: document their public methods (dot, box) too.
    canvas_class = type(vyrn.canvas(1, 1))
    for name, member in vars(canvas_class).items():
        if not name.startswith("_") and inspect.isfunction(member):
            entries.append(Entry(name, f"{canvas_class.__name__}.{name}", f"picture.{name}", "method",
                                 format_signature(member, drop_first=True), own_doc(member)))

    top_level = set(names)
    for entry in entries:
        clash = entry.kind == "method" and entry.name in top_level
        entry.anchor = f"{canvas_class.__name__.lower()}-{entry.name}" if clash else entry.name
    return entries


# ---- vyrn.api ---------------------------------------------------------------------------------------

def make_api(entries):
    """One entry per line: "vyrn.screen(x=None, ...) Summary.", values as a bare "vyrn.tick".

    QScintilla stops reading at the first empty line, and reads a line without "(" entirely as a name,
    so there are no blank or comment lines and values get no description.
    """
    lines = []
    for entry in entries:
        if entry.kind == "value":
            lines.append(entry.api_name)
        else:
            lines.append(f"{entry.api_name}{entry.signature} {entry.summary}".rstrip())
    return "\n".join(lines) + "\n"


# ---- docs/api.html ----------------------------------------------------------------------------------

STYLE = """
body { background-color: #15181d; color: #e2e6eb; margin: 24px 28px;
       font-family: "DejaVu Sans", "Noto Sans", "Segoe UI", sans-serif; font-size: 14px; line-height: 1.5; }
h1 { color: #ffffff; font-size: 26px; margin: 0 0 4px 0; }
h2 { color: #2dd4bf; font-size: 18px; margin: 30px 0 6px 0; border-bottom: 1px solid #2c333c; }
h3 { color: #f1f4f7; font-size: 16px; margin: 0 0 4px 0; font-family: "DejaVu Sans Mono", "Noto Sans Mono", monospace; }
a { color: #2dd4bf; text-decoration: none; }
a:hover { text-decoration: underline; }
code, pre { font-family: "DejaVu Sans Mono", "Noto Sans Mono", monospace; }
code { color: #f3c98b; }
a code { color: #2dd4bf; }
pre { background-color: #1e232a; color: #e2e6eb; border: 1px solid #2c333c; border-radius: 6px;
      padding: 10px 14px; margin: 6px 0 12px 0; font-size: 13px; white-space: pre-wrap; }
pre code { color: #e2e6eb; }
.lead { color: #c3cad3; font-size: 15px; }
.muted, .sig { color: #8e99a6; }
.summary { color: #ffffff; margin: 0 0 6px 0; }
.entry { margin: 18px 0 0 0; }
.label { color: #8e99a6; font-size: 12px; margin: 10px 0 0 0; }
table.toc { border-collapse: collapse; margin: 6px 0 0 0; }
table.toc td { padding: 3px 18px 3px 0; vertical-align: top; }
.k { color: #c792ea; } .s { color: #a5d6a7; } .n { color: #f9c97a; } .c { color: #7f8c98; font-style: italic; }
.footer { color: #6b7682; font-size: 12px; margin-top: 36px; }
"""

# Code-like bits of a docstring sentence: calls such as vyrn.press("space") or see(), vyrn.tick, "text".
INLINE_CODE = re.compile(r"""
    \b[A-Za-z_][\w.]*\((?:[^()"']|"[^"]*"|'[^']*'|\([^()]*\))*\)
  | \bvyrn\.\w+
  | "[^"\n]*"
""", re.VERBOSE)


def inline(text, anchors):
    """Escape a sentence, setting code in <code> and linking mentions of documented vyrn names."""
    out, position = [], 0
    for match in INLINE_CODE.finditer(text):
        out.append(html.escape(text[position:match.start()], quote=False))
        code = f"<code>{html.escape(match.group(0), quote=False)}</code>"
        name = re.match(r"vyrn\.(\w+)", match.group(0))
        if name and name.group(1) in anchors:
            code = f'<a href="#{anchors[name.group(1)]}">{code}</a>'
        out.append(code)
        position = match.end()
    out.append(html.escape(text[position:], quote=False))
    return "".join(out)


def highlight(code):
    """Python example → HTML with keywords, strings, numbers and comments marked (plain on trouble)."""
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(code + "\n").readline))
    except (tokenize.TokenError, SyntaxError):
        return html.escape(code, quote=False)
    starts = [0]
    for line in (code + "\n").splitlines(keepends=True):
        starts.append(starts[-1] + len(line))
    string_types = {tokenize.STRING} | {getattr(tokenize, name) for name in
                                        ("FSTRING_START", "FSTRING_MIDDLE", "FSTRING_END") if hasattr(tokenize, name)}
    out, position = [], 0
    for token in tokens:
        if token.start[0] > len(code.splitlines()) or token.type == tokenize.ENDMARKER:
            break
        start = starts[token.start[0] - 1] + token.start[1]
        end = min(starts[token.end[0] - 1] + token.end[1], len(code))
        if start < position or start >= end:
            continue
        out.append(html.escape(code[position:start], quote=False))
        text = html.escape(code[start:end], quote=False)
        css = ("c" if token.type == tokenize.COMMENT else "s" if token.type in string_types else
               "n" if token.type == tokenize.NUMBER else
               "k" if token.type == tokenize.NAME and keyword.iskeyword(token.string) else "")
        out.append(f'<span class="{css}">{text}</span>' if css else text)
        position = end
    out.append(html.escape(code[position:], quote=False))
    return "".join(out)


def doc_blocks(text):
    """Split docstring text into ("p", text), ("list", [items]) and ("code", text) blocks.

    Indented lines are a code example; lines starting with "- " make a list; the rest are paragraphs.
    """
    lines, blocks, i = text.splitlines(), [], 0
    indented = lambda line: line[:1] in (" ", "\t")              # noqa: E731
    bullet = lambda line: re.match(r"[-*] ", line) is not None    # noqa: E731
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
        elif indented(line):
            j = i
            while j < len(lines) and (not lines[j].strip() or indented(lines[j])):
                j += 1
            blocks.append(("code", textwrap.dedent("\n".join(lines[i:j])).strip("\n")))
            i = j
        elif bullet(line):
            items = []
            while i < len(lines) and lines[i].strip() and (bullet(lines[i]) or (items and indented(lines[i]))):
                if bullet(lines[i]):
                    items.append(lines[i][2:].strip())
                else:
                    items[-1] += " " + lines[i].strip()
                i += 1
            blocks.append(("list", items))
        else:
            j = i
            while j < len(lines) and lines[j].strip() and not indented(lines[j]) and not bullet(lines[j]):
                j += 1
            blocks.append(("p", " ".join(part.strip() for part in lines[i:j])))
            i = j
    return blocks


def entry_html(entry, anchors):
    rest = "\n".join(entry.doc.splitlines()[1:])
    parts = [f'<div class="entry">',
             f'<h3 id="{entry.anchor}">{html.escape(entry.shown)}'
             f'<span class="sig">{html.escape(entry.signature)}</span></h3>',
             f'<p class="summary">{inline(entry.summary, anchors)}</p>']
    examples = 0
    for kind, content in doc_blocks(rest):
        if kind == "p":
            parts.append(f"<p>{inline(content, anchors)}</p>")
        elif kind == "list":
            parts.append("<ul>" + "".join(f"<li>{inline(item, anchors)}</li>" for item in content) + "</ul>")
        else:
            examples += 1
            parts.append(f'<p class="label">{"Example" if examples == 1 else "Another example"}</p>')
            parts.append(f"<pre><code>{highlight(content)}</code></pre>")
    parts.append("</div>")
    return "\n".join(parts)


def make_html(entries, vyrn):
    anchors = {entry.name: entry.anchor for entry in entries if entry.kind != "method"}
    groups = [
        ("functions", "Functions", "Call these from your scripts.",
         [e for e in entries if e.kind in ("function", "class")]),
        ("values", "What is happening now", "Vyrn keeps these up to date while the AI plays; just read them.",
         [e for e in entries if e.kind == "value"]),
        ("drawing", "Drawing on a canvas",
         'A picture from <a href="#canvas"><code>vyrn.canvas()</code></a> has these methods, '
         "for example <code>picture = vyrn.canvas(10, 10)</code> then <code>picture.dot(3, 4, \"white\")</code>. "
         "x counts columns from the left and y rows from the top; drawing off the edge is simply left out.",
         [e for e in entries if e.kind == "method"]),
    ]
    groups = [group for group in groups if group[3]]
    version = getattr(vyrn, "__version__", "")
    out = ["<!DOCTYPE html>",
           '<html lang="en">',
           "<!-- Generated by python/tools/gen_api.py from the vyrn library's docstrings: "
           "edit those, not this file. -->",
           "<head>",
           '<meta charset="utf-8">',
           '<meta name="viewport" content="width=device-width, initial-scale=1">',
           '<meta name="color-scheme" content="dark">',
           "<title>vyrn library reference</title>",
           f"<style>{STYLE}</style>",
           "</head>",
           "<body>",
           f'<h1>vyrn <span class="muted">library reference{" " + html.escape(version) if version else ""}</span></h1>',
           '<p class="lead">Everything your scripts can use. Start a script with <code>import vyrn</code>, '
           "then call these in inputs.py, responses.py, points.py and world.py. "
           "In the editor, press F1 on a <code>vyrn.</code> name to jump here.</p>",
           '<table class="toc">']
    for _, title, _, members in groups:
        for entry in members:
            out.append(f'<tr><td><a href="#{entry.anchor}"><code>{html.escape(entry.shown + entry.signature)}'
                       f"</code></a></td><td>{inline(entry.summary, anchors)}</td></tr>")
    out.append("</table>")
    for group_id, title, intro, members in groups:
        out.append(f'<h2 id="{group_id}">{html.escape(title)}</h2>')
        out.append(f'<p class="muted">{intro}</p>')
        out.extend(entry_html(entry, anchors) for entry in members)
    out += ['<p class="footer">Made from the vyrn library\'s own docstrings (python/vyrn/__init__.py).</p>',
            "</body>",
            "</html>"]
    return "\n".join(out) + "\n"


# ---- main -------------------------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description="Generate vyrn.api and docs/api.html from the vyrn docstrings.")
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR,
                        help="folder for vyrn.api and docs/api.html (default: app/resources)")
    parser.add_argument("--check", action="store_true",
                        help="write nothing; exit 1 if the files there are missing or out of date")
    args = parser.parse_args(argv)

    try:
        vyrn = load_vyrn()
    except Exception as err:                    # anything wrong in the library itself
        print(f"gen_api.py: can't import the vyrn package from {PYTHON_DIR}: {err}", file=sys.stderr)
        return 1
    entries = collect(vyrn)
    missing = [entry for entry in entries if not entry.summary]
    if missing:
        print("gen_api.py: every public name needs a docstring whose first line sums it up. Missing:",
              file=sys.stderr)
        for entry in missing:
            how = ('add "#:" comment lines right above it in python/vyrn/__init__.py' if entry.kind == "value"
                   else "add a docstring")
            print(f"  {entry.api_name}: {how}", file=sys.stderr)
        return 1

    outputs = {os.path.join(args.out_dir, "vyrn.api"): make_api(entries),
               os.path.join(args.out_dir, "docs", "api.html"): make_html(entries, vyrn)}
    if args.check:
        stale = []
        for path, text in outputs.items():
            try:
                with open(path, encoding="utf-8", newline="") as file:
                    if file.read() == text:
                        continue
            except OSError:
                pass
            stale.append(path)
        for path in stale:
            print(f"gen_api.py: {path} is missing or out of date; run python3 python/tools/gen_api.py "
                  f"--out-dir {args.out_dir}", file=sys.stderr)
        return 1 if stale else 0

    for path, text in outputs.items():
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as file:
            file.write(text)
    print(f"gen_api.py: wrote {len(entries)} entries to " + " and ".join(outputs))
    print("  " + ", ".join(entry.shown for entry in entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())

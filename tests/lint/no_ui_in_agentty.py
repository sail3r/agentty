#!/usr/bin/env python3
r"""no_ui_in_agentty — agentty builds widget Configs; maya draws everything.

The rule (docs/UI.md, AGENTS.md): no code under src/ or include/agentty may
construct UI. Every pixel comes from a maya widget; agentty only fills a
widget's Config from the Model and hands it over. This lint fails the build's
test run on any way of building UI by hand:

  * the DSL           using namespace maya / maya::dsl, any `dsl::` name
  * builder calls     maya::text / h / v / hstack / vstack / box / component /
                      fill / paint / adapt / fit_row / fit_col / zstack /
                      center / place / pick / clamp / gradient / rainbow /
                      responsive / nothing / blank / spacer / list_ref /
                      maya::detail::*
  * unqualified ones  text( h( v( hstack( vstack( component( spacer( blank(
                      nothing( zstack( fit_row( -- reachable by ADL through a
                      maya::Style argument, so the name alone is banned
  * element nodes     Element{ TextElement BoxElement ComponentElement
                      ElementList
  * layout pipes      | border( | bcolor( | btext( | padding( | margin( |
                      | bgc( | grow( | shrink( | clip | width( | height( ...
  * builder headers   <maya/dsl.hpp> <maya/element/builder.hpp>
                      <maya/element/text.hpp> <maya/element/box.hpp>
  * raw escapes       "\x1b[" / "\033[" / "\e[" / "\x1b]" in a string literal: painting
                      the terminal by hand (colour, cursor, scroll margins)
  * styled runs       maya::StyledRun / `.runs =` -- composing per-byte styled
                      text is drawing; a widget takes the data and styles it
  * CLI layout        a printf / fprintf / fputs / puts / std::cout statement
                      whose text lays out columns (%-16s, %3d, {:<16}) or draws
                      marks and rules (✓ ✗ ● ○ ▸ ━ ─ │ ╭ ╰): print a
                      maya::report instead. A plain sentence or a value meant
                      for a pipe is not layout.

Comments and string literals are stripped first, so prose about the DSL is
fine and a log message saying "text(" is not a violation.

There is NO allowlist. A file that needs to draw something needs a maya
widget for it (third_party/maya/include/maya/widget/agentty/).

  python3 tests/lint/no_ui_in_agentty.py <repo_root>   # exit 1 on violations
"""
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
SCAN = [os.path.join(ROOT, "src"), os.path.join(ROOT, "include", "agentty")]
EXT = (".cpp", ".hpp", ".h", ".cc", ".ipp")

# maya's UI-building surface, read from dsl.hpp and element/*.hpp: the free
# builder functions (maya:: and maya::dsl::) and the pipe modifiers.
BUILDERS = (r"text|h|v|hstack|vstack|box|center|component|fill|paint|adapt|"
            r"fit_row|fit_rows|fit_col|zstack|place|pick|clamp|gradient|"
            r"gradient_rule|rainbow|paint_per_cp|responsive|nothing|blank|"
            r"spacer|separator|list_ref|ledger_ref|row|col|columns|grid|"
            r"sidebar|viewport|glyphs|pixels|when|each|map|dyn|as_wrapped|visible")
# Bare names a maya::Style argument can reach by ADL, or the dsl brings in.
UNQUALIFIED = (r"text|h|v|hstack|vstack|component|spacer|blank|nothing|"
               r"zstack|fit_row|fit_col|separator|center|box")
PIPES = (r"align|bcolor|bgc|border|btext|fgc|gap|grow|height|hit|justify|"
         r"margin|overflow|padding|scroll|scrollx|scrolly|shrink|visible|"
         r"width|clip")

# Checked on the source with comments removed but string literals KEPT.
ESCAPE = re.compile(r'"(?:[^"\\\n]|\\.)*(?:\\x1[bB]|\\033|\\e)[\[\]](?:[^"\\\n]|\\.)*"')
RUNS = re.compile(r"\bStyledRun\b|\.runs\s*=(?!=)")
OUTPUT = re.compile(r"(?<![\w:.>])(?:std::)?(?:printf|fprintf|fputs|puts)\s*\(|\bstd::(?:cout|cerr)\b")
LAYOUT = re.compile(
    r"%-?\d+(?:l|ll|z|h)?[sduxf]|%-\*s|\{:[<>^]\d+\}|"
    r"[✓✗●○▸━─│╭╰╮╯┃┊]|"
    r"\\x[eE]2\\x9[cC]\\x9[37]|\\x[eE]2\\x9[45]\\x[0-9a-fA-F]{2}|\\x[eE]2\\x97\\x8[bfBF]|"
    r"\\x[eE]2\\x96\\x[bB]8")
LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')

RULES = [
    ("DSL namespace",   re.compile(r"\busing\s+namespace\s+maya\b")),
    ("DSL namespace",   re.compile(r"(?<![\w:])dsl::")),
    ("builder call",    re.compile(r"\bmaya::(?:dsl::)?(?:" + BUILDERS + r")\s*\(")),
    ("builder call",    re.compile(r"\bmaya::detail::(?:component|fit_row|fit_col|adapt|fill|paint)\b")),
    ("element node",    re.compile(r"\b(?:maya::)?(?:Element\s*\{|TextElement|BoxElement|"
                                   r"ComponentElement|ElementList)\b")),
    ("layout pipe",     re.compile(r"\|\s*(?:maya::(?:dsl::)?)?(?:" + PIPES + r")\b\s*(?:\(|;|\)|$)")),
    ("builder header",  re.compile(r"#\s*include\s*[<\"]maya/(?:dsl|element/(?:builder|text|box))\.hpp[>\"]")),
]

# An unqualified builder name at call position. Skipped: member calls
# (`b.text(`), other namespaces (`mcp::text(`), and declarations /
# constructors, whose name follows a TYPE (`std::string v(`, `Field text(`,
# `Builder& text(`). A keyword before it (`return text(`) is still a call.
UNQ = re.compile(r"\b(" + UNQUALIFIED + r")\s*\(")
KEYWORDS = {"return", "co_return", "co_yield", "else", "case", "do", "throw",
            "and", "or", "not", "new", "sizeof", "decltype"}

def unqualified_call(line):
    for m in UNQ.finditer(line):
        before = line[:m.start()].rstrip()
        if before.endswith((".", "->", "::")):
            continue
        if re.search(r"[\w>]\s*[&*]+$", before):
            continue                       # `Builder& text(` -- a declaration
        tok = re.search(r"([A-Za-z_]\w*|>)$", before)
        if tok and tok.group(1) not in KEYWORDS:
            continue                       # `std::string v(` / `Field text(`
        return True
    return False


def strip(src, keep_strings=False):
    """Blank out comments and string/char literals, keeping line numbers."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j])); i = j
        elif c == "R" and src.startswith('R"', i) and (i == 0 or not (src[i-1].isalnum() or src[i-1] == "_")):
            m = re.match(r'R"([^(\s]{0,16})\(', src[i:])
            if not m:
                out.append(c); i += 1; continue
            end = ")" + m.group(1) + '"'
            j = src.find(end, i + m.end())
            j = n if j < 0 else j + len(end)
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j])); i = j
        elif c in "\"'" and keep_strings:
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\": j += 1
                elif src[j] == "\n": break
                j += 1
            out.append(src[i:j + 1]); i = j + 1
        elif c in "\"'":
            # `#include "x"` keeps its path: the header rule needs it.
            line_start = src.rfind("\n", 0, i) + 1
            if c == '"' and re.match(r"\s*#\s*include\s*$", src[line_start:i]):
                j = src.find('"', i + 1)
                out.append(src[i:j + 1]); i = j + 1; continue
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\": j += 1
                elif src[j] == "\n": break
                j += 1
            out.append(c + " " * (j - i - 1) + (c if j < n else "")); i = j + 1
        else:
            out.append(c); i += 1
    return "".join(out)

violations = []
for base in SCAN:
    for d, _, fs in os.walk(base):
        for f in sorted(fs):
            if not f.endswith(EXT):
                continue
            p = os.path.join(d, f)
            text = strip(open(p, encoding="utf-8", errors="replace").read())
            for ln, line in enumerate(text.splitlines(), 1):
                hit = None
                for name, rx in RULES:
                    if rx.search(line):
                        hit = name
                        break
                if hit is None and unqualified_call(line):
                    hit = "builder call"
                if hit:
                    violations.append((os.path.relpath(p, ROOT), ln, hit, line.strip()))
            # Same file, string literals kept: escapes, runs, CLI layout.
            raw = strip(open(p, encoding="utf-8", errors="replace").read(), keep_strings=True)
            lines = raw.splitlines()
            for ln, line in enumerate(lines, 1):
                if ESCAPE.search(line):
                    violations.append((os.path.relpath(p, ROOT), ln, "raw escape", line.strip()))
                elif RUNS.search(LITERAL.sub('""', line)):
                    violations.append((os.path.relpath(p, ROOT), ln, "styled runs", line.strip()))
            # An output statement runs from the call to its `;`; its format
            # string is often on a later line.
            for m in OUTPUT.finditer(raw):
                end = m.end()
                depth = 0
                while end < len(raw):
                    ch = raw[end]
                    if ch == '"':
                        lm = LITERAL.match(raw, end)
                        end = lm.end() if lm else end + 1
                        continue
                    if ch in "([{": depth += 1
                    elif ch in ")]}": depth -= 1
                    elif ch == ";" and depth <= 0: break
                    end += 1
                stmt = raw[m.start():end]
                if any(LAYOUT.search(s) for s in LITERAL.findall(stmt)):
                    ln = raw.count("\n", 0, m.start()) + 1
                    violations.append((os.path.relpath(p, ROOT), ln, "CLI layout",
                                       lines[ln - 1].strip()))

if violations:
    files = sorted({v[0] for v in violations})
    print(f"no_ui_in_agentty: {len(violations)} UI construction(s) in {len(files)} file(s).")
    print("agentty fills widget Configs; maya draws. Move the drawing into a maya")
    print("widget under third_party/maya/include/maya/widget/agentty/.\n")
    if "--summary" in sys.argv:
        from collections import Counter
        for f, k in Counter(v[0] for v in violations).most_common():
            print(f"  {k:4d}  {f}")
    else:
        for f, ln, name, line in violations:
            print(f"{f}:{ln}: [{name}] {line[:110]}")
    sys.exit(1)
print("no_ui_in_agentty: clean")

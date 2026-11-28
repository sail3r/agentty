#!/usr/bin/env python3
"""view_executor: maya::exec is the VIEW library's parallelism; only agentty's
view/app layer may use it.

maya::exec (maya/core/executor.hpp) is how maya's renderer asks for helper
threads without linking a runtime. It is installed only inside maya::run, so
outside the TUI -- ACP, `agentty run`, a CLI verb, a test -- it silently runs
on the calling thread. And it takes no capability, so a blocking fork-join
can run under a lock unchecked.

ThreadLog::range used it from io/ (commit 1fde507a): every non-TUI process
parsed threads on one core, the save path forked-and-joined inside the
thread-log lock, and the narrow race test failed to LINK because the hook's
code lives in libmaya, the renderer (PR #138).

Everything outside the view layer uses maya::parallel_for(cap, n, width, f):
jaal's, the same in every process, and it needs `unlocked`, so the compiler
refuses it under a lock.

Allowed: src/runtime/ and include/agentty/runtime/ (the view and app, which
run inside maya::run), and tests/. Fails on #include <maya/core/executor.hpp>
or maya::exec anywhere else. Comments and strings may say anything.

Usage: view_executor.py ROOT
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from disk_writes import strip  # noqa: E402

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
ALLOWED = ("src/runtime/", "include/agentty/runtime/")
INCLUDE = re.compile(r'#\s*include\s*[<"]maya/core/executor\.hpp[>"]')
USE = re.compile(r"\bmaya::exec::|\bexec::(parallel_for|post)\b")

hits = []
for top in ("src", "include/agentty"):
    for d, _, fs in os.walk(os.path.join(ROOT, top)):
        for f in sorted(fs):
            if not f.endswith((".cpp", ".hpp", ".h")):
                continue
            p = os.path.join(d, f)
            rel = os.path.relpath(p, ROOT).replace(os.sep, "/")
            if rel.startswith(ALLOWED):
                continue
            raw = open(p, encoding="utf-8", errors="replace").read()
            code = strip(raw)
            for ln, (r, c) in enumerate(zip(raw.splitlines(), code.splitlines()), 1):
                if INCLUDE.search(r) or USE.search(c):
                    hits.append(f"{rel}:{ln}: {r.strip()[:110]}")

if hits:
    print("view_executor: maya::exec used outside agentty's view layer:\n")
    for h in hits:
        print("  " + h)
    print("\nOutside src/runtime/, use maya::parallel_for(cap, n, width, body):"
          "\nit runs the same in every process and needs `unlocked`.")
    sys.exit(1)
print("view_executor: ok")

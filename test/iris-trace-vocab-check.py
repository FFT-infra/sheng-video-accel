#!/usr/bin/env python3
"""Check that every session/encode trace prefix has a §8 vocabulary entry."""
import pathlib, re, sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
if not (root / "docs/architecture.md").is_file():
    print(f"FAIL no docs/architecture.md under {root}", file=sys.stderr)
    sys.exit(2)
doc = (root / "docs/architecture.md").read_text()
sec = doc.split("## 8. Trace vocabulary", 1)[1].split("## 9.", 1)[0]
block = sec.split("```", 2)[1]
lines = [l.strip() for l in block.splitlines() if l.strip() and not l.startswith("(")]
# The vocabulary uses <n>/<i>/<hex>/<why> placeholders; reduce both sides to
# a literal prefix ending before the first placeholder or printf conversion.
def head(s):
    s = re.split(r"[%<]", s, maxsplit=1)[0]
    return re.sub(r"\s+", " ", s).strip().rstrip("| ")
entries = {head(l) for l in lines if head(l)}

# Accepted exceptions: VA startup/teardown diagnostics, not session
# completions. Keep this list in sync with the paragraph after the §8 block.
accepted = (
    "va init node", "va error where", "va sync failed surface status",
    "va destroy_context encode finish failed",
)
prefixes = set()
pat = re.compile(r'(?:trace_?\(|\.trace\()\s*"([^"\\]*(?:\\.[^"\\]*)*)"')
for p in sorted((root / "src").rglob("*.cc")):
    text = p.read_text(errors="replace")
    for m in pat.finditer(text):
        fmt = m.group(1)
        if fmt.startswith("%s token="):
            prefixes.add("capture token")
            prefixes.add("capture noshow token")
            continue
        prefixes.add(head(fmt))
missing = sorted(p for p in prefixes
                 if not any(p.startswith(a) or a.startswith(p) for a in accepted)
                 and not any(p.startswith(e) or e.startswith(p) for e in entries))
if missing:
    print("FAIL trace records missing from docs/architecture.md §8:", file=sys.stderr)
    for p in missing:
        print("  " + p, file=sys.stderr)
    sys.exit(1)
print(f"PASS §8 trace vocabulary covers {len(prefixes)} emitter prefixes"
      f" ({len(accepted)} accepted exceptions)")

"""Check the game data on the Vita memory card against the PC copy.

The card stores the data flattened (see vita/flatten_data.py), so each PC file
data/a/b/c.ext is looked for at <card>/data~a~b/c.ext. Reports missing files
and files whose size differs.

    python tools/compare_card.py gamedata/GloftR6HP H:/data/r6sv/gamedata
"""
import os
import sys


def flat(rel):
    parts = [x for x in rel.replace(os.sep, "/").split("/") if x]
    folder = "~".join(parts[:-1])
    return os.path.join(folder, parts[-1]) if folder else parts[-1]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    missing, diff, n = [], [], 0
    for dp, _, fs in os.walk(src):
        for f in fs:
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, src)
            if not rel.replace(os.sep, "/").startswith("data/"):
                continue
            n += 1
            t = os.path.join(dst, flat(rel))
            try:
                s = os.path.getsize(t)
            except OSError:
                missing.append(rel)
                continue
            if s != os.path.getsize(full):
                diff.append((rel, os.path.getsize(full), s))
    print("checked", n, "missing", len(missing), "size differs", len(diff))
    for m in missing:
        print("MISSING", m)
    for d in diff:
        print("DIFF", *d)


if __name__ == "__main__":
    main()

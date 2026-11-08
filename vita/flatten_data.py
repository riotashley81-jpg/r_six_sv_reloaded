"""Lay out the game data the way the Vita loader reads it.

The Vita can't open files more than 9 folders below ux0:, and the game's data
goes up to 9 folders deep on its own. So on the Vita each file's folder path
becomes a single folder, with '~' in place of '/':

    data/3d/characters/US/Textures/a.tga -> data~3d~characters~US~Textures/a.tga

This must match flatten() in src/hle_stdio.cpp.

usage:
  python vita/flatten_data.py SRC DST         copy SRC (e.g. gamedata/GloftR6HP) to DST, flattened
  python vita/flatten_data.py --in-place DIR  rearrange an already copied DIR (moves files)
  add --dry-run to only report what would happen
"""
import os
import shutil
import sys
import time


def flat_rel(rel):
    parts = [p for p in rel.replace("\\", "/").split("/") if p]
    folder = "~".join(parts[:-1])
    return os.path.join(folder, parts[-1]) if folder else parts[-1]


def files_under(root):
    for dirpath, _, names in os.walk(root):
        for n in names:
            full = os.path.join(dirpath, n)
            yield full, os.path.relpath(full, root)


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = len(args) != len(sys.argv) - 1
    in_place = args[:1] == ["--in-place"]
    if in_place:
        args = args[1:]
    if len(args) != (1 if in_place else 2):
        sys.exit(__doc__)
    src = args[0]
    dst = src if in_place else args[1]

    done = skipped = 0
    for full, rel in list(files_under(src)):
        target = os.path.join(dst, flat_rel(rel))
        if os.path.normcase(os.path.abspath(target)) == os.path.normcase(os.path.abspath(full)):
            skipped += 1  # already where the loader looks
            continue
        if not dry:
            for attempt in range(5):  # a Vita in USB mode drops requests now and then
                try:
                    os.makedirs(os.path.dirname(target) or dst, exist_ok=True)
                    if in_place:
                        os.replace(full, target)
                    else:
                        shutil.copy2(full, target)
                    break
                except OSError:
                    if attempt == 4:
                        raise
                    time.sleep(1)
        done += 1
    if in_place and not dry:
        # remove the now empty nested folders, deepest first
        for dirpath, _, _ in sorted(os.walk(src), key=lambda w: -w[0].count(os.sep)):
            if dirpath != src and not os.listdir(dirpath):
                os.rmdir(dirpath)
    verb = "would move" if dry and in_place else "would copy" if dry else "moved" if in_place else "copied"
    print(f"{verb} {done} files, {skipped} already in place")


if __name__ == "__main__":
    main()

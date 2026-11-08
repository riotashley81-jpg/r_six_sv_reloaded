"""Put a game data folder (from any device build) on the Vita memory card in one go.

    python tools/vita_install_data.py SOURCE [CARD]

SOURCE is the data as you have it: a .rar/.zip/.7z archive (needs 7-Zip) or an
already extracted folder (the one that contains "data"). CARD is the Vita's
memory card as mounted by VitaShell's USB mode, e.g. H:. Leave it out to use
the drive that has data/r6sv or app/RSIX00001 on it.

What it does:
  1. extracts the archive to a temporary folder (archives only);
  2. renames the data already on the card to gamedata-<date> (nothing is deleted);
  3. copies the new data to ux0:data/r6sv/gamedata, laid out the way the Vita
     loader reads it (see vita/flatten_data.py), leaving out the save files
     that came with the source;
  4. copies your saves over from the old data, so your progress stays;
  5. checks every file arrived at the right size;
  6. copies vita/r6sv.vpk to the card root, ready to install in VitaShell.

Options: --no-vpk skips step 6, --keep-saves-from-source uses the save files
that came with the source instead of yours.
"""
import os
import shutil
import string
import subprocess
import sys
import tempfile
import time

PORT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(PORT, "vita"))
from flatten_data import flat_rel  # noqa: E402  (same layout the loader expects)

ARCHIVES = (".rar", ".zip", ".7z")


def fail(msg):
    print("\nERROR: " + msg)
    sys.exit(1)


def retry(fn, *args):
    """A Vita in USB mode drops requests now and then."""
    for attempt in range(5):
        try:
            return fn(*args)
        except OSError:
            if attempt == 4:
                raise
            time.sleep(1)


def find_card():
    hits = []
    for d in string.ascii_uppercase:
        root = d + ":\\"
        if os.path.isdir(root + "data\\r6sv") or os.path.isdir(root + "app\\RSIX00001"):
            hits.append(root)
    if len(hits) == 1:
        return hits[0]
    if not hits:
        fail("no Vita card found. Start USB mode in VitaShell (Start button > USB mode), "
             "or give the drive letter, e.g.  vita_install_data.py SOURCE H:")
    fail("several drives look like a Vita card (%s); give the one to use as the second argument" % ", ".join(hits))


def find_7zip():
    for p in (shutil.which("7z"), r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe"):
        if p and os.path.isfile(p):
            return p
    fail("7-Zip is needed to unpack archives (https://www.7-zip.org), or extract it yourself and pass the folder")


def find_data_root(folder):
    """The folder that holds 'data' (the archive may wrap it in one or two folders)."""
    for depth in range(3):
        for dirpath, dirs, _ in os.walk(folder):
            if dirpath[len(folder):].count(os.sep) != depth:
                continue
            if "data" in dirs and os.path.isdir(os.path.join(dirpath, "data", "3d")):
                return dirpath
    fail("no game data in %s (expected a folder with data/3d inside)" % folder)


def is_root_file(rel):
    return os.sep not in rel and "/" not in rel


def files_under(root):
    for dirpath, _, names in os.walk(root):
        for n in names:
            full = os.path.join(dirpath, n)
            yield full, os.path.relpath(full, root)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if not 1 <= len(args) <= 2:
        sys.exit(__doc__)
    source = os.path.abspath(args[0].strip('"'))
    if not os.path.exists(source):
        fail("not found: " + source)
    card = args[1] if len(args) > 1 else find_card()
    if len(card) == 1 or card.endswith(":"):
        card = card[0] + ":\\"
    r6 = os.path.join(card, "data", "r6sv")
    gamedata = os.path.join(r6, "gamedata")
    print("Card:   " + card)
    print("Source: " + source)

    tmp = None
    try:
        if os.path.isfile(source) and source.lower().endswith(ARCHIVES):
            tmp = tempfile.mkdtemp(prefix="r6sv_data_")
            print("\n[1/6] Extracting the archive...")
            r = subprocess.run([find_7zip(), "x", "-y", "-bso0", "-bsp1", "-o" + tmp, source])
            if r.returncode != 0:
                fail("7-Zip could not extract the archive")
            src = find_data_root(tmp)
        elif os.path.isdir(source):
            print("\n[1/6] Source is a folder, nothing to extract.")
            src = find_data_root(source)
        else:
            fail("SOURCE must be a folder or a .rar/.zip/.7z archive")

        new_files = [(f, rel) for f, rel in files_under(src)
                     if not is_root_file(rel) or rel == ".nomedia" or "--keep-saves-from-source" in flags]
        need = sum(os.path.getsize(f) for f, _ in new_files)
        free = shutil.disk_usage(card).free
        print("      %d files, %.0f MB; the card has %.0f MB free" % (len(new_files), need / 2**20, free / 2**20))
        if need + 50 * 2**20 > free:
            fail("not enough space on the card. Delete an old gamedata-* backup in %s and try again" % r6)

        backup = None
        os.makedirs(r6, exist_ok=True)
        if os.path.isdir(gamedata):
            backup = os.path.join(r6, "gamedata-" + time.strftime("%Y%m%d-%H%M%S"))
            print("\n[2/6] Keeping the current data as " + backup)
            retry(os.rename, gamedata, backup)
        else:
            print("\n[2/6] No data on the card yet.")

        print("\n[3/6] Copying to %s (this takes a while over USB)..." % gamedata)
        for i, (f, rel) in enumerate(new_files, 1):
            target = os.path.join(gamedata, flat_rel(rel))
            retry(os.makedirs, os.path.dirname(target), 0o777, True)
            retry(shutil.copyfile, f, target)
            if i % 250 == 0 or i == len(new_files):
                print("      %d / %d" % (i, len(new_files)), end="\r", flush=True)
        print()

        saves = []
        if backup and "--keep-saves-from-source" not in flags:
            print("\n[4/6] Bringing your saves over from the old data...")
            for n in os.listdir(backup):
                p = os.path.join(backup, n)
                if os.path.isfile(p):
                    retry(shutil.copyfile, p, os.path.join(gamedata, n))
                    saves.append(n)
            print("      " + (", ".join(saves) if saves else "none found"))
        else:
            print("\n[4/6] No saves to bring over.")

        print("\n[5/6] Checking the copy...")
        bad = [rel for f, rel in new_files
               if not os.path.isfile(os.path.join(gamedata, flat_rel(rel)))
               or os.path.getsize(os.path.join(gamedata, flat_rel(rel))) != os.path.getsize(f)]
        if bad:
            for rel in bad[:20]:
                print("      BAD " + rel)
            fail("%d files did not copy correctly. Reconnect USB mode and run this again" % len(bad))
        print("      all %d files OK" % len(new_files))

        vpk = os.path.join(PORT, "vita", "r6sv.vpk")
        if "--no-vpk" not in flags and os.path.isfile(vpk):
            print("\n[6/6] Copying r6sv.vpk to the card root...")
            retry(shutil.copyfile, vpk, os.path.join(card, "r6sv.vpk"))
        else:
            print("\n[6/6] Skipping the vpk.")

        print("\nDone. Now:")
        print("  - eject the card in Windows (right-click %s > Eject) before leaving USB mode;" % card[:2])
        if "--no-vpk" not in flags and os.path.isfile(vpk):
            print("  - in VitaShell, install ux0:r6sv.vpk (press X on it), then start the game.")
        if backup:
            print("  - your previous data is in %s; delete it once the game works," % os.path.basename(backup))
            print("    or rename it back to gamedata to switch back.")
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()

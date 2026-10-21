#!/usr/bin/env bash
# The golden snapshot: dc2tk's behaviour, byte for byte. Runs the release binary over the extracted
# disc and the mods, hashes every file it writes and every disc image it builds, and with a
# baseline compares the two. See src/docs/development.md.
#
#   tools/golden.sh <out-dir> [<baseline-dir>]
#
#   (cd src && cargo build --release) && tools/golden.sh golden/base             # take a baseline
#   (cd src && cargo build --release) && tools/golden.sh golden/new golden/base  # compare with it
#
# The disc images (logs/*.sha1: pack, build_full, build_<mod>) are the hard limit: they must
# never change. Everything else (decompiled C, assets, reports, messages) should stay identical
# unless a change means it to differ; logs/ diffs readably with `diff -r base/logs new/logs`.
#
# Needs extracted/disc (dc2tk setup or extract) and about 6 GB free: a disc image is written,
# hashed and deleted for each build. Settings, by environment variable:
#   DC2TK        the binary (default build/release/dc2tk, .exe on Windows)
#   GOLDEN_DIR   where the retail image and the mods copy live (default golden/)
#   GOLDEN_MODS  the mods to build (default: every folder of $GOLDEN_DIR/mods with a mod.json)
#
# The retail image builds start from is $GOLDEN_DIR/packed.iso, which the first run packs from
# extracted/disc (an unedited extracted disc packs to the retail bytes). The mods are a copy in
# $GOLDEN_DIR/mods, made from mods/ on the first run, so editing a mod doesn't move the baseline;
# delete the copy to take a new one.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
OUT=${1:?usage: tools/golden.sh <out-dir> [<baseline-dir>]}
BASE=${2:-}
EXE=$ROOT/build/release/dc2tk
[ -f "$EXE.exe" ] && EXE=$EXE.exe
B=${DC2TK:-$EXE}
GOLDEN=${GOLDEN_DIR:-$ROOT/golden}
ISO=$GOLDEN/packed.iso
MODS=$GOLDEN/mods
[ -f "$B" ] || { echo "no binary at $B: cargo build --release in src/ first" >&2; exit 1; }
[ -d extracted/disc ] || { echo "no extracted/disc: run dc2tk setup or dc2tk extract first" >&2; exit 1; }
mkdir -p "$GOLDEN"
if [ ! -f "$ISO" ]; then
  echo "== packing the retail image $ISO" >&2
  "$B" pack extracted/disc -o "$ISO" >&2 || exit 1
fi
if [ ! -d "$MODS" ] && [ -d mods ]; then
  echo "== copying mods/ to $MODS" >&2
  cp -r mods "$MODS"
fi
if [ -n "${GOLDEN_MODS:-}" ]; then
  read -r -a MOD_NAMES <<< "$GOLDEN_MODS"
else
  MOD_NAMES=()
  for dir in "$MODS"/*/; do [ -f "$dir/mod.json" ] && MOD_NAMES+=("$(basename "$dir")"); done
fi

rm -rf "$OUT"; mkdir -p "$OUT/logs"
log() { echo "== $*" >&2; }
# run NAME CMD...: stdout and stderr to logs/NAME.txt, the exit code appended
run() { local name=$1; shift; log "$name"; "$@" > "$OUT/logs/$name.txt" 2>&1; echo "exit $?" >> "$OUT/logs/$name.txt"; }
# hash_iso ISO NAME: the image's sha1 to logs/NAME.sha1, then the image goes
hash_iso() {
  if [ -f "$1" ]; then sha1sum "$1" | cut -d' ' -f1 > "$OUT/logs/$2.sha1"; rm -f "$1"
  else echo missing > "$OUT/logs/$2.sha1"; fi
}

run pack "$B" pack extracted/disc -o "$OUT/packed.iso"
hash_iso "$OUT/packed.iso" pack
run decompile "$B" decompile extracted/disc -o "$OUT/decompiled"
run verify "$B" verify extracted/disc
run assets "$B" assets extracted/disc -o "$OUT/assets" --force
run maps "$B" maps "$OUT/decompiled"
run flags "$B" flags "$OUT/decompiled"
run map_m01 "$B" map "$OUT/decompiled" m01
run map_s17 "$B" map "$OUT/decompiled" s17
run xref_map "$B" xref "$OUT/decompiled" map s51
run xref_monster "$B" xref "$OUT/decompiled" monster 1
run mod_list "$B" mod list "$OUT/decompiled" --mods "$MODS"
for m in "${MOD_NAMES[@]}"; do
  run "check_$m" "$B" mod check "$OUT/decompiled" "$m" --mods "$MODS"
  run "test_$m" "$B" mod test "$OUT/decompiled" "$m" --mods "$MODS"
  run "build_$m" "$B" build "$OUT/decompiled" "$ISO" -o "$OUT/$m.iso" --mod "$m" --mods "$MODS"
  hash_iso "$OUT/$m.iso" "build_$m"
done
run build_full "$B" build "$OUT/decompiled" "$ISO" -o "$OUT/full.iso" --full
hash_iso "$OUT/full.iso" build_full

# in the logs the output folder's name (either slash) becomes <out> and timings <t>, so the logs
# of two runs compare
perl "$ROOT/tools/normalize.pl" "$OUT" "$OUT"/logs/*.txt
( cd "$OUT" && find . -type f ! -name hashes.txt ! -name diff.txt | LC_ALL=C sort | tr '\n' '\0' | xargs -0 sha1sum ) \
  > "$OUT/hashes.txt"
echo "files: $(wc -l < "$OUT/hashes.txt")" >&2
[ -n "$BASE" ] || exit 0

# the hard limit first: every disc image as the baseline's
images=0
for sha in "$BASE"/logs/*.sha1; do
  name=$(basename "$sha")
  if ! cmp -s "$sha" "$OUT/logs/$name"; then echo "DISC IMAGE CHANGED: ${name%.sha1}" >&2; images=1; fi
done
[ $images = 0 ] && echo "disc images: all identical to $BASE" >&2
# sha1sum marks binary-mode hashes with '*': compare the hash and the path only
if diff <(cut -c1-40,42- "$BASE/hashes.txt") <(cut -c1-40,42- "$OUT/hashes.txt") > "$OUT/diff.txt"; then
  echo "IDENTICAL to $BASE" >&2
else
  echo "DIFFERS from $BASE: $(grep -c '^[<>]' "$OUT/diff.txt") lines, see $OUT/diff.txt" >&2
fi
exit $images

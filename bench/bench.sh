#!/usr/bin/env bash
# bench.sh: compare delta tools on old/new pairs.
#
#   bench/bench.sh CORPUS_DIR [OUT_DIR]
#
# CORPUS_DIR holds one subdirectory per pair, each with files `old` and `new`
# (see bench/synth.py). For every tool found on PATH this measures patch size,
# encode and decode wall time, and peak RSS, and verifies the round trip
# byte for byte. Results: OUT_DIR/results.csv and OUT_DIR/results.md
# (default OUT_DIR: bench/results/<timestamp>).
#
# Tools: xdelta3, bsdiff/bspatch, zstd --patch-from, and graft once it
# has `diff`/`apply` (M1). Set GRAFT=path/to/graft to choose the binary.

set -euo pipefail

corpus=${1:?usage: bench.sh CORPUS_DIR [OUT_DIR]}
here=$(cd "$(dirname "$0")" && pwd)
out=${2:-$here/results/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out"
work=$(mktemp -d "${TMPDIR:-/tmp}/graft-bench.XXXXXX")
trap 'rm -rf "$work"' EXIT

GRAFT=${GRAFT:-$here/../build/release/tools/graft}

# --- measurement -------------------------------------------------------------
# measure CMD...: run CMD, set $secs (wall) and $rss (peak bytes).
if [[ $(uname) == Darwin ]]; then
  measure() {
    local log=$work/time.log
    /usr/bin/time -l bash -c '"$0" "$@"' "$@" 2>"$log" >/dev/null || { cat "$log" >&2; return 1; }
    secs=$(awk '/ real /{print $1; exit}' "$log")
    rss=$(awk '/maximum resident set size/{print $1; exit}' "$log")
  }
else
  measure() {
    local log=$work/time.log
    /usr/bin/time -f '%e %M' bash -c '"$0" "$@"' "$@" 2>"$log" >/dev/null || { cat "$log" >&2; return 1; }
    read -r secs kb < <(tail -1 "$log")
    rss=$((kb * 1024))
  }
fi

# --- tools: encode OLD NEW PATCH / decode OLD PATCH OUT ------------------------
tools=()
have() { command -v "$1" >/dev/null 2>&1; }

if have xdelta3; then
  tools+=(xdelta3)
  enc_xdelta3() { xdelta3 -e -9 -f -s "$1" "$2" "$3"; }
  dec_xdelta3() { xdelta3 -d -f -s "$1" "$2" "$3"; }
fi
if have bsdiff && have bspatch; then
  tools+=(bsdiff)
  enc_bsdiff() { bsdiff "$1" "$2" "$3"; }
  dec_bsdiff() { bspatch "$1" "$3" "$2"; }
fi
if have zstd; then
  tools+=(zstd)
  # --long must cover the whole source; size it from the old file.
  zlong() { local n; n=$(wc -c <"$1"); local w=10; while (( (1 << w) < n && w < 31 )); do ((w++)); done; echo $w; }
  enc_zstd() { zstd -q -f -19 --long="$(zlong "$1")" --patch-from="$1" "$2" -o "$3"; }
  dec_zstd() { zstd -q -f -d --long=31 --patch-from="$1" "$2" -o "$3"; }
fi
if [[ -x $GRAFT ]] && "$GRAFT" help 2>/dev/null | grep -q '^  diff'; then
  tools+=(graft)
  enc_graft() { "$GRAFT" diff "$1" "$2" "$3"; }
  dec_graft() { "$GRAFT" apply "$1" "$2" "$3"; }
fi
# measure runs the enc_/dec_ functions through `bash -c`, so export them.
export GRAFT
export -f $(compgen -A function | grep -E '^(enc_|dec_|zlong$)')

(( ${#tools[@]} )) || { echo "bench.sh: no delta tools found" >&2; exit 1; }

# --- run ---------------------------------------------------------------------
csv=$out/results.csv
echo "pair,tool,old_bytes,new_bytes,patch_bytes,ratio_pct,enc_s,enc_rss,dec_s,dec_rss,ok" >"$csv"

for dir in "$corpus"/*/; do
  pair=$(basename "$dir")
  old=$dir/old new=$dir/new
  [[ -f $old && -f $new ]] || continue
  oldb=$(wc -c <"$old" | tr -d ' ')
  newb=$(wc -c <"$new" | tr -d ' ')
  for t in "${tools[@]}"; do
    p=$work/$pair.$t.patch r=$work/$pair.$t.out
    ok=yes enc_s=- enc_rss=- dec_s=- dec_rss=- pb=-
    if measure "enc_$t" "$old" "$new" "$p"; then
      enc_s=$secs enc_rss=$rss pb=$(wc -c <"$p" | tr -d ' ')
      if measure "dec_$t" "$old" "$p" "$r"; then
        dec_s=$secs dec_rss=$rss
        cmp -s "$new" "$r" || ok=MISMATCH
      else ok=DECODE_FAIL; fi
    else ok=ENCODE_FAIL; fi
    ratio=-
    [[ $pb != - ]] && ratio=$(awk -v p="$pb" -v n="$newb" 'BEGIN{printf "%.3f", n ? 100*p/n : 0}')
    echo "$pair,$t,$oldb,$newb,$pb,$ratio,$enc_s,$enc_rss,$dec_s,$dec_rss,$ok" >>"$csv"
    printf '%-12s %-8s patch=%-10s (%s%%)  enc=%ss  dec=%ss  %s\n' "$pair" "$t" "$pb" "$ratio" "$enc_s" "$dec_s" "$ok"
  done
done

# --- markdown summary --------------------------------------------------------
awk -F, 'NR==1{next}
  function mb(b){ return b=="-" ? "-" : sprintf("%.1f", b/1048576) }
  { rows[NR]=sprintf("| %s | %s | %s | %s%% | %s | %s | %s | %s | %s |", $1,$2,$5,$6,$7,mb($8),$9,mb($10),$11) }
  END {
    print "| pair | tool | patch bytes | of new | enc s | enc MB | dec s | dec MB | ok |"
    print "|---|---|--:|--:|--:|--:|--:|--:|---|"
    for (i=2; i<=NR; i++) print rows[i]
  }' "$csv" >"$out/results.md"

echo
echo "results: $out/results.md"

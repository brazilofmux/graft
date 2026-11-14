#!/usr/bin/env bash
# test.sh: build graft-apply for SLOW-32 and check it against patches made by
# the host graft: multi-block, a filter, masks, a wrong source, and damage.
#
#   tools/slow32/test.sh HOST_GRAFT [WORK_DIR]
#
# Uses the same environment as build.sh, plus SLOW32_RUN (the emulator;
# default $SLOW32_HOME/tools/dbt/slow32-dbt).

set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
G=${1:?usage: test.sh HOST_GRAFT [WORK_DIR]}
W=${2:-$(mktemp -d "${TMPDIR:-/tmp}/graft-s32.XXXXXX")}
mkdir -p "$W"
S32=${SLOW32_HOME:-$HOME/slow-32}
RUN=${SLOW32_RUN:-$S32/tools/dbt/slow32-dbt}
A=$("$here/build.sh" "$W/build" | tail -1)
cd "$W"

# Inputs: pseudo-random data with scattered edits, an 8080 program, a serial.
python3 -I - <<'EOF'
import random
r = random.Random(3)
a = bytes(r.randrange(256) for _ in range(300000))
b = bytearray(a)
for _ in range(200):
    p = r.randrange(len(b)); b[p:p + 3] = b'\x01\x02\x03'
b[1000:1000] = b'inserted'
open('r.old', 'wb').write(a); open('r.new', 'wb').write(bytes(b))
# 8080: a run of CALL/JP/LXI with absolute addresses, then the same shifted by 3.
code = bytearray()
for i in range(2000):
    op = (0xcd, 0xc3, 0x21, 0x3e)[i % 4]
    code += bytes([op]) + ((0x100 + 7 * i) & 0xffff).to_bytes(2, 'little') if op != 0x3e else bytes([op, i & 0xff])
shifted = bytes(code[:100]) + b'\x00\x00\x00' + bytes(code[100:])
open('z.old', 'wb').write(code); open('z.new', 'wb').write(shifted)
t = b'SERIAL:0000-0000 ' + bytes(range(256)) * 50
open('m.old', 'wb').write(t); open('m.new', 'wb').write(b'NEW' + t + b'tail')
c = bytearray(t); c[7:16] = b'ABCD-9999'; open('m.inst', 'wb').write(bytes(c))
n = b'NEW' + t + b'tail'; i = n.find(b'SERIAL:') + 7
open('m.want', 'wb').write(n[:i] + b'ABCD-9999' + n[i + 9:])
EOF

"$G" diff -l 9 -b 65536 r.old r.new r.gft
"$G" diff -f z80 z.old z.new z.gft
"$G" diff --mask "marker=SERIAL:,len=9" m.old m.new m.gft 2>/dev/null

fail=0
check() {  # name old patch want
  rm -f out
  if "$RUN" "$A" "$2" "$3" out >log 2>&1 && cmp -s out "$4"; then echo "ok   $1"
  else echo "FAIL $1"; cat log; fail=1; fi
}
reject() {  # name args...
  local name=$1; shift
  rm -f out out.graft-tmp
  if "$RUN" "$A" "$@" >log 2>&1 || [[ -e out || -e out.graft-tmp ]]; then echo "FAIL $name"; cat log; fail=1
  else echo "ok   $name"; fi
}
check "multi-block patch" r.old r.gft r.new
check "Z80 filter" z.old z.gft z.new
check "masked serial" m.inst m.gft m.want
reject "wrong source rejected" r.new r.gft out
python3 -I -c "b=bytearray(open('r.gft','rb').read()); b[len(b)//2]^=1; open('bad.gft','wb').write(b)"
reject "damaged patch rejected" r.old bad.gft out
exit $fail

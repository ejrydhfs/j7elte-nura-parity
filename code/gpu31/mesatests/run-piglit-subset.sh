#!/bin/sh
# run-piglit-subset.sh — replay the gpu31 piglit baseline (llvmpipe today,
# Mali/panfrost hardware later). Compares against results-llvmpipe-20261119.txt.
#
# Device usage (one flock hold < 120 s per chunk; big logs live in /data):
#   sh /data/mesatests/run-piglit-subset.sh            # full 60-test subset
#   sh /data/mesatests/run-piglit-subset.sh compare    # diff vs llvmpipe baseline
#
# Repo copy lives at code/gpu31/mesatests/; deploy with:
#   sh /tmp/devput.sh code/gpu31/mesatests/run-piglit-subset.sh /data/mesatests/run-piglit-subset.sh
#
# Env this script sets (the exact env the baseline was captured with):
#   PYTHONPATH=/data/pigroot/usr/lib/python3.14/site-packages:/data/pigroot/usr/lib/piglit
#   LD_LIBRARY_PATH=/data/pigroot/usr/lib/piglit/lib:/data/pigroot/usr/lib
#     (the /piglit/lib entry is REQUIRED: libpiglitutil*.so live there,
#      NOT on the default path; without it every test dies rc=127)
#   PIGLIT_SOURCE_DIR=/data/pigroot/usr/lib/piglit
#   DISPLAY=:0  LIBGL_ALWAYS_SOFTWARE=1
# NEVER run as the user ssh account for writes: /data/mesatests is root-owned;
# run via rootshell (see /tmp/dev110.sh) or the chunk.sh wrapper.
# POSIX sh only.
set -u

OUT=${OUT:-/data/mesatests}
SUBSET=${SUBSET:-$OUT/piglit-subset.txt}
BASE=${BASE:-$OUT/baseline-llvmpipe-20261119/results-table.txt}
NAME=${NAME:-gpu31-$(date -u +%Y%m%d)}
RESULTS=${RESULTS:-$OUT/piglit-results}

export PYTHONPATH=/data/pigroot/usr/lib/python3.14/site-packages:/data/pigroot/usr/lib/piglit
export LD_LIBRARY_PATH=/data/pigroot/usr/lib/piglit/lib:/data/pigroot/usr/lib
export PIGLIT_SOURCE_DIR=/data/pigroot/usr/lib/piglit
export DISPLAY=${DISPLAY:-:0}
export LIBGL_ALWAYS_SOFTWARE=1

usage() { echo "usage: $0 [run|compare] (default: run)"; exit 2; }

cmd=${1:-run}
case "$cmd" in
  run)
    [ -f "$SUBSET" ] || { echo "missing subset list: $SUBSET"; exit 2; }
    rm -rf "$RESULTS"
    timeout 115 python3 /data/pigroot/usr/bin/piglit run quick_gl "$RESULTS" \
      -1 -p glx --test-list "$SUBSET" -n "$NAME" 2>&1 | tail -6
    echo "run rc=$? (expect 0; >60 tests need a second pass, see resume below)"
    ;;
  resume-restart)
    # piglit `resume` needs the original profile, not a test-list; instead
    # re-run ONLY the tests missing from $RESULTS/tests/ via a remainder list.
    python3 -c "
import json,glob
done={}
for f in glob.glob('$RESULTS/tests/*.json'): done.update(json.load(open(f)))
sub=[l.strip() for l in open('$SUBSET') if l.strip()]
missing=[t for t in sub if t not in done]
open('$OUT/piglit-remaining.txt','w').write('\n'.join(missing)+'\n')
print('done:',len(done),'missing:',len(missing))
"
    ;;
  compare)
    [ -f "$BASE" ] || { echo "missing baseline table: $BASE"; exit 2; }
    python3 - "$BASE" "$RESULTS" <<'EOF'
import json,glob,bz2,os,sys
base={}
for line in open(sys.argv[1]):
    line=line.strip()
    if not line: continue
    r,_,t=line.partition(' | ')
    base[t.strip()]=r.strip()
cur={}
shards=glob.glob(os.path.join(sys.argv[2],'tests','*.json'))
if shards:
    for f in shards: cur.update(json.load(open(f)))
else:
    p=os.path.join(sys.argv[2],'results.json.bz2')
    cur=dict((k,v.get('result')) for k,v in json.load(bz2.open(p))['tests'].items())
print('baseline=%d current=%d' % (len(base),len(cur)))
regr=fixed=new=0
for t,r in sorted(cur.items()):
    b=base.get(t,'<new>')
    flag=''
    if b!=r:
        if b=='pass' and r!='pass': flag='  <-- REGRESSION'; regr+=1
        elif r=='pass' and b!='pass': flag='  <-- FIXED'; fixed+=1
        else: flag='  <-- CHANGED'; new+=1
    print('%-6s %-6s %s%s' % (b,r,t,flag))
print('regressions=%d fixed=%d other-changes=%d' % (regr,fixed,new))
EOF
    ;;
  *) usage ;;
esac

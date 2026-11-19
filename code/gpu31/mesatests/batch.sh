#!/bin/sh
# Background batch: piglit subset (quick_gl, glx, serial) + glmark2 sweeps on llvmpipe.
# Launched once via rootshell nohup; polls via short flock holds. No reboot/flash.
OUT=/data/mesatests
export PYTHONPATH=/data/pigroot/usr/lib/python3.14/site-packages:/data/pigroot/usr/lib/piglit
export LD_LIBRARY_PATH=/data/pigroot/usr/lib
export DISPLAY=:0
export LIBGL_ALWAYS_SOFTWARE=1
{
echo "batch start $(date -u +%FT%TZ)"
echo "--- piglit subset (quick_gl, glx, -1) ---"
rm -rf $OUT/piglit-results
python3 /data/pigroot/usr/bin/piglit run quick_gl $OUT/piglit-results \
  -1 -p glx --test-list $OUT/piglit-subset.txt -n gpu31-llvmpipe
echo "piglit rc=$? $(date -u +%FT%TZ)"
python3 /data/pigroot/usr/bin/piglit summary console $OUT/piglit-results \
  > $OUT/piglit-summary.txt 2>&1
tail -6 $OUT/piglit-summary.txt
echo "--- glmark2-es2 ---"
timeout 300 glmark2-es2 -s 320x240 -b :duration=2.0 --results fps \
  > $OUT/glmark2-es2.log 2>&1
echo "es2 rc=$? $(date -u +%FT%TZ)"
echo "--- glmark2 (glx) ---"
timeout 300 glmark2 -s 320x240 -b :duration=2.0 --results fps \
  > $OUT/glmark2.log 2>&1
echo "glmark2 rc=$? $(date -u +%FT%TZ)"
echo "batch done $(date -u +%FT%TZ)"
} >> $OUT/batch.log 2>&1 &
echo "launched pid $!"

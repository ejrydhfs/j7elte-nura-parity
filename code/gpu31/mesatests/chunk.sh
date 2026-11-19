#!/bin/sh
# gpu31-mesatests background batch — chunked, reboot-resilient. REVISION 2:
#  LD_LIBRARY_PATH includes /data/pigroot/usr/lib/piglit/lib (util libs),
#  repaired waffle .so copied in, PIGLIT_SOURCE_DIR set so shader sources resolve.
# Each chunk is one short device command (one flock hold < 120 s).
OUT=/data/mesatests
export PYTHONPATH=/data/pigroot/usr/lib/python3.14/site-packages:/data/pigroot/usr/lib/piglit
export LD_LIBRARY_PATH=/data/pigroot/usr/lib/piglit/lib:/data/pigroot/usr/lib
export PIGLIT_SOURCE_DIR=/data/pigroot/usr/lib/piglit
export DISPLAY=:0
export LIBGL_ALWAYS_SOFTWARE=1
case "$1" in
  piglit)
    rm -rf $OUT/piglit-results
    : > $OUT/piglit-run.log
    timeout 100 python3 /data/pigroot/usr/bin/piglit run quick_gl $OUT/piglit-results \
      -1 -p glx --test-list $OUT/piglit-subset.txt -n gpu31-llvmpipe \
      >> $OUT/piglit-run.log 2>&1
    echo "piglit chunk rc=$? $(date -u +%FT%TZ)" | tee -a $OUT/batch.log
    tail -2 $OUT/piglit-run.log
    ;;
  piglit-resume)
    timeout 100 python3 /data/pigroot/usr/bin/piglit resume $OUT/piglit-results \
      -1 >> $OUT/piglit-run.log 2>&1
    echo "resume chunk rc=$? $(date -u +%FT%TZ)" | tee -a $OUT/batch.log
    tail -2 $OUT/piglit-run.log
    ;;
  summary)
    python3 /data/pigroot/usr/bin/piglit summary console $OUT/piglit-results \
      > $OUT/piglit-summary.txt 2>&1
    tail -8 $OUT/piglit-summary.txt
    ;;
  es2)
    timeout 100 glmark2-es2 -s 320x240 -b :duration=2.0 --results fps \
      > $OUT/glmark2-es2.log 2>&1
    echo "es2 rc=$? $(date -u +%FT%TZ)" | tee -a $OUT/batch.log
    grep -cE 'FPS' $OUT/glmark2-es2.log
    ;;
  glx)
    timeout 100 glmark2 -s 320x240 -b :duration=2.0 --results fps \
      > $OUT/glmark2.log 2>&1
    echo "glx rc=$? $(date -u +%FT%TZ)" | tee -a $OUT/batch.log
    grep -cE 'FPS' $OUT/glmark2.log
    ;;
  status)
    tail -6 $OUT/batch.log 2>/dev/null
    ls -la $OUT/ | head -20
    ;;
esac

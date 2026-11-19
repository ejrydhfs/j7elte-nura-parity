#!/system/bin/sh
# gpu-power-state.sh — read-only dump of G3D clocks / regulator / power-domain.
# Runs ON THE DEVICE (not the build host). Takes no arguments, writes nothing.
# Usage:  sh gpu-power-state.sh   (or: ./gpu-power-state.sh)
# Exit status is always 0 unless a shell error occurs.

echo "== G3D clocks (CLOCK_RATE trace nodes, Hz) =="
for c in aclk_g3d fout_g3d_pll mout_g3d dout_aclk_g3d aclk_g3d_400 fin_pll; do
  hit=0
  for f in /sys/kernel/debug/clk/"$c"/clk_rate \
           /sys/kernel/debug/clk/clk_"$c"/clk_rate \
           /sys/kernel/clk/"$c"/clk_rate; do
    if [ -r "$f" ]; then
      echo "$c: $(cat "$f")"
      hit=1
      break
    fi
  done
  if [ "$hit" -eq 0 ]; then
    echo "$c: (not found under /sys/kernel/debug/clk)"
  fi
done

echo ""
echo "== G3D clock enable counts =="
for f in /sys/kernel/debug/clk/aclk_g3d/clk_enable_count \
         /sys/kernel/debug/clk/aclk_g3d/clk_prepare_count; do
  if [ -r "$f" ]; then
    echo "$f: $(cat "$f")"
  fi
done

echo ""
echo "== vdd_g3d regulator (BUCK4) =="
found=0
for d in /sys/class/regulator/regulator.*; do
  if [ -r "$d/name" ] && [ "$(cat "$d/name")" = "vdd_g3d" ]; then
    found=1
    echo "node: $d"
    for n in microvolts voltage num_users use_count state status; do
      if [ -r "$d/$n" ]; then
        echo "  $n: $(cat "$d/$n")"
      fi
    done
  fi
done
if [ "$found" -eq 0 ]; then
  echo "(vdd_g3d regulator node not found; try: ls /sys/class/regulator/)"
fi

echo ""
echo "== pd-g3d power domain =="
for p in /sys/kernel/debug/pm_genpd/pd-g3d \
         /sys/kernel/debug/power-domains/pd-g3d; do
  if [ -d "$p" ]; then
    echo "node: $p"
    for n in status_state current_state; do
      if [ -r "$p/$n" ]; then
        echo "  $n: $(cat "$p/$n")"
      fi
    done
  fi
done
if [ -r /sys/kernel/debug/pm_genpd/pm_genpd_summary ]; then
  grep -i "g3d" /sys/kernel/debug/pm_genpd/pm_genpd_summary || \
    echo "(no g3d line in pm_genpd_summary)"
else
  echo "(pm_genpd_summary not available)"
fi

echo ""
echo "== kbase / gpu cooling state =="
for f in /sys/class/thermal/cooling_device*/type; do
  if [ -r "$f" ] && grep -qi "gpu\|gpufreq" "$f" 2>/dev/null; then
    dev=$(dirname "$f")
    echo "$dev type: $(cat "$f")"
    for n in cur_state max_state; do
      if [ -r "$dev/$n" ]; then
        echo "  $n: $(cat "$dev/$n")"
      fi
    done
  fi
done

exit 0

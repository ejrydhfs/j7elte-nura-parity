#!/bin/sh
# GPS baud/flow/power/parity RX matrix. Usage: sh baudmatrix.sh START COUNT
DEV=/dev/ttySAC0
PWR=/sys/class/sec/gps/GPS_PWR_EN/value
BAUDS="4800 9600 19200 38400 57600 115200 230400 460800 921600"
START=${1:-1}
COUNT=${2:-108}
n=0
echo "# start=$(date +%s) seg=$START+$COUNT pwr_before=$(cat $PWR)"
for b in $BAUDS; do
  for fl in none rtscts; do
    for pw in none t250 t2000; do
      for par in 8n1 8e1; do
        n=$((n+1))
        if [ $n -lt $START ] || [ $n -gt $((START+COUNT-1)) ]; then continue; fi
        SET="$b cs8 -cstopb raw -echo -echoe -echok -echoctl clocal cread -ixon -ixoff"
        if [ "$par" = "8e1" ]; then SET="$SET parenb -parodd"; else SET="$SET -parenb"; fi
        if [ "$fl" = "rtscts" ]; then SET="$SET crtscts"; else SET="$SET -crtscts"; fi
        if ! stty -F $DEV $SET 2>/dev/null; then
          echo "$n $b $fl $pw $par UNSUPPORTED stty_rc_fail"; continue
        fi
        # read back what the tty actually kept
        RB=$(stty -F $DEV -a 2>/dev/null | tr -s ' ')
        case "$RB" in *" parenb"*) PAROK=E;; *) PAROK=N;; esac
        case "$RB" in *" crtscts"*) FLOWOK=C;; *) FLOWOK=N;; esac
        case "$RB" in *" cs8"*) CSOK=8;; *) CSOK=x;; esac
        case "$pw" in
          t250) echo 0 > $PWR; sleep 0.25; echo 1 > $PWR; sleep 0.25 ;;
          t2000) echo 0 > $PWR; sleep 2; echo 1 > $PWR; sleep 0.25 ;;
        esac
        H=$( { timeout 2.2 dd if=$DEV bs=1 count=2048 2>/dev/null; } 2>/dev/null | od -An -v -tx1)
        H=$(echo $H)
        set -- $H
        echo "$n $b $fl $pw $par pwr=$(cat $PWR) applied=$CSOK$PAROK$FLOWOK bytes=$# hex=${H:-none}"
      done
    done
  done
done
echo 1 > $PWR
echo "# done=$(date +%s) pwr_after=$(cat $PWR)"

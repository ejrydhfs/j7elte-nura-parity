#!/bin/sh
# /tmp/mesa203-watch.sh - keep the on-device Mesa 20.3.5 build moving across
# device reboots, exit when ninja has no work left.
SSH="ssh -o StrictHostKeyChecking=no -o PreferredAuthentications=password -o PubkeyAuthentication=no -o ConnectTimeout=10 user@172.16.42.1"
export SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force

while :; do
	out=$(setsid -w flock /tmp/gpsdev.lock $SSH '
		cd /data/mesa203-build || exit 7
		if ! pgrep -x ninja >/dev/null 2>&1; then
			if ninja -C build -n 2>/dev/null | grep -q "no work to do"; then
				echo DONE; exit 0
			fi
			nohup setsid nice -n 10 ninja -C build -j2 >> build.log 2>&1 < /dev/null &
			sleep 3
			echo "RESTARTED $(grep -c Compiling build.log)"
		else
			echo "RUNNING $(grep -c Compiling build.log)"
		fi
		tail -1 build.log
	' 2>&1)
	rc=$?
	echo "[$(date +%H:%M:%S)] rc=$rc $out"
	echo "$out" | grep -q DONE && [ $rc -eq 0 ] && exit 0
	sleep 45
done

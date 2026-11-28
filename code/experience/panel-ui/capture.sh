#!/bin/sh
pkill -f 'cat /dev/kmsg'
sleep 1
: > /data/live4.log
setsid nohup sh -c 'cat /dev/kmsg > /data/live4.log' >/dev/null 2>&1 </dev/null &
sleep 1
echo CAP_STARTED

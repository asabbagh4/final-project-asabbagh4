#!/bin/bash

MIN=0.0950
MAX=0.1050

# record start timestamp
START=$(date "+%b %e %H:%M:%S")

./sequencer 10

prev=""
pass=0
total=0

grep "COURSE #:4" /var/log/syslog | \
awk -v start="$START" '$0 >= start' | \
sed -n 's/.*Start Time:\([0-9.]*\).*/\1/p' | \
(while read t; do
    if [ -n "$prev" ]; then
        diff=$(awk "BEGIN{print $t-$prev}")
        total=$((total+1))

        if awk "BEGIN{exit !($diff>=$MIN && $diff<=$MAX)}"; then
            echo "Delta t @ $total: $diff sec -> PASS"
            pass=$((pass+1))
        else
            echo "Delta t @ $total: $diff sec -> FAIL"
        fi
    fi
    prev=$t
done

echo "Passed $pass / $total")

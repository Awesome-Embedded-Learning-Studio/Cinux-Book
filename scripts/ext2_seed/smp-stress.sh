set -e
i=0
while /bin/busybox test "$i" -lt 24; do
    payload="smp-round-$i"
    heard=$(/bin/busybox echo "$payload" | /bin/busybox wc -c)
    expected=$((${#payload} + 1))
    /bin/busybox test "$heard" -eq "$expected"
    listed=$(/bin/busybox ls /bin)
    case "$listed" in *busybox*smpstress*) ;; *) exit 1;; esac
    i=$((i + 1))
done
echo BUSYBOX_SMP_PASS

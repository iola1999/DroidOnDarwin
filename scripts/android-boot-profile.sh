#!/system/bin/sh
# A marker arms one boot only; the service otherwise exits without collecting.
marker=/data/local/tmp/husk-profile-next-boot
[ -f "$marker" ] || exit 0
rm "$marker" || exit 1
output=/data/local/tmp/husk-boot-profile
mkdir -p "$output" || exit 1
rm -f "$output/report.txt" "$output/report-threads.txt"
exec >"$output/collector.log" 2>&1
date
cat /proc/uptime
cat /proc/meminfo > "$output/memory-before.txt"
cat /proc/stat > "$output/stat-before.txt"
ps -A -T -o PID,TID,USER,STAT,WCHAN,NAME > "$output/threads-before.txt"
old_kptr=$(cat /proc/sys/kernel/kptr_restrict)
trap 'echo "$old_kptr" > /proc/sys/kernel/kptr_restrict' EXIT
echo 0 > /proc/sys/kernel/kptr_restrict
cat /proc/kallsyms > "$output/kallsyms.txt"
simpleperf record -a -e cpu-clock -f 49 --duration 180 --size-limit 16M --no-dump-symbols --no-dump-build-id -o "$output/perf.data"
result=$?
echo "record_exit=$result"
# Generate reports after the measured run so ELF parsing does not compete with boot.
cat /proc/uptime
cat /proc/meminfo > "$output/memory-after.txt"
cat /proc/stat > "$output/stat-after.txt"
ps -A -T -o PID,TID,USER,STAT,WCHAN,NAME > "$output/threads-after.txt"
sync
echo completed

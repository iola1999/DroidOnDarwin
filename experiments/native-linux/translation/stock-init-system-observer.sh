#!/system/bin/sh

umask 077
PATH=/system/bin
export PATH

timeout=/system/bin/timeout
command_seconds=5
kill_after_seconds=1

boot_id=$("$timeout" -k "$kill_after_seconds" "$command_seconds" /system/bin/cat /proc/sys/kernel/random/boot_id)
boot_id_rc=$?
if [ "$boot_id_rc" -ne 0 ]; then
    printf 'observer_start_error\tboot_id\t%d\n' "$boot_id_rc" >&2
    exit 1
fi
case "$boot_id" in
    ''|*[!0-9a-f-]*)
        printf 'observer_start_error\tinvalid_boot_id\n' >&2
        exit 1
        ;;
esac
case "$boot_id" in
    ????????-????-????-????-????????????) ;;
    *)
        printf 'observer_start_error\tinvalid_boot_id\n' >&2
        exit 1
        ;;
esac

output=/data/local/tests/system/$boot_id
"$timeout" -k "$kill_after_seconds" "$command_seconds" /system/bin/mkdir "$output"
mkdir_rc=$?
if [ "$mkdir_rc" -ne 0 ]; then
    printf 'observer_start_error\tmkdir\t%d\n' "$mkdir_rc" >&2
    exit 1
fi

# 同一 boot_id 的重复启动保留已有记录；中断记录使用 incomplete 文件名。
incomplete=$output/result.incomplete.tsv
exec 3>"$incomplete" || exit 1
printf 'format\tdroid_plain_system_observer_v1\n' >&3
printf 'kernel_boot_id\t%s\n' "$boot_id" >&3
printf 'command_seconds\t%d\nkill_after_seconds\t%d\n' "$command_seconds" "$kill_after_seconds" >&3
printf 'observer_begin\n' >&3
trap 'printf "observer_end\tinterrupted\n" >&3; exit 1' HUP INT TERM

checks=0
failures=0

run_query() {
    check_name=$1
    shift
    stdout=$output/$check_name.stdout
    stderr=$output/$check_name.stderr
    printf 'command\t%s\t%s\n' "$check_name" "$*" >&3
    "$timeout" -k "$kill_after_seconds" "$command_seconds" "$@" </dev/null >"$stdout" 2>"$stderr" 3>&-
    query_rc=$?
    first_line=
    IFS= read -r first_line <"$stdout" || :
}

record_query() {
    checks=$((checks + 1))
    if [ "$query_rc" -eq 0 ] && [ "$1" = true ]; then
        outcome=passed
    else
        outcome=failed
        failures=$((failures + 1))
    fi
    printf 'check\t%s\t%d\t%s\t%s\t%s\n' "$check_name" "$query_rc" "$outcome" "$check_name.stdout" "$check_name.stderr" >&3
}

run_query boot_completed_begin /system/bin/getprop sys.boot_completed
valid=false
[ "$first_line" = 1 ] && valid=true
record_query "$valid"

for init_service in zygote surfaceflinger; do
    run_query "${init_service}_state" /system/bin/getprop "init.svc.$init_service"
    valid=false
    [ "$first_line" = running ] && valid=true
    record_query "$valid"
done

for process in zygote64 system_server surfaceflinger; do
    run_query "${process}_pid" /system/bin/pidof "$process"
    valid=false
    case "$first_line" in
        ''|*[!0-9\ ]*) ;;
        *) valid=true ;;
    esac
    record_query "$valid"
done

for binder_service in activity package window input lock_settings SurfaceFlinger; do
    run_query "binder_$binder_service" /system/bin/service check "$binder_service"
    valid=false
    # 原 service check 在未找到服务的情况下也可能返回 0。
    [ "$first_line" = "Service $binder_service: found" ] && valid=true
    record_query "$valid"
done

run_query cmd_activity /system/bin/cmd activity get-current-user
valid=false
case "$first_line" in
    ''|*[!0-9]*) ;;
    *) valid=true ;;
esac
record_query "$valid"

run_query cmd_package /system/bin/cmd package path android
valid=false
case "$first_line" in
    package:/*) valid=true ;;
esac
record_query "$valid"

run_query cmd_window /system/bin/cmd window size
valid=false
case "$first_line" in
    'Physical size: '*x*) valid=true ;;
esac
record_query "$valid"

# help 检查 input 的服务命令接口；输入事件处理需要后续验证。
run_query cmd_input /system/bin/cmd input help
valid=false
case "$first_line" in
    'Usage: input '*) valid=true ;;
esac
record_query "$valid"

run_query cmd_lock_settings /system/bin/cmd lock_settings get-disabled --user 0
valid=false
case "$first_line" in
    true|false) valid=true ;;
esac
record_query "$valid"

run_query boot_id_end /system/bin/cat /proc/sys/kernel/random/boot_id
valid=false
[ "$first_line" = "$boot_id" ] && valid=true
record_query "$valid"

run_query boot_completed_end /system/bin/getprop sys.boot_completed
valid=false
[ "$first_line" = 1 ] && valid=true
record_query "$valid"

if [ "$failures" -eq 0 ]; then
    result=passed
else
    result=failed
fi
printf 'observer_end\tcomplete\t%s\t%d\t%d\n' "$result" "$checks" "$failures" >&3

# 完成文件要求所有查询结束且最后一次 sync 成功。
"$timeout" -k "$kill_after_seconds" "$command_seconds" /system/bin/sync </dev/null >/dev/null 2>"$output/finalize-sync.stderr" 3>&-
sync_rc=$?
if [ "$sync_rc" -ne 0 ]; then
    printf 'finalization_error\tsync\t%d\n' "$sync_rc" >&3
    exit 1
fi
exec 3>&-
trap - HUP INT TERM
"$timeout" -k "$kill_after_seconds" "$command_seconds" /system/bin/mv "$incomplete" "$output/result.$result.tsv" </dev/null >/dev/null 2>"$output/finalize-mv.stderr"
mv_rc=$?
if [ "$mv_rc" -ne 0 ]; then
    printf 'observer_finalize_error\tmv\t%d\n' "$mv_rc" >&2
    exit 1
fi
[ "$failures" -eq 0 ]

set -eu
PATH=/bin:/
export PATH

printf 'alpha\nbeta\n' > /shell.input
result=$(while IFS= read -r line; do printf '[%s]' "$line"; done < /shell.input)
test "$result" = '[alpha][beta]'
result=$(printf 'one\ntwo\n' | while IFS= read -r line; do printf '%s:' "$line"; done)
test "$result" = 'one:two:'
read -r line <<EOF
here document
EOF
test "$line" = 'here document'
printf '%s\n' '{"test":"linux_dash_pipeline_redirection","passed":true}'

value=parent
result=$(value=child; printf '%s' "$value")
test "$value:$result" = 'parent:child'
compute() { total=0; for n in 1 2 3 4 5; do total=$((total+n)); done; printf '%s' "$total"; }
test "$(compute)" = 15
test "$(sh -c 'printf "%s" "$1"' child argument)" = argument
case "$(/worker check)" in *'worker passed'*) ;; *) exit 91;; esac
printf '%s\n' '{"test":"linux_dash_fork_exec_environment","passed":true}'

caught=0
trap 'caught=$((caught+1))' USR1
kill -USR1 $$
test "$caught" = 1
trap - USR1
(exit 23) &
child=$!
status=0
wait "$child" || status=$?
test "$status" = 23
sh -c 'kill -TERM $$; exit 92' &
child=$!
status=0
wait "$child" || status=$?
test "$status" = 143
printf '%s\n' '{"test":"linux_dash_trap_background_wait","passed":true}'

printf '#!/bin/sh\nprintf "script:%%s" "$1"\n' > /shell.child
test "$(sh /shell.child argument)" = script:argument
printf '%s\n' '{"test":"linux_dash_shell_suite","passed":true}'

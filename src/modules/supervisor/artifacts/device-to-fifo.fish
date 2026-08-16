# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set fifo_path $argv[1]
set pid_file $argv[2]
set stop_file $argv[3]
mkfifo --mode=600 -- $fifo_path
set create_status $status
if test $create_status -ne 0
    rm -f -- $pid_file $stop_file
    exit $create_status
end
set -l fish (status fish-path)
$fish --no-config -c 'exec cat >$argv[1]' -- $fifo_path &
set relay_pid $last_pid
set --global relay_exit_code 1
function record_relay_exit --on-process-exit $relay_pid
    set --global relay_exit_code $argv[3]
end
echo $relay_pid >$pid_file
if test $status -ne 0
    kill -TERM $relay_pid
else if test -e $stop_file
    kill -TERM $relay_pid
else
    echo -n R || kill -TERM $relay_pid
end
wait $relay_pid
rm -f -- $pid_file $stop_file $fifo_path
set cleanup_status $status
if test $relay_exit_code -ne 0
    exit $relay_exit_code
end
exit $cleanup_status

#!/usr/bin/env -S fish --no-config
# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Test that the Fish startup code for `--run-fish-program` gives the supplied
# program its command-line arguments unchanged, makes the PBS configuration
# available in variables, and leaves the encryption key for the program to read
# from standard input. Also check that Fish returns the exit status specified
# by the supplied program.
#
# The test runs the Fish code directly with dummy configuration and credentials,
# so it can run without a configured DeviceFs installation or a PBS server.

set -l bootstrap (path resolve (status dirname)/../src/modules/supervisor/artifacts/start-pbs.fish)
set -l directory (mktemp -d -t devicefs-fish-program.XXXXXXXXXX) || exit
function remove_test_files --on-event fish_exit --inherit-variable directory
    rm -rf -- $directory
end

set -l program '
test (count $argv) -eq 5 || exit 1
test "$argv[1]" = --list-backups || exit 1
test -z "$argv[2]" || exit 1
test "$argv[3]" = "two words" || exit 1
test "$argv[4]" = λ || exit 1
test "$argv[5]" = \'literal $HOME; * "quote"\' || exit 1
if set --query _flag_list_backups
    echo "FAIL: the bootstrap parsed the supplied program argument as its own option." 1>&2
    exit 1
end
echo "PASS: the supplied program received its arguments, including an empty argument and literal shell syntax."

test "$PBS_SERVER" = test-server || exit 1
test "$PBS_PASSWORD" = test-password || exit 1
test "$DEVICEFS_MANIFEST" = test-manifest || exit 1
test "$DEVICEFS_RPC_PASSWORD" = test-rpc-password || exit 1
test "$backup_id" = test-host || exit 1
test "$_flag_parallel_images" = --parallel-images || exit 1
read --local key
test "$key" = test-key-document || exit 1
echo "PASS: the bootstrap consumed the configuration records and left the key document for the supplied program."

exit 37
'

set -l fish (status fish-path)
begin
    cat $bootstrap
    printf '\n%s\0' $program
    printf '%s\0' test-client test-server 8007 test-store test-user test-namespace \
        test-fingerprint test-password test-manifest test-rpc-password
    printf %s test-key-document
end | $fish --no-config -c 'read --null --global DEVICEFS_FISH_PROGRAM && eval $DEVICEFS_FISH_PROGRAM' \
    -- $directory/pid $directory/stop test-host --parallel-images --run-fish-program \
    -- --list-backups '' 'two words' λ 'literal $HOME; * "quote"'
set -l result $pipestatus
if test $result[1] -ne 0 || test $result[2] -ne 37
    printf 'FAIL: the input writer returned %s and Fish returned %s; expected 0 and 37.\n' $result 1>&2
    exit 1
end
echo 'PASS: Fish returned the exit status selected by the supplied program.'

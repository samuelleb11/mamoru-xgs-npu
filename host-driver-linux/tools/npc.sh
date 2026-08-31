#!/bin/sh
# SPDX-License-Identifier: MIT
# NPU console driver on the x86 host: pipe a command (stdin) to ttyS2 (the NPU's
# serial console) and print whatever the NPU replies within W seconds (default 3).
stty -F /dev/ttyS2 115200 raw -echo 2>/dev/null
: > /tmp/npout
cat /dev/ttyS2 > /tmp/npout 2>&1 &
CP=$!
sleep 0.4
{ cat; printf '\r\n'; } > /dev/ttyS2
sleep "${W:-3}"
kill "$CP" 2>/dev/null
tr -d '\000' < /tmp/npout

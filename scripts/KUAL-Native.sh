#!/bin/sh
# Name: KUAL Native
# Author: KUAL Native contributors
#
# Copy this scriptlet to documents using the post-jailbreak mechanism that
# executes .sh document items. The ARM binary lives in the extension package.
LOG=/var/tmp/kual-native.log
nohup /mnt/us/extensions/KUAL/bin/kual-native >"$LOG" 2>&1 &
exit 0

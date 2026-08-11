#!/bin/sh
# Name: KUAL Native
# Author: KUAL Native contributors
#
# Copy this scriptlet to documents using the post-jailbreak mechanism that
# executes .sh document items. The ARM binary lives in the extension package.
LOG=/var/tmp/kual-native.log
# Do not background this process: returning to the document scriptlet makes
# the framework repaint its page over KUAL's e-ink UI.  Replacing this shell
# keeps the native menu in control until the user chooses × Quit.
exec /mnt/us/extensions/KUAL/bin/kual-native >"$LOG" 2>&1

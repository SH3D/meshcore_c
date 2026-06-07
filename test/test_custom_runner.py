# PlatformIO custom test runner for the host "native" environment.
#
# test_codec.c is a plain C program with its own main() that prints results and
# returns a non-zero exit code if any check fails. The "custom" test framework
# (set in platformio.ini) means PlatformIO does NOT inject Unity's main/runner;
# it just builds and runs our program and reports failure on a non-zero exit.
# This minimal subclass is all that's needed to wire that up.
#
#     pio test -e native
#
# SPDX-License-Identifier: MIT

from platformio.public import TestRunnerBase


class CustomTestRunner(TestRunnerBase):
    pass

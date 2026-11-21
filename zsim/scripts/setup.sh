#!/bin/bash

# Fix ptrace security restriction (requires sudo)
sudo sh -c 'echo 0 > /proc/sys/kernel/yama/ptrace_scope'

# Run the autofs-ldap setup script
sudo /share/testbed/bin/linux-autofs-ldap


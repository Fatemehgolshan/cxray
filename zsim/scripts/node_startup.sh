#!/bin/bash
#
# Custom startup script for zsim experiment nodes
# This runs on each node when it boots up
#

# Log everything to /tmp for debugging
exec > /tmp/user_zsim_startup_$(date +%Y%m%d_%H%M%S).log 2>&1

echo "=== zsim Node Startup Script ==="
echo "Start time: $(date)"
echo "Hostname: $(hostname)"
echo "User: $(whoami)"
echo ""

# First, run the standard setup (REQUIRED for disk setup)
echo "[1/4] Running linux-autofs-ldap..."
/share/testbed/bin/linux-autofs-ldap
if [ $? -eq 0 ]; then
    echo "  ✓ linux-autofs-ldap completed successfully"
else
    echo "  ✗ linux-autofs-ldap failed with exit code $?"
fi
echo ""

# Set kernel parameters for zsim/Pin
echo "[2/4] Setting kernel parameters for zsim..."
echo "  Setting kernel.yama.ptrace_scope=0"
sudo sh -c 'echo 0 > /proc/sys/kernel/yama/ptrace_scope'
if [ $? -eq 0 ]; then
    PTRACE_VALUE=$(cat /proc/sys/kernel/yama/ptrace_scope)
    echo "  ✓ ptrace_scope set to: $PTRACE_VALUE"
else
    echo "  ✗ Failed to set ptrace_scope"
fi
echo ""

# Verify zsim binary exists
echo "[3/3] Checking zsim binary..."
ZSIM_BIN="/path/to/home/repos/memtiering/zsim/build/opt/zsim"
if [ -f "$ZSIM_BIN" ]; then
    echo "  ✓ zsim binary found: $ZSIM_BIN"
    ls -lh "$ZSIM_BIN"
else
    echo "  ✗ zsim binary not found at $ZSIM_BIN"
fi
echo ""

echo "=== Startup Complete ==="
echo "End time: $(date)"
echo ""
echo "Summary:"
echo "  Hostname: $(hostname)"
echo "  ptrace_scope: $(cat /proc/sys/kernel/yama/ptrace_scope)"
echo "  zsim binary: $(test -f $ZSIM_BIN && echo 'exists' || echo 'missing')"
echo ""

exit 0

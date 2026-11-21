# ipc.py  (updated)
import sys, pathlib

file_path = sys.argv[1] if len(sys.argv) > 1 else "zsim.out"
if not pathlib.Path(file_path).is_file():
    sys.exit(f"File not found: {file_path}")

cycles = instrs = None
with open(file_path) as f:
    for line in f:
        if cycles is None and "cycles:" in line:
            cycles = int(line.split()[1])
        elif instrs is None and "instrs:" in line:
            instrs = int(line.split()[1])
        if cycles and instrs:
            break

print(f"IPC = {instrs / cycles:.3f}")



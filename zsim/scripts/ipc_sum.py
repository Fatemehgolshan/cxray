import sys
import pathlib

file_path = sys.argv[1] if len(sys.argv) > 1 else "zsim.out"
if not pathlib.Path(file_path).is_file():
    sys.exit(f"File not found: {file_path}")

total_instrs = 0
max_cycles = 0

with open(file_path) as f:
    for line in f:
        if "instrs:" in line:
            try:
                total_instrs += int(line.split()[1])
            except (IndexError, ValueError):
                pass
        elif "cycles:" in line:
            try:
                val = int(line.split()[1])
                if val > max_cycles:
                    max_cycles = val
            except (IndexError, ValueError):
                pass
if max_cycles == 0:
    sys.exit("No cycle values found in file.")

print(f"Total Instructions = {total_instrs}")
print(f"Max Cycles         = {max_cycles}")
print(f"IPC                = {total_instrs / max_cycles:.3f}")

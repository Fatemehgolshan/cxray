#!/usr/bin/env python3
"""
zsim Parameter Sweep Runner

Simple script to run parameter sweeps for zsim experiments.
Similar to MARS's mars_gen_run_cmds.py pattern.
"""

import sys
import os
import re
import subprocess
import time
import json
import argparse
from pathlib import Path
from datetime import datetime
import matplotlib.pyplot as plt
import numpy as np
import importlib.util
import threading
import h5py

# ANSI color codes for terminal output
class Colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    PINK = '\033[95m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    RESET = '\033[0m'
    BOLD = '\033[1m'

    @staticmethod
    def colorize(text, color):
        """Add color to text if stdout is a TTY"""
        if sys.stdout.isatty():
            return f"{color}{text}{Colors.RESET}"
        return text

def load_experiment_config(config_file):
    """Load experiment configuration from Python file"""
    spec = importlib.util.spec_from_file_location("config", config_file)
    config = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(config)
    return config

def substitute_sweep_name(template, values):
    """Substitute %n placeholders with actual values

    Example: "Migration=%1_Page4K" with values=["none"] -> "Migration=none_Page4K"
    Empty strings are replaced with "NA"
    """
    result = template
    for i, value in enumerate(values, 1):
        placeholder = f"%{i}"
        # Use "NA" for empty string values
        display_value = "NA" if value == "" else str(value)
        result = result.replace(placeholder, display_value)
    return result

def sanitize_name_for_filesystem(name):
    """Sanitize name for filesystem use by replacing '=' with '_'

    Example: "Migration=none_Page=4K" -> "Migration_none_Page_4K"
    """
    return name.replace('=', '_')

def parse_max_instrs_from_config(config_file):
    """Parse maxTotalInstrs from zsim config file"""
    try:
        content = config_file.read_text()
        # Look for maxTotalInstrs = <value> (with optional L suffix);
        match = re.search(r'maxTotalInstrs\s*=\s*(\d+)L?', content)
        if match:
            return int(match.group(1))
    except:
        pass
    return None

def get_simulation_progress(h5_file, max_instrs):
    """Get simulation progress from h5 file

    Returns: (current_instrs, progress_pct, display_str)
    """
    try:
        f = h5py.File(h5_file, 'r')
        stats = f['stats']['root']
        cores = stats['c']

        # Get latest instruction count
        current_instrs = np.sum(cores['instrs'][-1])
        f.close()

        if max_instrs and max_instrs > 0:
            progress_pct = (current_instrs / max_instrs) * 100
        else:
            progress_pct = 0

        # Format display string
        if current_instrs >= 1e9:
            display_str = f"{current_instrs/1e9:.1f}B"
        elif current_instrs >= 1e6:
            display_str = f"{current_instrs/1e6:.1f}M"
        else:
            display_str = f"{current_instrs/1e3:.1f}K"

        return current_instrs, progress_pct, display_str
    except:
        return 0, 0, "0"

def detect_crash_cause(exp_dir):
    """Detect crash cause from zsim.out

    Returns: (status, error_msg)
    - status: 'completed', 'killed', 'error', 'unknown'
    - error_msg: Description of the error
    """
    zsim_out = exp_dir / "zsim.out"
    if not zsim_out.exists():
        return 'unknown', 'No output file found'

    try:
        # Read last 100 lines of zsim.out for error detection
        with open(zsim_out, 'rb') as f:
            # Go to end and read last chunk
            f.seek(0, 2)
            file_size = f.tell()
            # Read last 50KB or entire file
            chunk_size = min(50000, file_size)
            f.seek(max(0, file_size - chunk_size))
            content = f.read().decode('utf-8', errors='ignore')

        # Check for successful completion
        if 'Termination condition met' in content or 'Simulation done' in content:
            return 'completed', 'Successfully completed'

        # Check for common error patterns
        error_patterns = [
            (r'Killed', 'SIGTERM/SIGKILL - likely OOM or timeout'),
            (r'Segmentation fault', 'Segmentation fault - memory corruption'),
            (r'Assertion.*failed', 'Assertion failed - internal error'),
            (r'panic:.*', 'Panic - critical error'),
            (r'ERROR:.*', 'Error in simulation'),
            (r'Exception.*', 'Exception thrown'),
            (r'Aborted', 'Aborted - possibly assertion or signal'),
            (r'out of memory', 'Out of memory'),
            (r'Cannot allocate', 'Memory allocation failed'),
        ]

        for pattern, description in error_patterns:
            match = re.search(pattern, content, re.IGNORECASE)
            if match:
                # Extract context around error
                lines = content.split('\n')
                for i, line in enumerate(lines):
                    if re.search(pattern, line, re.IGNORECASE):
                        # Get few lines around the error for context
                        start = max(0, i - 2)
                        end = min(len(lines), i + 3)
                        context = '\n'.join(lines[start:end])
                        return 'error', f"{description}\n{context}"

        # Check if simulation is still running (has heartbeat but no completion)
        if 'Simulation started' in content and 'Termination condition' not in content:
            # Check file modification time
            import time
            mod_time = zsim_out.stat().st_mtime
            age = time.time() - mod_time
            if age > 300:  # 5 minutes
                return 'killed', f'Killed (no activity for {age/60:.1f} minutes)'
            else:
                return 'running', 'Still running'

        return 'unknown', 'Unable to determine status'

    except Exception as e:
        return 'unknown', f'Error reading output: {str(e)}'

def replace_param(content, param_name, value, is_string=False):
    """Replace parameter in config file"""
    if is_string:
        pattern = rf'{param_name}\s*=\s*"[^"]*"'
        replacement = f'{param_name} = "{value}"'
    else:
        # For numeric values, use the value as-is (don't add L suffix)
        # If a value needs an L suffix, it should be included in the experiment file
        pattern = rf'{param_name}\s*=\s*[^;]+;'
        replacement = f'{param_name} = {value};'

    new_content, count = re.subn(pattern, replacement, content)
    if count == 0:
        print(f"Warning: Parameter '{param_name}' not found in config")
    return new_content

def inject_start_fast_forwarded(content, value):
    """Inject or replace startFastForwarded inside every processN block.

    When startFastForwarded=true, also inject startPaused=true to prevent
    the "process0 cannot exec()" error.

    Handles both cases:
      - parameter already present: replaces the value
      - parameter absent: inserts it as the first line inside the opening brace

    Example transformation (value="true"):
      process0 = {
        command = "...";
      };
    becomes:
      process0 = {
        startFastForwarded = true;
        startPaused = true;
        command = "...";
      };
    """
    ff_line = f'startFastForwarded = {value};'

    # When startFastForwarded=true, also set startPaused=true to avoid exec() error
    if value == "true":
        lines_to_inject = f'startFastForwarded = true;\n  startPaused = true;'
    else:
        lines_to_inject = ff_line

    # If the parameter already exists inside any processN block, replace it.
    existing_pattern = r'(process\d+\s*=\s*\{[^}]*?)startFastForwarded\s*=\s*\w+;'
    new_content, count = re.subn(
        existing_pattern,
        lambda m: m.group(1) + lines_to_inject,
        content,
        flags=re.DOTALL,
    )
    if count > 0:
        return new_content

    # Parameter not present — insert after each opening brace of processN blocks.
    def insert_after_brace(m):
        # m.group(0) is "process0 = {" (the opening)
        return m.group(0) + f'\n  {lines_to_inject}'

    new_content = re.sub(r'process\d+\s*=\s*\{', insert_after_brace, content)
    return new_content

def is_nvidia_machine():
    """Check if running on nvidia/rocky8 machine"""
    import socket
    hostname = socket.gethostname().lower()
    return 'nvidia' in hostname

def inject_omp_env_if_needed(content):
    """Inject OMP environment variables to disable thread affinity on nvidia machines.

    On nvidia/rocky8 machines, OpenMP's pthread_setaffinity_np() fails under zsim,
    so we need to disable it by setting multiple environment variables for both
    GNU OpenMP (GOMP) and Intel OpenMP (KMP).

    Example transformation:
      process0 = {
        command = "...";
      };
    becomes:
      process0 = {
        env = "OMP_PROC_BIND=false:KMP_AFFINITY=disabled:GOMP_CPU_AFFINITY=";
        command = "...";
      };
    """
    if not is_nvidia_machine():
        return content

    # Set multiple env vars to disable affinity for both GNU and Intel OpenMP
    env_line = 'env = "OMP_PROC_BIND=false:KMP_AFFINITY=disabled:GOMP_CPU_AFFINITY=";'

    # If env parameter already exists, replace it
    existing_pattern = r'(process\d+\s*=\s*\{[^}]*?)env\s*=\s*"[^"]*";'
    new_content, count = re.subn(
        existing_pattern,
        lambda m: m.group(1) + env_line,
        content,
        flags=re.DOTALL,
    )
    if count > 0:
        return new_content

    # Parameter not present — insert after each opening brace of processN blocks
    def insert_after_brace(m):
        return m.group(0) + f'\n  {env_line}'

    new_content = re.sub(r'process\d+\s*=\s*\{', insert_after_brace, content)
    return new_content

def wrap_command_with_omp_env_if_needed(workload_cmd, workload_has_roi):
    """Wrap command with OMP env vars to disable thread affinity on nvidia machines

    On nvidia/rocky8 machines, OpenMP's pthread_setaffinity_np() fails under zsim,
    so we need to disable it with OMP_PROC_BIND=false for all workloads that use
    OpenMP (particularly graph workloads).

    Args:
        workload_cmd: Either a string (single process command) or
                     a list of (command, mask) tuples (multi-process)
        workload_has_roi: Boolean indicating if workload has ROI markers (unused now)

    Returns:
        Modified workload_cmd (same type as input)
    """
    # Apply workaround on all nvidia machines for all workloads
    if not is_nvidia_machine():
        return workload_cmd

    env_prefix = "env OMP_PROC_BIND=false "

    if isinstance(workload_cmd, str):
        if not workload_cmd.startswith("env "):
            return env_prefix + workload_cmd
        return workload_cmd
    elif isinstance(workload_cmd, list):
        return [(env_prefix + cmd if not cmd.startswith("env ") else cmd, mask)
                for cmd, mask in workload_cmd]
    else:
        return workload_cmd

def generate_multiprocess_config_section(processes_list):
    """Generate config section for multiple processes

    Args:
        processes_list: List of (command, mask) tuples, e.g.:
            [("cmd1", "0x00FF"), ("cmd2", "0xFF00")]

    Returns:
        String containing individual process blocks (NOT nested in processes):
        process0 = {
            command = "cmd1";
            mask = "0x00FF";
        };
        process1 = {
            command = "cmd2";
            mask = "0xFF00";
        };
    """
    lines = []

    for i, (cmd, mask) in enumerate(processes_list):
        # Process blocks at top level (no indent)
        lines.append(f"process{i} = {{")
        lines.append(f"    command = \"{cmd}\";")
        lines.append(f"    mask = \"{mask}\";")
        lines.append(f"}};")
        if i < len(processes_list) - 1:
            lines.append("")  # Empty line between processes

    return "\n".join(lines)

def apply_workload_to_config(content, workload_cmd):
    """Apply workload (single or multi-process) to config

    Args:
        content: Base config content
        workload_cmd: Either a string (single process command) or
                     a list of (command, mask) tuples (multi-process)

    Returns:
        Modified config content
    """
    if isinstance(workload_cmd, str):
        # Single process - replace command parameter
        return replace_param(content, "command", workload_cmd, is_string=True)
    elif isinstance(workload_cmd, list):
        # Multi-process - replace ALL process0/process1/etc. blocks
        processes_section = generate_multiprocess_config_section(workload_cmd)

        # Pattern to match consecutive process blocks: process0 = { ... }; process1 = { ... }; etc.
        # This handles nested braces (e.g., mask = "0:8" inside process0 block)
        # Match: process\d+ = { (anything including nested braces) };
        # Pattern captures leading whitespace and trailing content separately
        pattern = r'(\s*)((?:process\d+\s*=\s*\{(?:[^{}]|\{[^{}]*\})*\};(?:\s*(?=process\d+))?)+)'

        match = re.search(pattern, content, re.DOTALL)
        if match:
            # Preserve leading whitespace from original (only last line's indent)
            leading_ws = match.group(1)
            # Extract just the last line's whitespace (after last newline)
            last_line_ws = leading_ws.split('\n')[-1] if '\n' in leading_ws else leading_ws

            #Replace: keep everything before match, add leading part without last indent,
            # add processes section, add trailing newline
            before_processes = content[:match.start()] + leading_ws.rsplit('\n', 1)[0]
            if before_processes and not before_processes.endswith('\n'):
                before_processes += '\n'

            return before_processes + processes_section + '\n' + content[match.end():]
        else:
            # No existing process blocks - try to replace single command line
            # This handles configs with just: command = "...";
            pattern_command = r'command\s*=\s*"[^"]*";'
            return re.sub(pattern_command, processes_section, content, count=1)
    else:
        # Unchanged if workload_cmd is None or other type
        return content

def parse_core_mask(mask_str):
    """Parse zsim core mask format (e.g., '0:8' or '8:16') to core list

    Returns: list of core IDs
    """
    try:
        if ':' in mask_str:
            start, end = mask_str.split(':')
            return list(range(int(start), int(end)))
        else:
            # Single core or other format
            return [int(mask_str)]
    except:
        return []


def parse_text_stats(zsim_out_file, metadata_file=None):
    """Parse metrics from zsim.out text file as fallback

    Args:
        zsim_out_file: Path to zsim.out
        metadata_file: Optional path to metadata.json for process info

    Returns dict with basic metrics (IPC, and per-process if metadata available)
    """
    try:
        # Read with error handling for non-UTF-8 characters
        with open(zsim_out_file, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()

        lines = content.split('\n')

        # Find the final stats dump (look for last occurrence)
        core_instrs = []
        core_cycles = []

        # Scan backward from end to find most recent stats
        in_core_stats = False
        for line in reversed(lines):
            # Look for core stats section markers
            if 'c-0: # Core stats' in line or 'c: # Core stats' in line:
                in_core_stats = True

        # Now scan forward through the file, collecting the last set of core stats
        core_instrs = []
        core_cycles = []
        current_core_instrs = []
        current_core_cycles = []
        in_stats = False

        for line in lines:
            # Detect start of a stats dump
            if '# Core stats' in line or 'c: # Core stats' in line:
                # Starting new stats section - reset current accumulators
                current_core_instrs = []
                current_core_cycles = []
                in_stats = True
            elif in_stats:
                # Parse instruction counts
                if 'instrs:' in line and '# Simulated instructions' in line:
                    match = re.search(r'instrs:\s+(\d+)', line)
                    if match:
                        current_core_instrs.append(int(match.group(1)))
                # Parse cycle counts
                elif 'cycles:' in line and '# Simulated unhalted cycles' in line:
                    match = re.search(r'cycles:\s+(\d+)', line)
                    if match:
                        current_core_cycles.append(int(match.group(1)))

            # Check if we've finished a complete stats section
            # (next section starts or file ends)
            if in_stats and (line.startswith('===') or 'Termination' in line):
                # Save this complete set
                if current_core_instrs and current_core_cycles:
                    core_instrs = current_core_instrs[:]
                    core_cycles = current_core_cycles[:]

        # Calculate overall IPC from the last complete stats dump
        if core_instrs and core_cycles and len(core_instrs) == len(core_cycles):
            total_instrs = sum(core_instrs)
            max_cycles = max(core_cycles) if core_cycles else 1

            metrics = {}

            if max_cycles > 0 and total_instrs > 0:
                ipc = total_instrs / max_cycles
                metrics['ipc'] = ipc

            # Try to calculate per-process metrics if metadata is available
            if metadata_file and metadata_file.exists():
                try:
                    import json
                    metadata = json.loads(metadata_file.read_text())
                    workload_cmd = metadata.get('workload_cmd')

                    # Check if this is a multi-process workload (list of tuples)
                    if isinstance(workload_cmd, list):
                        per_proc_ipc = {}

                        for proc_id, item in enumerate(workload_cmd):
                            # Handle both (cmd, mask) tuples and single cmd strings
                            if isinstance(item, (list, tuple)) and len(item) >= 2:
                                cmd, mask = item[0], item[1]
                            else:
                                # Single item - skip per-process parsing
                                continue
                            cores = parse_core_mask(mask)

                            # Sum instructions and max cycles for this process's cores
                            proc_instrs = sum(core_instrs[c] for c in cores if c < len(core_instrs))
                            proc_cycles = max((core_cycles[c] for c in cores if c < len(core_cycles)), default=0)

                            if proc_cycles > 0:
                                per_proc_ipc[proc_id] = proc_instrs / proc_cycles

                        if per_proc_ipc:
                            metrics['perProcIPC'] = per_proc_ipc

                except Exception as e:
                    # If per-process parsing fails, just continue with overall IPC
                    pass

            return metrics if metrics else None

    except Exception as e:
        print(f"Warning: Could not parse text stats: {e}")
        import traceback
        traceback.print_exc()

    return None


def parse_zsim_h5_compound(h5_file, config_obj):
    """Parse zsim.h5 compound dataset format

    This handles the native zsim H5 format where stats are stored as
    a compound datatype dataset rather than hierarchical groups.
    """
    try:
        f = h5py.File(h5_file, 'r')

        if 'stats' not in f or not isinstance(f['stats'], h5py.Dataset):
            f.close()
            return None

        stats_dataset = f['stats']
        last_phase = stats_dataset[-1]
        root = last_phase['root']

        # Extract overall IPC from cores
        cores = root['c']
        total_instrs = sum(cores[i]['instrs'] for i in range(len(cores)))
        max_cycles = max(cores[i]['cycles'] for i in range(len(cores)))
        overall_ipc = total_instrs / max_cycles if max_cycles > 0 else 0

        result = {'ipc': overall_ipc}

        # Extract per-process stats if available
        try:
            mem = root['mem'][0]  # Get mem-0

            # Detect number of processes
            num_processes = 0
            while f'Process{num_processes}' in mem.dtype.names:
                num_processes += 1

            if num_processes > 0:
                # Calculate per-process IPC from core stats
                cores_per_process = len(cores) // num_processes
                per_proc_ipc = {}

                for proc_id in range(num_processes):
                    start_core = proc_id * cores_per_process
                    end_core = start_core + cores_per_process
                    proc_instrs = sum(cores[i]['instrs'] for i in range(start_core, end_core))
                    proc_cycles = max(cores[i]['cycles'] for i in range(start_core, end_core))
                    per_proc_ipc[proc_id] = proc_instrs / proc_cycles if proc_cycles > 0 else 0

                result['perProcIPC'] = per_proc_ipc

                # Extract per-process memory stats
                per_proc_hit_ratio = {}
                per_proc_hbm = {}
                per_proc_ddr = {}

                for proc_id in range(num_processes):
                    proc_data = mem[f'Process{proc_id}']

                    per_proc_hbm[proc_id] = int(proc_data['procHbmAccesses'])
                    per_proc_ddr[proc_id] = int(proc_data['procDdrAccesses'])

                    load_hits = int(proc_data['procLoadHits'])
                    load_misses = int(proc_data['procLoadMisses'])
                    total_loads = load_hits + load_misses

                    per_proc_hit_ratio[proc_id] = load_hits / total_loads if total_loads > 0 else 0.0

                result['perProcHitRatio'] = per_proc_hit_ratio
                result['perProcHbmAccesses'] = per_proc_hbm
                result['perProcDdrAccesses'] = per_proc_ddr

        except (KeyError, IndexError):
            pass  # Per-process stats not available

        # Call custom metric calculators if provided
        metric_calculators = getattr(config_obj, 'METRIC_CALCULATORS', None)
        if metric_calculators:
            # Get warmup phase from config
            warmup_phase = getattr(config_obj, 'WARMUP_PHASE', 0.2)
            num_records = len(stats_dataset)
            warmup_idx = int(num_records * warmup_phase)
            warmup_idx = max(0, min(warmup_idx, num_records - 1))

            # Only read the two phases we need: warmup and last
            # This is MUCH faster than reading all 200k-500k phases
            warmup_phase_data = stats_dataset[warmup_idx]
            last_phase_data = stats_dataset[-1]

            # Create a stats-like object that calculators can use
            class CompoundStatsAdapter:
                """Adapter to make compound dataset look like group-based format

                The calculators expect stats['mem'] to return an array that can be indexed
                by phase index. We only store warmup and last phase for efficiency.
                """
                def __init__(self, warmup_data, last_data, warmup_idx, total_len):
                    self.warmup_data = warmup_data['root']
                    self.last_data = last_data['root']
                    self.warmup_idx = warmup_idx
                    self.total_len = total_len

                def __getitem__(self, key):
                    # Return a fake array that supports indexing by warmup_idx and -1
                    field_data = TwoPhaseArray(
                        self.warmup_data[key],
                        self.last_data[key],
                        self.warmup_idx,
                        self.total_len
                    )
                    return field_data

                def __len__(self):
                    return self.total_len

            class TwoPhaseArray:
                """Fake array that only stores two phases but supports indexing"""
                def __init__(self, warmup_val, last_val, warmup_idx, total_len):
                    self.warmup_val = warmup_val
                    self.last_val = last_val
                    self.warmup_idx = warmup_idx
                    self.total_len = total_len

                def __getitem__(self, idx):
                    # Support both integer indexing (for phase access) and string indexing (for field access)
                    if isinstance(idx, str):
                        # Field access like ['accesses'] - return TwoPhaseArray of that field
                        warmup_field = self.warmup_val[idx] if hasattr(self.warmup_val, '__getitem__') else getattr(self.warmup_val, idx)
                        last_field = self.last_val[idx] if hasattr(self.last_val, '__getitem__') else getattr(self.last_val, idx)
                        return TwoPhaseArray(warmup_field, last_field, self.warmup_idx, self.total_len)
                    elif idx == self.warmup_idx:
                        return self.warmup_val
                    elif idx == -1 or idx == self.total_len - 1:
                        return self.last_val
                    else:
                        raise IndexError(f"Only warmup phase ({self.warmup_idx}) and last phase (-1) are available")

                @property
                def dtype(self):
                    """Expose dtype for field checking"""
                    return self.warmup_val.dtype if hasattr(self.warmup_val, 'dtype') else None

            stats = CompoundStatsAdapter(warmup_phase_data, last_phase_data, warmup_idx, num_records)

            for metric_name, calculator_func in metric_calculators.items():
                try:
                    value = calculator_func(stats, warmup_idx)
                    if value is not None:
                        # Handle both scalar and dict-valued metrics
                        if isinstance(value, dict):
                            result[metric_name] = value
                        else:
                            result[metric_name] = float(value)
                except Exception as e:
                    print(f"      Error calculating {metric_name}: {e}")
                    import traceback
                    traceback.print_exc()

        f.close()
        return result

    except Exception as e:
        print(f"      Error parsing H5 compound format: {e}")
        import traceback
        traceback.print_exc()
        return None


def generate_config(base_config, params, values, fixed_params):
    """Generate a zsim config file with specified parameters

    Empty string values are skipped (parameter keeps its default value)
    """
    content = base_config

    # Apply swept parameters (skip empty strings)
    for param, value in zip(params, values):
        # Skip empty string values - leave parameter at default
        if value == "":
            continue
        # Determine if it's a string parameter.
        # Float values like "0.001" fail str.isdigit(), so try float() conversion.
        if isinstance(value, str):
            try:
                float(value.rstrip('Ll'))   # handles ints, floats, and "4096L"
                is_string = False
            except ValueError:
                is_string = True            # e.g. "freqBased", "DDR5-2666-CL20"
        else:
            is_string = False
        content = replace_param(content, param, value, is_string)

    # Apply fixed parameters (skip empty strings)
    for fixed in fixed_params:
        if '=' in fixed:
            param, value = [x.strip() for x in fixed.split('=', 1)]
            value = value.rstrip(';')
            # Skip empty string values
            if value == "":
                continue
            is_string = '"' in value
            if is_string:
                value = value.strip('"')
            content = replace_param(content, param, value, is_string)

    return content

def parse_zsim_h5(h5_file, config_obj):
    """Parse zsim.h5 file and extract metrics using warmup phase

    Args:
        h5_file: Path to zsim.h5 file
        config_obj: Experiment config object with WARMUP_PHASE and optional METRIC_CALCULATORS

    Returns dict with available metrics, e.g.:
    {
        'ipc': 4.5,
        'fastMemHitRatio': 0.85
    }
    """
    if not h5_file.exists():
        return None

    try:
        f = h5py.File(h5_file, 'r')

        # Try to access stats/root structure (group-based format)
        if 'stats' in f and isinstance(f['stats'], h5py.Group) and 'root' in f['stats']:
            stats = f['stats']['root']
        elif 'stats' in f and isinstance(f['stats'], h5py.Dataset):
            # Compound dataset format - use specialized parser
            f.close()
            return parse_zsim_h5_compound(h5_file, config_obj)
        else:
            print(f"      Warning: H5 file has unexpected structure")
            f.close()
            return None
    except (UnicodeDecodeError, OSError) as e:
        print(f"      Error opening h5 file (encoding issue): {e}")
        return None
    except Exception as e:
        print(f"      Error opening h5 file: {e}")
        return None

    metrics = {}

    # Get warmup phase (default 20%)
    warmup_phase = getattr(config_obj, 'WARMUP_PHASE', 0.2)
    num_records = len(stats)
    warmup_idx = int(num_records * warmup_phase)

    # Ensure warmup_idx is at least 0 and less than num_records
    warmup_idx = max(0, min(warmup_idx, num_records - 1))

    # Built-in IPC calculation
    try:
        cores = stats['c']
        instrs_start = np.sum(cores['instrs'][warmup_idx])
        instrs_end = np.sum(cores['instrs'][-1])
        cycles_start = np.max(cores['cycles'][warmup_idx])
        cycles_end = np.max(cores['cycles'][-1])

        total_instrs = instrs_end - instrs_start
        total_cycles = cycles_end - cycles_start

        if total_cycles > 0:
            metrics['ipc'] = float(total_instrs / total_cycles)
    except Exception as e:
        print(f"Warning: Could not calculate IPC: {e}")

    # Call custom metric calculators if provided
    metric_calculators = getattr(config_obj, 'METRIC_CALCULATORS', None)
    if metric_calculators:
        print(f"      Found {len(metric_calculators)} metric calculators, calculating...")
        for metric_name, calculator_func in metric_calculators.items():
            try:
                value = calculator_func(stats, warmup_idx)
                if value is not None:
                    # Handle both scalar and dict-valued metrics
                    if isinstance(value, dict):
                        # Per-process metrics: store as dict
                        metrics[metric_name] = value
                    else:
                        # Scalar metrics: convert to float
                        metrics[metric_name] = float(value)
                    print(f"      ✓ {metric_name}: {value if not isinstance(value, dict) else 'dict'}")
                else:
                    print(f"      ✗ {metric_name}: returned None")
            except Exception as e:
                print(f"      ✗ {metric_name}: Error - {e}")
    else:
        print(f"      Warning: No METRIC_CALCULATORS found in config")

    f.close()
    return metrics if metrics else None

def run_experiments(config_obj, config_file_path, dry_run=False):
    """Run experiments locally - wrapper that handles single or multiple configs

    Args:
        config_obj: Experiment configuration object
        config_file_path: Path to experiment config file
        dry_run: If True, generate configs but don't run zsim

    Returns:
        output_dir: Path to results directory (parent dir if multiple configs)
    """

    # Check if BASE_CONFIG is a list or single config
    base_configs = config_obj.BASE_CONFIG
    if not isinstance(base_configs, list):
        # Single config - convert to list for uniform handling
        base_configs = [base_configs]

    if len(base_configs) == 1:
        # Single config - extract name from path
        config_path = Path(base_configs[0])
        config_name = config_path.stem  # e.g., "Tiering_SimpleDdrCxl_WeakSramCaches"
        return run_experiments_single_config(
            config_obj, config_file_path, base_configs[0], config_name, dry_run=dry_run
        )
    else:
        # Multiple configs - run each separately
        print("=" * 70)
        print(f"MULTIPLE CONFIG MODE: Running {len(base_configs)} configurations")
        print("=" * 70)
        print()

        parent_output_dir = None
        for i, base_cfg_str in enumerate(base_configs, 1):
            config_path = Path(base_cfg_str)
            config_name = config_path.stem  # e.g., "Tiering_SimpleDdrCxl_WeakSramCaches"

            print()
            print(f"{'=' * 70}")
            print(f"CONFIG {i}/{len(base_configs)}: {config_name}")
            print(f"{'=' * 70}")
            print()

            output_dir = run_experiments_single_config(
                config_obj, config_file_path, base_cfg_str, config_name, dry_run=dry_run
            )

            if parent_output_dir is None:
                # Save parent directory (first config's parent)
                parent_output_dir = output_dir.parent

            print()
            print(f"Completed config {i}/{len(base_configs)}: {config_name}")
            print(f"Results: {output_dir.resolve()}")

        print()
        print("=" * 70)
        print(f"ALL {len(base_configs)} CONFIGS COMPLETED")
        print(f"Results parent directory: {parent_output_dir.resolve()}")
        print("=" * 70)

        return parent_output_dir


def create_zsim_wrapper(output_dir):
    """Create wrapper script for zsim execution with proper library paths

    This is needed for rocky8/nvidia machines that require conda libraries.

    Returns absolute path to the wrapper script.
    """
    import shutil

    zsim_root = Path(__file__).parent.parent.resolve()
    zsim_bin = zsim_root / "build/opt/zsim"

    if not zsim_bin.exists():
        print(f"Error: zsim binary not found at {zsim_bin}")
        sys.exit(1)

    # Ensure output directory exists and is absolute
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # Create wrapper script
    wrapper_script = output_dir / "zsim_wrapper.sh"

    wrapper_content = f"""#!/bin/bash
# Wrapper script to run zsim with proper library paths
# Ensure conda's libraries are used (they have GLIBCXX_3.4.34)
export LD_LIBRARY_PATH=/path/to/scratch/MARS/tools/conda/lib:$LD_LIBRARY_PATH
exec {zsim_bin} "$@"
"""

    try:
        wrapper_script.write_text(wrapper_content)
        wrapper_script.chmod(0o755)  # Make executable
    except Exception as e:
        print(f"Error creating wrapper script: {e}")
        sys.exit(1)

    # Verify the script is executable
    if not os.access(wrapper_script, os.X_OK):
        print(f"Error: Wrapper script is not executable")
        sys.exit(1)

    print(f"✓ Created zsim wrapper: {wrapper_script}")
    print(f"  Zsim binary: {zsim_bin}")
    print()

    return wrapper_script.resolve()


def copy_zsim_binary(output_dir):
    """Copy zsim binary to output directory to freeze the version

    This ensures experiments use the zsim version from launch time,
    even if the code is rebuilt during execution.

    Returns absolute path to the copied binary.
    """
    import shutil

    zsim_bin = Path(__file__).parent.parent / "build/opt/zsim"
    zsim_bin = zsim_bin.resolve()  # Make absolute

    if not zsim_bin.exists():
        print(f"Error: zsim binary not found at {zsim_bin}")
        sys.exit(1)

    # Ensure output directory exists and is absolute
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # Copy to output directory
    output_bin = output_dir / "zsim_binary"

    try:
        shutil.copy2(zsim_bin, output_bin)
        output_bin.chmod(0o755)  # Make executable
    except Exception as e:
        print(f"Error copying zsim binary: {e}")
        sys.exit(1)

    # Verify the copy succeeded
    if not output_bin.exists():
        print(f"Error: Failed to copy zsim binary to {output_bin}")
        sys.exit(1)

    if not os.access(output_bin, os.X_OK):
        print(f"Warning: Copied binary is not executable, attempting to fix permissions...")
        try:
            output_bin.chmod(0o755)
        except Exception as e:
            print(f"Error: Could not make binary executable: {e}")
            sys.exit(1)

    print(f"✓ Copied zsim binary to: {output_bin}")
    print(f"  Original: {zsim_bin}")
    print(f"  Binary is executable: {os.access(output_bin, os.X_OK)}")

    # Quick test: check if binary can show help (just test it runs)
    try:
        test_result = subprocess.run([str(output_bin), "--help"],
                                     capture_output=True, timeout=5)
        if test_result.returncode != 0 and b"usage" not in test_result.stderr.lower():
            print(f"  Warning: Binary test returned code {test_result.returncode}")
            print(f"  This may indicate missing dependencies")
            # Check for common library issues
            ldd_result = subprocess.run(["ldd", str(output_bin)],
                                       capture_output=True, text=True, timeout=5)
            if "not found" in ldd_result.stdout:
                print(f"  ⚠ Missing shared libraries detected:")
                for line in ldd_result.stdout.split('\n'):
                    if "not found" in line:
                        print(f"    {line.strip()}")
    except subprocess.TimeoutExpired:
        pass  # Timeout is okay, just means it's running
    except Exception as e:
        print(f"  Note: Could not test binary: {e}")

    print()
    return output_bin.resolve()  # Return absolute path


def run_experiments_single_config(config_obj, config_file_path, base_cfg_path_str, config_name, dry_run=False):
    """Run all experiments for a single config file

    Args:
        config_name: Short name extracted from config file (e.g., "Tiering_SimpleDdrCxl_WeakSramCaches")
        dry_run: If True, generate configs but don't run zsim
    """

    mode_str = "LOCAL MODE - DRY RUN" if dry_run else "LOCAL MODE"
    print("=" * 70)
    print(f"zsim Parameter Sweep - {mode_str}")
    print(f"Config: {config_name}")
    print("=" * 70)
    print()

    # Load base config (resolve relative to config file location, not script)
    config_file_dir = Path(config_file_path).parent
    base_cfg_path = Path(base_cfg_path_str)
    if not base_cfg_path.is_absolute():
        base_cfg_path = (config_file_dir / base_cfg_path).resolve()

    if not base_cfg_path.exists():
        print(f"Error: Base config not found: {base_cfg_path}")
        sys.exit(1)

    print(f"Base config: {base_cfg_path}")
    base_config = base_cfg_path.read_text()

    # Create output directory with config name
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    # Only add suffix if using default "results" directory
    if config_obj.OUTPUT_DIR == "results":
        output_dir = Path(f"results_local_{timestamp}")
    else:
        output_dir = Path(config_obj.OUTPUT_DIR)

    # Append config name to output directory
    output_dir = output_dir / config_name
    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {output_dir.resolve()}")

    # Save experiment metadata
    exp_metadata = {
        'config_file': str(Path(config_file_path).resolve()),
        'execution_mode': 'local',
        'timestamp': timestamp,
        'experiment_name': getattr(config_obj, 'EXPERIMENT_NAME', None)
    }
    (output_dir / "experiment_metadata.json").write_text(json.dumps(exp_metadata, indent=2))

    print()

    # Copy zsim binary to freeze version
    zsim_bin = copy_zsim_binary(output_dir)

    # Apply global knobs to base config
    for global_knob in config_obj.GLOBAL_KNOBS:
        if '=' in global_knob:
            param, value = [x.strip() for x in global_knob.split('=', 1)]
            value = value.rstrip(';')
            is_string = '"' in value
            if is_string:
                value = value.strip('"')
            base_config = replace_param(base_config, param, value, is_string)

    # On nvidia/rocky8 machines, enable ASLR to avoid personality() requirement
    if is_nvidia_machine():
        print("Enabling ASLR (nvidia/rocky8 machine without personality() capability)")
        # Insert aslr parameter into sim section
        sim_pattern = r'(sim\s*=\s*\{)'
        if re.search(sim_pattern, base_config):
            # Check if aslr already exists
            if re.search(r'\baslr\s*=', base_config):
                # Replace existing value
                base_config = re.sub(r'\baslr\s*=\s*[^;]+;', 'aslr = true;', base_config)
            else:
                # Insert new parameter
                base_config = re.sub(sim_pattern, r'\1\n    aslr = true;', base_config)
        else:
            print("  Warning: No sim section found in config, aslr parameter not added")
        print()

    # Check zsim binary exists
    if not zsim_bin.exists():
        print(f"Error: zsim binary not found at {zsim_bin}")
        return

    # Set kernel parameters (skip on nvidia/rocky8 machines without sudo)
    if not is_nvidia_machine():
        try:
            subprocess.run("sudo sh -c 'echo 0 > /proc/sys/kernel/yama/ptrace_scope'",
                          shell=True, check=False)
        except:
            print("Warning: Could not set ptrace_scope")
    else:
        print("Skipping ptrace_scope setup (nvidia/rocky8 machine)")

    # Get workloads if defined
    workloads = getattr(config_obj, 'WORKLOADS', None)
    if workloads is None:
        # No workloads defined, use single run with base config
        workloads = [(None, None)]

    # Run experiments
    all_results = []

    for workload_entry in workloads:
        # Handle 2-tuple, 3-tuple, and 4-tuple formats
        # 2-tuple: (cmd, name)
        # 3-tuple: (cmd, name, has_roi)
        # 4-tuple: (cmd, name, has_roi, should_plot)
        if len(workload_entry) == 2:
            workload_cmd, workload_name = workload_entry
            workload_has_roi = None  # Not specified
            workload_should_plot = True  # Default to True
        elif len(workload_entry) == 3:
            workload_cmd, workload_name, workload_has_roi = workload_entry
            workload_should_plot = True  # Default to True
        else:  # 4 or more elements
            workload_cmd, workload_name, workload_has_roi, workload_should_plot = workload_entry[:4]
        if workload_name:
            print(f"=== Workload: {workload_name} ({workload_cmd}) ===")
            print()

        for knob in config_obj.KNOBS:
            knob_params = knob[0]
            knob_values = knob[1]
            knob_fixed = knob[2]
            knob_name = knob[3]

            print(f"Running sweep: {knob_name}")
            print(f"  Parameters: {knob_params}")
            print(f"  Fixed: {knob_fixed if knob_fixed else 'None'}")
            if workload_name:
                print(f"  Workload: {workload_name}")
            print()

            for i, values in enumerate(knob_values, 1):
                # Extract method plot flag if present
                # Values can be: ["policy", "metric"] or ["policy", "metric", should_plot]
                if isinstance(values, (list, tuple)) and len(values) > len(knob_params):
                    # Extra element is the plot flag
                    method_should_plot = values[len(knob_params)]
                    param_values = values[:len(knob_params)]
                else:
                    method_should_plot = True
                    param_values = values

                # Overall plot flag: only plot if both method and workload are enabled
                should_plot = method_should_plot and workload_should_plot

                # Create experiment name using substituted sweep name
                config_label = substitute_sweep_name(knob_name, param_values)
                exp_name = config_label
                if workload_name:
                    exp_name = f"{workload_name}_{config_label}"

                # Create filesystem-safe directory name (replace '=' with '_')
                exp_dir_name = sanitize_name_for_filesystem(exp_name)
                exp_dir = output_dir / exp_dir_name
                exp_dir.mkdir(parents=True, exist_ok=True)

                # Generate config with workload command if specified
                config_content = base_config
                if workload_cmd:
                    config_content = apply_workload_to_config(config_content, workload_cmd)

                # Inject startFastForwarded into every processN block
                if workload_has_roi is not None:
                    ff_value = "true" if workload_has_roi else "false"
                    config_content = inject_start_fast_forwarded(config_content, ff_value)

                config_content = generate_config(config_content, knob_params, param_values, knob_fixed)
                config_file = exp_dir / "config.cfg"
                config_file.write_text(config_content)

                # Save metadata
                metadata = {
                    'sweep': knob_name,
                    'parameters': dict(zip(knob_params, param_values)),
                    'fixed': knob_fixed,
                    'workload': workload_name,
                    'workload_cmd': workload_cmd,
                    'should_plot': should_plot,
                }
                (exp_dir / "metadata.json").write_text(json.dumps(metadata, indent=2))

                print(f"  [{i}/{len(knob_values)}] Running: {exp_name}")
                print(f"      Parameters: {dict(zip(knob_params, param_values))}")

                if dry_run:
                    # Dry-run mode: just print what would be run
                    print(f"      [DRY-RUN] Would execute: {zsim_bin} config.cfg")
                    print(f"      [DRY-RUN] Config saved to: {config_file.resolve()}")
                    print(f"      [DRY-RUN] Output would go to: {exp_dir.resolve()}")
                    continue

                # Run zsim
                output_file = exp_dir / "zsim.out"
                cmd = f"cd {exp_dir} && {zsim_bin} config.cfg > zsim.out 2>&1"

                start = time.time()
                result = subprocess.run(cmd, shell=True)
                elapsed = time.time() - start

                if result.returncode == 0:
                    # Parse results from h5 file
                    h5_file = exp_dir / "zsim.h5"
                    metrics = parse_zsim_h5(h5_file, config_obj)

                    # Fallback to text parsing if H5 fails
                    if not metrics:
                        print(f"      H5 parsing failed, trying text stats...")
                        zsim_out = exp_dir / "zsim.out"
                        metadata_file = exp_dir / "metadata.json"
                        if zsim_out.exists():
                            metrics = parse_text_stats(zsim_out, metadata_file)

                    if metrics and 'ipc' in metrics:
                        ipc = metrics['ipc']
                        print(f"      ✓ Completed in {elapsed:.1f}s - IPC: {ipc:.4f}")
                        # config_label already computed above
                        result_entry = {
                            'name': exp_name,
                            'params': dict(zip(knob_params, param_values)),
                            'workload': workload_name,
                            'sweep': knob_name,
                            'config_label': config_label,
                            'config_name': config_name,  # Add config name for plotting
                            'should_plot': should_plot,
                            'ipc': ipc,
                            'runtime': elapsed
                        }
                        # Add all additional metrics from h5 file
                        for metric_name, metric_value in metrics.items():
                            if metric_name != 'ipc':  # IPC already added
                                result_entry[metric_name] = metric_value
                        all_results.append(result_entry)
                    else:
                        print(f"      ✓ Completed but could not parse metrics")
                else:
                    print(f"      ✗ Failed with exit code {result.returncode}")
                    print(f"      Check {output_file} for details")
                    # Show last few lines of output for quick diagnosis
                    if output_file.exists():
                        try:
                            with open(output_file, 'r') as f:
                                lines = f.readlines()
                                if lines:
                                    print(f"      Last lines of output:")
                                    for line in lines[-5:]:
                                        print(f"        {line.rstrip()}")
                        except:
                            pass
                print()

        if workload_name:
            print()

    # Save results
    if not dry_run and all_results:
        results_file = output_dir / "results.json"
        with open(results_file, 'w') as f:
            json.dump(all_results, f, indent=2)
        print(f"Results saved to: {results_file.resolve()}")

        # Generate plots for each metric
        print()
        print("Generating plots...")
        plot_results(all_results, output_dir, config_obj)
    elif dry_run:
        print()
        print("=" * 70)
        print("DRY-RUN COMPLETE")
        print(f"Generated {len(knob_values) * len(workloads) * len(config_obj.KNOBS)} experiment configs")
        print(f"Configs saved to: {output_dir.resolve()}")
        print("=" * 70)

    return output_dir

def get_method_order_from_knobs(config_obj):
    """Extract the order of methods from KNOBS definition

    Returns a dict mapping config_label to its order index
    """
    method_order = {}
    order_idx = 0

    for knob in config_obj.KNOBS:
        knob_params = knob[0]
        knob_values = knob[1]
        knob_name = knob[3]

        for values in knob_values:
            # Generate the config_label the same way we do during experiment generation
            config_label = substitute_sweep_name(knob_name, values)
            method_order[config_label] = order_idx
            order_idx += 1

    return method_order


def plot_results(results, output_dir, config_obj):
    """Generate comparison plots for all requested metrics

    Generates separate plots for each (config_file, sweep, metric) combination.

    METRICS can be:
    - List of tuples: [("IPC", "normalized"), ("fastMemHitRatio", "percentage")]
    - List of strings (backward compat): ["IPC", "fastMemHitRatio"]
    """

    # Get method order from KNOBS
    method_order = get_method_order_from_knobs(config_obj)

    # Group results by config file (from directory structure)
    # Results structure: results_dir/config_name/exp_name/
    results_by_config = {}
    for result in results:
        # Extract config name from experiment directory structure
        # Assume structure: output_dir/config_name/exp_dir
        exp_name = result.get('name', '')

        # Try to infer config name from parent directory
        # This will be set when we parse results from multi-config runs
        config_name = result.get('config_name', 'default')

        if config_name not in results_by_config:
            results_by_config[config_name] = []
        results_by_config[config_name].append(result)

    # Check if we have workloads
    has_workloads = any(result.get('workload') is not None for result in results)

    # Parse METRICS - support both tuple and string format
    metrics_to_plot = []
    for metric_spec in config_obj.METRICS:
        if isinstance(metric_spec, tuple):
            metric_name, display_mode = metric_spec
            metrics_to_plot.append((metric_name, display_mode))
        else:
            # Backward compatibility: treat string as (metric_name, "absolute")
            if metric_spec in ['IPC', 'fastMemHitRatio']:
                metrics_to_plot.append((metric_spec, "absolute"))

    # Generate plots for each config file
    for config_name, config_results in results_by_config.items():
        # Group by sweep within each config
        results_by_sweep = {}
        for result in config_results:
            sweep_name = result.get('sweep', 'default_sweep')
            if sweep_name not in results_by_sweep:
                results_by_sweep[sweep_name] = []
            results_by_sweep[sweep_name].append(result)

        # Generate plots for each (sweep, metric) combination
        for sweep_name, sweep_results in results_by_sweep.items():
            for metric_name, display_mode in metrics_to_plot:
                # Map metric name to key in results dict
                # Handle special cases where display name differs from key
                if metric_name == 'IPC':
                    metric_key = 'ipc'
                else:
                    # For all other metrics, use the metric name as-is
                    # (they should match the keys in METRIC_CALCULATORS)
                    metric_key = metric_name

                # Verify at least one result has this metric
                if not any(metric_key in result for result in sweep_results):
                    print(f"Warning: Metric '{metric_name}' not found in results, skipping plot")
                    continue

                # Create output subdirectory for this config
                config_output_dir = output_dir
                if config_name != 'default':
                    config_output_dir = output_dir / config_name
                    config_output_dir.mkdir(parents=True, exist_ok=True)

                # Create a clean sweep name for filenames (remove template placeholders)
                # Replace %1, %2, etc. with parameter names
                clean_sweep_name = sweep_name
                if '%' in sweep_name:
                    # Extract parameter names from first result
                    if sweep_results:
                        params = sweep_results[0].get('params', {})
                        param_names = '_'.join(params.keys())
                        clean_sweep_name = param_names if param_names else 'sweep'

                if has_workloads:
                    plot_grouped_results(sweep_results, config_output_dir, metric_key, metric_name,
                                       display_mode, config_name, clean_sweep_name, method_order)
                else:
                    plot_simple_results(sweep_results, config_output_dir, metric_key, metric_name,
                                      display_mode, config_name, clean_sweep_name, method_order)


def plot_simple_results(results, output_dir, metric_key, metric_name, display_mode="absolute",
                       config_name="", sweep_name="", method_order=None):
    """Generate simple comparison plot (no workloads)

    Args:
        results: List of result dicts
        output_dir: Directory to save plot
        metric_key: Key to extract from results (e.g., 'ipc', 'fastMemHitRatio')
        metric_name: Human-readable name for axis labels
        display_mode: "absolute", "normalized", or "percentage"
        config_name: Name of config file (for title and filename)
        sweep_name: Name of sweep (for title and filename)
        method_order: Dict mapping config_label to order index
    """
    import scipy.stats

    # Filter results to only those that should be plotted
    results = [r for r in results if r.get('should_plot', True)]

    # Collect labels and values
    data_pairs = []

    for result in results:
        if metric_key not in result:
            continue

        if 'config_label' in result and result['config_label']:
            label = result['config_label']
        else:
            params = result['params']
            label = ", ".join(f"{k}={v}" for k, v in params.items())
        data_pairs.append((label, result[metric_key]))

    if not data_pairs:
        print(f"Warning: No {metric_name} data to plot for {config_name} / {sweep_name}")
        return

    # Sort by method_order if provided
    if method_order:
        data_pairs.sort(key=lambda x: method_order.get(x[0], 999999))

    labels, values = zip(*data_pairs)
    labels = list(labels)
    values = list(values)

    # Apply display mode transformation
    if display_mode == "normalized" and values:
        baseline = values[0]
        if baseline != 0:
            values = [v / baseline for v in values]
    elif display_mode == "percentage" and values:
        values = [v * 100 for v in values]

    # Create bar plot with improved sizing
    fig, ax = plt.subplots(figsize=(max(14, len(labels) * 1.2), 8))

    bars = ax.bar(range(len(labels)), values, color='steelblue', edgecolor='black', linewidth=0.8)

    # Add value labels on bars (rotated 90 degrees) with larger font
    for bar in bars:
        height = bar.get_height()
        if display_mode == "percentage":
            label_text = f'{height:.1f}%'
        elif display_mode == "normalized":
            label_text = f'{height:.2f}x'
        else:
            label_text = f'{height:.3f}'
        ax.text(bar.get_x() + bar.get_width()/2., height,
                label_text,
                ha='center', va='bottom', fontsize=10, fontweight='bold',
                rotation=90)

    # Create labels based on display mode with larger fonts
    ax.set_xlabel('Configuration', fontsize=14, fontweight='bold')

    if display_mode == "normalized":
        ylabel = f"Normalized {metric_name}"
        title_suffix = f" (Normalized to {labels[0]})"
    elif display_mode == "percentage":
        ylabel = f"{metric_name} (%)"
        title_suffix = ""
    else:  # absolute
        ylabel = metric_name
        title_suffix = ""

    ax.set_ylabel(ylabel, fontsize=14, fontweight='bold')

    # Build informative title with config and sweep name
    title_parts = []
    if config_name and config_name != 'default':
        title_parts.append(config_name)
    if sweep_name and sweep_name != 'default_sweep':
        title_parts.append(sweep_name)
    title_parts.append(f"{metric_name}{title_suffix}")

    full_title = " | ".join(title_parts) if title_parts else f"{metric_name}{title_suffix}"
    ax.set_title(full_title, fontsize=16, fontweight='bold', pad=20)

    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, rotation=45, ha='right', fontsize=12)
    ax.grid(axis='y', alpha=0.3, linestyle='--')

    # Add reference line at y=1.0 for normalized plots
    if display_mode == "normalized":
        ax.axhline(y=1.0, color='black', linestyle='--', linewidth=1.5, alpha=0.7, label='Baseline')

    # Increase tick label sizes
    ax.tick_params(axis='both', which='major', labelsize=11)

    plt.tight_layout()

    # Create filename from config, sweep, metric key and display mode
    filename_parts = []
    if config_name and config_name != 'default':
        filename_parts.append(config_name)
    if sweep_name and sweep_name != 'default_sweep':
        # Sanitize sweep name for filesystem (remove all special chars except _)
        import re
        safe_sweep = re.sub(r'[^a-zA-Z0-9_]', '_', sweep_name)
        # Remove consecutive underscores
        safe_sweep = re.sub(r'_+', '_', safe_sweep).strip('_')
        filename_parts.append(safe_sweep)
    filename_parts.append(metric_key)

    if display_mode == "normalized":
        filename_parts.append("normalized")
    elif display_mode == "percentage":
        filename_parts.append("percentage")

    plot_filename = "_".join(filename_parts) + ".png"
    plot_file = output_dir / plot_filename
    plt.savefig(plot_file, dpi=150, bbox_inches='tight')
    print(f"Plot saved to: {plot_file.resolve()}")
    plt.close()


def plot_grouped_results(results, output_dir, metric_key, metric_name, display_mode="absolute",
                        config_name="", sweep_name="", method_order=None):
    """Generate grouped bar chart: workloads on x-axis, bars for each sweep config

    Args:
        results: List of result dicts
        output_dir: Directory to save plot
        metric_key: Key to extract from results (e.g., 'ipc', 'fastMemHitRatio')
        metric_name: Human-readable name for axis labels
        display_mode: "absolute", "normalized", or "percentage"
        config_name: Name of config file (for title and filename)
        sweep_name: Name of sweep (for title and filename)
        method_order: Dict mapping config_label to order index
    """
    import scipy.stats

    # Filter results to only those that should be plotted
    results = [r for r in results if r.get('should_plot', True)]

    # Organize data: {workload: {config_label: value}}
    data = {}
    for result in results:
        if metric_key not in result:
            continue

        workload = result['workload']
        # Use config_label if available, otherwise create from parameters
        if 'config_label' in result and result['config_label']:
            config_label = result['config_label']
        else:
            params = result['params']
            config_label = ", ".join(f"{k}={v}" for k, v in params.items())

        if workload not in data:
            data[workload] = {}
        data[workload][config_label] = result[metric_key]

    if not data:
        print(f"Warning: No {metric_name} data to plot")
        return

    # Get unique workloads (preserve alphabetical order)
    workloads = sorted(data.keys())

    # Get configs in the order specified in experiment file
    all_configs = []
    for wl_data in data.values():
        for config in wl_data.keys():
            if config not in all_configs:
                all_configs.append(config)

    # Sort configs by method_order if provided
    if method_order:
        all_configs.sort(key=lambda c: method_order.get(c, 999999))

    # Apply display mode transformation
    if display_mode == "normalized":
        # Normalize each workload to the FIRST config (baseline)
        baseline_config = all_configs[0] if all_configs else None
        if baseline_config:
            for workload in workloads:
                if baseline_config in data[workload]:
                    baseline_value = data[workload][baseline_config]
                    if baseline_value != 0:
                        for config in data[workload]:
                            data[workload][config] = data[workload][config] / baseline_value
    elif display_mode == "percentage":
        # Convert to percentage
        for workload in workloads:
            for config in data[workload]:
                data[workload][config] = data[workload][config] * 100

    # Add geomean/arithmetic mean column
    workloads_with_avg = workloads + ['Average']
    averages = {}
    for config in all_configs:
        values = [data[wl].get(config, 0) for wl in workloads if data[wl].get(config, 0) > 0]
        if values:
            if display_mode == "normalized":
                # Use geometric mean for normalized data
                averages[config] = scipy.stats.gmean(values)
            else:
                # Use arithmetic mean for absolute/percentage data
                averages[config] = np.mean(values)
        else:
            averages[config] = 0

    # Add averages to data
    data['Average'] = averages

    # Prepare data for plotting
    num_cols = len(workloads_with_avg)
    num_configs = len(all_configs)

    # Create figure with better sizing for readability
    fig_width = max(16, num_cols * 1.5)
    fig, ax = plt.subplots(figsize=(fig_width, 8))

    # Set width of bars and positions
    bar_width = 0.8 / num_configs
    x = np.arange(num_cols)

    # Plot bars for each configuration with better colors
    colors = plt.cm.tab10(np.linspace(0, 1, min(10, num_configs)))
    if num_configs > 10:
        colors = plt.cm.tab20(np.linspace(0, 1, num_configs))

    for i, config in enumerate(all_configs):
        values = [data[wl].get(config, 0) for wl in workloads_with_avg]
        offset = (i - num_configs/2 + 0.5) * bar_width

        bars = ax.bar(x + offset, values, bar_width, label=config,
                     color=colors[i % len(colors)], edgecolor='black', linewidth=0.8)

        # Add value labels on bars (rotated 90 degrees) with larger font
        for bar in bars:
            height = bar.get_height()
            if height > 0:  # Only show label if there's a value
                if display_mode == "percentage":
                    label_text = f'{height:.1f}%'
                elif display_mode == "normalized":
                    label_text = f'{height:.2f}x'
                else:
                    label_text = f'{height:.2f}'
                ax.text(bar.get_x() + bar.get_width()/2., height,
                       label_text,
                       ha='center', va='bottom', fontsize=9,
                       rotation=90)

    # Customize plot based on display mode with larger fonts
    ax.set_xlabel('Workload', fontsize=14, fontweight='bold')

    if display_mode == "normalized":
        ylabel = f"Normalized {metric_name}"
        title_suffix = f" (Normalized to {all_configs[0] if all_configs else 'baseline'})"
    elif display_mode == "percentage":
        ylabel = f"{metric_name} (%)"
        title_suffix = ""
    else:  # absolute
        ylabel = metric_name
        title_suffix = ""

    ax.set_ylabel(ylabel, fontsize=14, fontweight='bold')

    # Build informative title with config and sweep name
    title_parts = []
    if config_name and config_name != 'default':
        title_parts.append(config_name)
    if sweep_name and sweep_name != 'default_sweep':
        title_parts.append(sweep_name)
    title_parts.append(f"{metric_name} Across Workloads{title_suffix}")

    full_title = " | ".join(title_parts) if title_parts else f"{metric_name} Across Workloads{title_suffix}"
    ax.set_title(full_title, fontsize=16, fontweight='bold', pad=20)

    ax.set_xticks(x)
    ax.set_xticklabels(workloads_with_avg, rotation=45, ha='right', fontsize=12)

    # Make Average column stand out visually
    avg_col_idx = len(workloads)
    ax.axvline(x=avg_col_idx - 0.5, color='red', linestyle='--', linewidth=2, alpha=0.5)

    # Improved legend positioning and sizing
    ax.legend(title='Method', bbox_to_anchor=(1.02, 1), loc='upper left', fontsize=11, title_fontsize=12)
    ax.grid(axis='y', alpha=0.3, linestyle='--')

    # Add reference line at y=1.0 for normalized plots
    if display_mode == "normalized":
        ax.axhline(y=1.0, color='black', linestyle='--', linewidth=1.5, alpha=0.7, label='Baseline')

    # Increase tick label sizes
    ax.tick_params(axis='both', which='major', labelsize=11)

    plt.tight_layout()

    # Create filename from config, sweep, metric key and display mode
    filename_parts = []
    if config_name and config_name != 'default':
        filename_parts.append(config_name)
    if sweep_name and sweep_name != 'default_sweep':
        # Sanitize sweep name for filesystem (remove all special chars except _)
        import re
        safe_sweep = re.sub(r'[^a-zA-Z0-9_]', '_', sweep_name)
        # Remove consecutive underscores
        safe_sweep = re.sub(r'_+', '_', safe_sweep).strip('_')
        filename_parts.append(safe_sweep)
    filename_parts.append(metric_key)

    if display_mode == "normalized":
        filename_parts.append("normalized")
    elif display_mode == "percentage":
        filename_parts.append("percentage")

    plot_filename = "_".join(filename_parts) + ".png"
    plot_file = output_dir / plot_filename
    plt.savefig(plot_file, dpi=150, bbox_inches='tight')
    print(f"Plot saved to: {plot_file.resolve()}")
    plt.close()

# =============================================================================
# Cluster Execution
# =============================================================================

def calculate_batch_size(total_experiments, max_batch_size=128):
    """Calculate optimal batch size for experiments

    Goal: maximize batch size while staying under max_batch_size
    and creating roughly equal-sized batches.

    Args:
        total_experiments: Total number of experiments to run
        max_batch_size: Maximum experiments per batch (default 128)

    Returns:
        batch_size: Optimal number of experiments per batch
    """
    if total_experiments <= max_batch_size:
        return total_experiments

    # Calculate number of batches needed
    num_batches = (total_experiments + max_batch_size - 1) // max_batch_size

    # Calculate batch size that divides experiments evenly
    batch_size = (total_experiments + num_batches - 1) // num_batches

    return batch_size


class Rocky8ClusterManager:
    """Manage Rocky8 cluster job submission with rolling job execution

    Unlike batch-by-batch execution, this maintains a constant pool of
    running jobs (ROCK8_JOB_BATCH) and submits new jobs as soon as
    one finishes.
    """

    def __init__(self, output_dir, max_concurrent_jobs=200):
        self.output_dir = output_dir
        self.max_concurrent_jobs = max_concurrent_jobs
        self.state_file = output_dir / ".rocky8_state.json"
        self.log_file = output_dir / "rocky8_execution.log"
        self.status_file = output_dir / "rocky8_status.txt"
        self.job_ids_file = output_dir / ".rocky8_job_ids.txt"  # Track all submitted job IDs

        # Track running jobs: {job_id: exp_info}
        self.running_jobs = {}
        # Track pending experiments to submit
        self.pending_experiments = []
        # Track completed experiments
        self.completed_experiments = []
        # Track failed experiments
        self.failed_experiments = []
        # Track failed submissions (for --resume)
        self.failed_submissions = []
        # Adaptive sleep between qsub calls (starts at 10s)
        self.submit_sleep = 10
        # Track ALL submitted job IDs for cleanup
        self.all_job_ids = set()
        # Cache for bulk bjobs query: {job_id: output_line}
        self._bjobs_cache = {}
        self._bjobs_cache_time = 0.0

    def log(self, msg):
        """Log to both console and file"""
        timestamp = datetime.now().strftime('%H:%M:%S')
        log_msg = f"[{timestamp}] {msg}"
        print(log_msg)
        with open(self.log_file, 'a') as f:
            f.write(log_msg + '\n')

    def save_job_id(self, job_id):
        """Save job ID to file for later cleanup"""
        self.all_job_ids.add(job_id)
        with open(self.job_ids_file, 'a') as f:
            f.write(f"{job_id}\n")

    def kill_all_jobs(self):
        """Kill all jobs associated with this run"""
        # Load job IDs from file
        if self.job_ids_file.exists():
            with open(self.job_ids_file, 'r') as f:
                job_ids = [line.strip() for line in f if line.strip()]
        else:
            job_ids = []

        if not job_ids:
            return 0

        self.log(f"Found {len(job_ids)} jobs from previous run, killing...")
        killed = 0
        already_done = 0

        for job_id in job_ids:
            try:
                result = subprocess.run(
                    ['bkill', job_id],
                    capture_output=True,
                    text=True,
                    timeout=600  # 10 minute timeout (farm can be slow)
                )
                if result.returncode == 0:
                    killed += 1
                elif 'not found' in result.stderr.lower() or 'already finished' in result.stderr.lower():
                    already_done += 1
                else:
                    self.log(f"  Warning killing job {job_id}: {result.stderr.strip()}")
            except Exception as e:
                self.log(f"  Error killing job {job_id}: {e}")

        self.log(f"  Killed: {killed}, Already finished: {already_done}")

        # Clear the job IDs file
        if self.job_ids_file.exists():
            self.job_ids_file.unlink()

        return killed

    def submit_job(self, exp, dry_run=False):
        """Submit a single experiment job using qsub

        Args:
            exp: Experiment configuration dict
            dry_run: If True, generate script but don't submit

        Returns job_id if successful, None otherwise
        """
        exp_dir = exp['dir']
        config_file = exp['config']
        exp_name = exp['name']

        # Ensure absolute paths
        exp_dir_abs = exp_dir.resolve()

        # Create PBS batch script (use absolute path)
        pbs_script = exp_dir_abs / "submit.sh"

        # Use wrapper script or direct binary
        zsim_exec = exp.get('zsim_bin', 'build/opt/zsim')

        # Check if this is an ML workload (uses inference binary) by reading config
        is_ml_workload = False
        config_path = exp_dir_abs / "config.cfg"
        try:
            if config_path.exists():
                config_content = config_path.read_text()
                is_ml_workload = '/inference' in config_content
        except:
            pass

        # Build environment variables for zsim launch
        env_vars = []

        # On nvidia machines, set OMP env vars to disable thread affinity
        if is_nvidia_machine():
            env_vars.extend(['OMP_PROC_BIND=false', 'KMP_AFFINITY=disabled'])

        # For ML workloads, add conda's libstdc++ for GLIBCXX 3.4.34+ and CXXABI compatibility
        # ONNX Runtime 1.14.1 is compatible with GLIBC 2.17, so we only need newer C++ libs
        if is_ml_workload:
            conda_lib = "/path/to/scratch/MARS/tools/conda/lib"  # GLIBCXX 3.4.34, CXXABI
            onnx_lib = "/path/to/scratch/workloads/onnx-runtime/onnxruntime-linux-x64-1.14.1/lib"
            env_vars.append(f'LD_LIBRARY_PATH={conda_lib}:{onnx_lib}:$LD_LIBRARY_PATH')

        # Combine all environment variables
        env_prefix = ' '.join(env_vars) + ' ' if env_vars else ''

        pbs_content = f"""#!/bin/bash
cd {exp_dir_abs}

# Record start time
date +%s.%N > .start_time

# Run zsim (with environment variables for ML workloads and nvidia machines)
{env_prefix}{zsim_exec} config.cfg > zsim.out 2>&1

# Record completion
EXIT_CODE=$?
date +%s.%N > .end_time
if [ $EXIT_CODE -eq 0 ]; then
    touch .completed
fi

exit $EXIT_CODE
"""

        try:
            pbs_script.write_text(pbs_content)
            pbs_script.chmod(0o755)

            if dry_run:
                # Dry run mode - don't actually submit, just return a dummy job ID
                self.log(f"  [DRY RUN] Would submit: {exp_name}")
                self.log(f"    Script: {pbs_script}")
                return None  # Return None to indicate no actual job was submitted

            # Submit job using qsub with 128GB RAM queue and affinity scheduling
            # o_cpu_128G_48H: 128GB RAM, 48 hour time limit (needed for zsim with 100B instructions)
            qsub_cmd = ['qsub', '-app', 'affinity', '-q', 'o_cpu_128G_48H', str(pbs_script)]

            # Log the command
            self.log(f"  Executing: {' '.join(qsub_cmd)}")

            # Time the qsub submission for adaptive sleep
            qsub_start = time.time()
            result = subprocess.run(
                qsub_cmd,
                capture_output=True,
                text=True,
                timeout=600,  # 10 minute timeout (farm can be slow)
                cwd=str(exp_dir)
            )
            qsub_duration = time.time() - qsub_start

            # Log stdout and stderr
            self.log(f"  qsub returncode: {result.returncode}")
            if result.stdout.strip():
                self.log(f"  qsub stdout: {result.stdout.strip()}")
            if result.stderr.strip():
                self.log(f"  qsub stderr: {result.stderr.strip()}")

            if result.returncode == 0:
                # Parse job ID from qsub output
                # Format: "Job <1004418143> is submitted to queue <o_cpu_128G_48H>."
                output = result.stdout.strip()

                # Extract job ID from angle brackets
                import re
                match = re.search(r'<(\d+)>', output)
                if match:
                    job_id = match.group(1)
                else:
                    # Fallback: use entire output if no match (shouldn't happen)
                    job_id = output

                # Store job info for tracking
                (exp_dir / ".job_id").write_text(job_id)

                # Save job ID for cleanup on next run
                self.save_job_id(job_id)

                # Log full qsub command to jobs manifest
                jobs_manifest = self.output_dir / "jobs_manifest.txt"
                with open(jobs_manifest, 'a') as f:
                    timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                    cmd_str = ' '.join(qsub_cmd)
                    f.write(f"[{timestamp}] Job {job_id}: {exp_name}\n")
                    f.write(f"  Command: {cmd_str}\n")
                    f.write(f"  Directory: {exp_dir_abs}\n")
                    f.write(f"  Script: {pbs_script}\n\n")

                # Adaptive sleep based on qsub duration
                # If qsub took 10+ seconds, farm is busy - increase sleep
                if qsub_duration >= 10:
                    self.submit_sleep = min(60, self.submit_sleep + 10)  # Cap at 60s
                    self.log(f"  Farm busy (qsub took {qsub_duration:.1f}s), increasing sleep to {self.submit_sleep}s")
                elif qsub_duration < 5 and self.submit_sleep > 10:
                    # Farm responding quickly, can reduce sleep
                    self.submit_sleep = max(10, self.submit_sleep - 5)  # Min 10s

                # Sleep before next submission
                time.sleep(self.submit_sleep)

                return job_id
            else:
                self.log(f"  Error submitting {exp_name}: {result.stderr}")
                self.failed_submissions.append(exp)
                return None

        except subprocess.TimeoutExpired:
            self.log(f"  Timeout submitting {exp_name} (waited 10 minutes)")
            self.failed_submissions.append(exp)
            # Increase sleep significantly after timeout
            self.submit_sleep = min(60, self.submit_sleep + 20)
            return None
        except Exception as e:
            self.log(f"  Error submitting {exp_name}: {e}")
            self.failed_submissions.append(exp)
            return None

    def _refresh_bjobs_cache(self):
        """Fetch all tracked job statuses in a single bjobs call.

        Replaces per-job bjobs invocations with one bulk query per monitoring
        iteration so the scheduler is not hammered.
        """
        job_ids = list(self.running_jobs.keys())
        if not job_ids:
            self._bjobs_cache = {}
            return
        try:
            result = subprocess.run(
                ['bjobs'] + job_ids,
                capture_output=True, text=True, timeout=120
            )
            self._bjobs_cache = {}
            for line in result.stdout.splitlines():
                parts = line.split()
                if parts and parts[0].isdigit():
                    self._bjobs_cache[parts[0]] = line
            self._bjobs_cache_time = time.time()
        except Exception as e:
            self.log(f"  bjobs bulk query failed: {e} — keeping previous cache")

    def check_job_status(self, exp):
        """Check job status using heartbeat files and completion markers

        Returns: dict with status info similar to ClusterManager.check_experiment_status
        """
        exp_dir = exp['dir']

        # Check completion markers
        completion_marker = exp_dir / ".completed"
        end_time_marker = exp_dir / ".end_time"
        heartbeat_file = exp_dir / "heartbeat"

        # Check if simulation has finished
        if end_time_marker.exists():
            # Check if it completed successfully
            zsim_output = exp_dir / "zsim.h5"
            if zsim_output.exists():
                return {'status': 'completed', 'current_cycles': None, 'speed': None}
            else:
                return {'status': 'failed', 'current_cycles': None, 'speed': None}

        # Try to read heartbeat file
        if heartbeat_file.exists():
            try:
                heartbeat_content = heartbeat_file.read_text()
                if heartbeat_content.strip():
                    # Parse heartbeat format
                    lines = heartbeat_content.strip().split('\n')
                    current_cycles = None
                    current_speed = None

                    for i, line in enumerate(lines):
                        # Look for total cycles
                        if 'cycles' in line and 'phases' not in line and 'cycles/s' not in line:
                            parts = line.strip().split()
                            if len(parts) >= 2 and parts[0].isdigit():
                                current_cycles = int(parts[0])

                        # Look for instantaneous speed
                        if 'Stats since last heartbeat' in line and i + 1 < len(lines):
                            next_line = lines[i + 1].strip()
                            parts = next_line.split()
                            if len(parts) >= 2 and parts[0].isdigit():
                                current_speed = int(parts[0]) / 1e6  # Convert to Mcycles/s

                    if current_cycles is not None:
                        # Check heartbeat age
                        mtime = heartbeat_file.stat().st_mtime
                        current_time = time.time()
                        age = current_time - mtime

                        # If heartbeat is recent (< 5 minutes old)
                        if age < 300:
                            return {
                                'status': 'running',
                                'current_cycles': current_cycles,
                                'speed': current_speed if current_speed else 0,
                                'progress': None
                            }
                        else:
                            # Heartbeat is stale - likely stalled or completed
                            return {
                                'status': 'stalled',
                                'current_cycles': current_cycles,
                                'speed': 0,
                                'age': age
                            }
            except:
                pass

        # No heartbeat or couldn't parse - check if job is still in queue
        # Use the bulk bjobs cache populated once per monitoring iteration.
        job_id_file = exp_dir / ".job_id"
        if job_id_file.exists():
            try:
                job_id = job_id_file.read_text().strip()
                # Look up from the per-iteration cache (populated by _refresh_bjobs_cache)
                cached_line = self._bjobs_cache.get(job_id, None)
                if cached_line is not None:
                    # Job found in cache — parse LSF state from the line
                    # LSF states: PEND=pending, RUN=running, DONE=completed, EXIT=failed
                    if 'PEND' in cached_line or 'PSUSP' in cached_line:
                        return {'status': 'pending', 'current_cycles': 0, 'speed': None}
                    elif 'RUN' in cached_line or 'USUSP' in cached_line:
                        return {'status': 'starting', 'current_cycles': 0, 'speed': None}
                    # DONE/EXIT: fall through to check completion markers
                else:
                    # Not in cache: job finished or not found — fall through
                    pass
            except Exception:
                # If we can't read the job_id file, assume still queued
                return {'status': 'starting', 'current_cycles': 0, 'speed': None}

        # No heartbeat, no job_id file - this is a real failure
        return {'status': 'failed', 'current_cycles': 0, 'speed': None}

    def update_status_file(self, all_experiments):
        """Write detailed status to file"""
        with open(self.status_file, 'w') as f:
            f.write(f"Rocky8 Cluster Status - {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write("=" * 80 + "\n\n")

            # Overall statistics
            total = len(all_experiments)
            completed = len(self.completed_experiments)
            failed = len(self.failed_experiments)
            running = len(self.running_jobs)
            pending = len(self.pending_experiments)
            progress_pct = (completed / total) * 100 if total > 0 else 0

            f.write(f"Overall Progress: {completed}/{total} ({progress_pct:.1f}%)\n")
            f.write(f"Completed: {completed}\n")
            f.write(f"Failed: {failed}\n")
            f.write(f"Running/Queued: {running}\n")
            f.write(f"Pending submission: {pending}\n")
            f.write("\n")

            # Classify running jobs by detailed state
            if self.running_jobs:
                job_states = {
                    'pending': [],
                    'starting': [],
                    'running': [],
                    'stalled': []
                }

                for job_id, exp in self.running_jobs.items():
                    status_info = self.check_job_status(exp)
                    status = status_info['status']
                    if status in job_states:
                        job_states[status].append((job_id, exp, status_info))

                f.write("Running Jobs by State:\n")
                f.write("-" * 80 + "\n")
                f.write(f"  Pending (in queue): {len(job_states['pending'])}\n")
                f.write(f"  Starting (no heartbeat yet): {len(job_states['starting'])}\n")
                f.write(f"  Running (active): {len(job_states['running'])}\n")
                f.write(f"  Stalled (old heartbeat): {len(job_states['stalled'])}\n")
                f.write("\n")

                # Show actively running jobs with progress
                if job_states['running']:
                    f.write("Active Running Jobs (with progress):\n")
                    f.write("-" * 80 + "\n")
                    for job_id, exp, status_info in job_states['running'][:50]:  # Show first 50
                        cycles = status_info.get('current_cycles', 0)
                        speed = status_info.get('speed', 0)

                        # Calculate progress in billions and percentage
                        instrs_b = cycles / 1e9  # Convert to billions

                        # Try to get max instructions from config to calculate percentage
                        max_instrs = None
                        progress_pct_str = ""
                        try:
                            config_file = exp['config']
                            max_instrs = parse_max_total_instrs(config_file)
                            if max_instrs:
                                progress_pct = (cycles / max_instrs) * 100
                                progress_pct_str = f" ({progress_pct:.1f}%)"
                        except:
                            pass

                        f.write(f"  Job {job_id}: {exp['name']}\n")
                        f.write(f"    Progress: {instrs_b:.1f}B{progress_pct_str}, Speed: {speed:.2f} Mcycles/s\n")
                    if len(job_states['running']) > 50:
                        f.write(f"  ... and {len(job_states['running']) - 50} more\n")
                    f.write("\n")

                # Show stalled jobs
                if job_states['stalled']:
                    f.write("Stalled Jobs (may need attention):\n")
                    f.write("-" * 80 + "\n")
                    for job_id, exp, status_info in job_states['stalled']:
                        age = status_info.get('age', 0)
                        f.write(f"  Job {job_id}: {exp['name']}\n")
                        f.write(f"    Heartbeat age: {age/60:.1f} minutes\n")
                    f.write("\n")

                # Show starting jobs
                if job_states['starting']:
                    f.write("Starting Jobs (waiting for heartbeat):\n")
                    f.write("-" * 80 + "\n")
                    for job_id, exp, status_info in job_states['starting'][:30]:  # Show first 30
                        f.write(f"  Job {job_id}: {exp['name']}\n")
                    if len(job_states['starting']) > 30:
                        f.write(f"  ... and {len(job_states['starting']) - 30} more\n")
                    f.write("\n")

            # Show recently completed experiments
            if self.completed_experiments:
                f.write("Recently Completed Experiments:\n")
                f.write("-" * 80 + "\n")
                for exp in self.completed_experiments[-30:]:  # Last 30
                    f.write(f"  {exp['name']}\n")
                if len(self.completed_experiments) > 30:
                    f.write(f"  ... and {len(self.completed_experiments) - 30} more\n")
                f.write("\n")

            # Show failed experiments
            if self.failed_experiments:
                f.write("Failed Experiments:\n")
                f.write("-" * 80 + "\n")
                for exp in self.failed_experiments:
                    f.write(f"  {exp['name']}\n")

    def run_experiments_rolling(self, experiments, zsim_bin, dry_run=False):
        """Run experiments with rolling job submission

        Maintains max_concurrent_jobs running at all times.
        Submits new job immediately when one finishes.

        Args:
            experiments: List of experiment configurations
            zsim_bin: Path to zsim binary/wrapper
            dry_run: If True, generate scripts but don't submit jobs
        """
        self.pending_experiments = list(experiments)

        # Store zsim_bin in each experiment
        for exp in self.pending_experiments:
            exp['zsim_bin'] = zsim_bin

        # Partition experiments: skip completed, re-adopt already-submitted in-flight jobs.
        # This makes re-running the script safe when jobs are already queued/running.
        actually_pending = []
        for exp in list(self.pending_experiments):
            exp_dir = exp['dir']
            if (exp_dir / ".completed").exists():
                self.completed_experiments.append(exp)
            elif (exp_dir / ".job_id").exists():
                try:
                    job_id = (exp_dir / ".job_id").read_text().strip()
                    if job_id:
                        self.running_jobs[job_id] = exp
                        self.all_job_ids.add(job_id)
                except Exception:
                    actually_pending.append(exp)
            else:
                actually_pending.append(exp)
        self.pending_experiments = actually_pending

        if self.completed_experiments:
            self.log(f"Skipping {len(self.completed_experiments)} already-completed experiments")
        if self.running_jobs:
            self.log(f"Resuming tracking of {len(self.running_jobs)} in-flight jobs")

        self.log(f"Starting rolling job submission with max {self.max_concurrent_jobs} concurrent jobs")
        self.log(f"Total experiments to run: {len(experiments)}")
        print()

        if dry_run:
            # Dry run mode - just generate submission scripts
            self.log("Generating job submission scripts (dry run)...")
            for exp in self.pending_experiments:
                self.submit_job(exp, dry_run=True)
            print()
            self.log(f"Generated {len(experiments)} job submission scripts")
            self.log("To actually submit jobs, remove --dry-run flag")
            return [], []  # Return empty lists for completed/failed

        # Initial job submission (fill up to max_concurrent_jobs, accounting for in-flight)
        self.log("Submitting initial batch of jobs...")
        slots_available = max(0, self.max_concurrent_jobs - len(self.running_jobs))
        initial_submit_count = min(slots_available, len(self.pending_experiments))

        for _ in range(initial_submit_count):
            if not self.pending_experiments:
                break

            exp = self.pending_experiments.pop(0)
            job_id = self.submit_job(exp, dry_run=False)

            if job_id:
                self.running_jobs[job_id] = exp
                self.log(f"  Submitted job {job_id}: {exp['name']}")
            else:
                # Failed to submit, add to failed list
                self.failed_experiments.append(exp)
                self.log(f"  Failed to submit: {exp['name']}")

        print()
        self.log(f"Submitted {len(self.running_jobs)} initial jobs")
        self.log("Monitoring jobs and submitting new ones as jobs complete...")
        print()

        # Wait for LSF to register jobs in the queue system
        if self.running_jobs:
            self.log("Waiting 60 seconds for LSF to register jobs in the queue...")
            time.sleep(60)
            self.log("Resuming job monitoring")
            print()

        # Track start time for elapsed calculation
        start_time = time.time()
        last_update_time = time.time()

        # Main monitoring loop
        while self.running_jobs or self.pending_experiments:
            # Single bulk bjobs call for all in-flight jobs (one per iteration)
            self._refresh_bjobs_cache()

            # Check status of all running jobs and collect display state in one pass
            completed_job_ids = []
            job_states = {'pending': 0, 'starting': 0, 'running': 0, 'stalled': 0}

            for job_id in list(self.running_jobs.keys()):
                exp = self.running_jobs[job_id]
                status_info = self.check_job_status(exp)
                status = status_info['status']

                if status == 'completed':
                    # Check if it actually completed successfully
                    completion_marker = exp['dir'] / ".completed"
                    if completion_marker.exists():
                        self.completed_experiments.append(exp)
                        self.log(f"✓ Job {job_id} completed: {exp['name']}")
                    else:
                        self.failed_experiments.append(exp)
                        self.log(f"✗ Job {job_id} failed: {exp['name']}")

                    completed_job_ids.append(job_id)

                elif status == 'failed':
                    self.failed_experiments.append(exp)
                    self.log(f"✗ Job {job_id} failed: {exp['name']}")
                    completed_job_ids.append(job_id)

                elif status == 'stalled':
                    self.failed_experiments.append(exp)
                    age = status_info.get('age', 0)
                    self.log(f"✗ Job {job_id} stalled: {exp['name']} (heartbeat age: {age:.0f}s)")
                    completed_job_ids.append(job_id)

                else:
                    # Still in flight — count for display
                    if status in job_states:
                        job_states[status] += 1

            # Remove completed jobs and submit new ones
            for job_id in completed_job_ids:
                del self.running_jobs[job_id]

                # Submit next pending job if available
                if self.pending_experiments:
                    exp = self.pending_experiments.pop(0)
                    new_job_id = self.submit_job(exp)

                    if new_job_id:
                        self.running_jobs[new_job_id] = exp
                        job_states['pending'] += 1
                        self.log(f"  Submitted job {new_job_id}: {exp['name']}")
                    else:
                        self.failed_experiments.append(exp)
                        self.log(f"  Failed to submit: {exp['name']}")

            # Update status file with detailed info
            self.update_status_file(experiments)

            # Calculate metrics (job_states already populated in the loop above)
            current_time = time.time()
            total = len(experiments)
            completed = len(self.completed_experiments)
            failed = len(self.failed_experiments)
            pending_submit = len(self.pending_experiments)
            total_submitted = total - pending_submit

            progress_pct = (completed / total) * 100 if total > 0 else 0

            # Calculate elapsed time
            elapsed_sec = current_time - start_time
            elapsed_min = int(elapsed_sec / 60)
            elapsed_hr = elapsed_min // 60
            elapsed_min_mod = elapsed_min % 60

            # Estimate time remaining
            if completed > 0:
                avg_time_per_job = elapsed_sec / completed
                remaining_jobs = total - completed - failed
                eta_sec = avg_time_per_job * remaining_jobs
                eta_min = int(eta_sec / 60)
                eta_hr = eta_min // 60
                eta_min_mod = eta_min % 60
                eta_str = f"{eta_hr}h{eta_min_mod:02d}m" if eta_hr > 0 else f"{eta_min_mod}m"
            else:
                eta_str = "calculating..."

            # Format elapsed time
            if elapsed_hr > 0:
                elapsed_str = f"{elapsed_hr}h{elapsed_min_mod:02d}m"
            else:
                elapsed_str = f"{elapsed_min_mod}m"

            # Build detailed summary
            timestamp = datetime.now().strftime('%H:%M:%S')
            summary_parts = [
                f"[{timestamp}]",
                f"Progress: {completed}/{total} ({progress_pct:.1f}%)",
                f"Submitted: {total_submitted}",
                f"Queued: {job_states['pending']}",
                f"Starting: {job_states['starting']}",
                f"Running: {job_states['running']}",
                f"Stalled: {job_states['stalled']}",
                f"Pending: {pending_submit}",
                f"Failed: {failed}",
                f"Elapsed: {elapsed_str}",
                f"ETA: {eta_str}"
            ]
            summary = " | ".join(summary_parts)

            # Overwrite previous line if in TTY, otherwise print every 2 minutes
            if sys.stdout.isatty():
                print(f"\r{summary}", end='', flush=True)
            else:
                # In non-TTY mode, print every 2 minutes to avoid log spam
                if current_time - last_update_time >= 120:
                    print(summary)
                    last_update_time = current_time

            # Check if all done
            if not self.running_jobs and not self.pending_experiments:
                break

            # Wait before next check (≥90 s between bjobs invocations)
            time.sleep(90)

        print()  # Final newline
        print()

        # Retry failed submissions once
        if self.failed_submissions:
            self.log("=" * 70)
            self.log(f"Retrying {len(self.failed_submissions)} failed submissions...")
            self.log("=" * 70)
            print()

            retry_failures = []
            for exp in self.failed_submissions:
                self.log(f"  Retrying: {exp['name']}")
                job_id = self.submit_job(exp, dry_run=False)
                if job_id:
                    self.log(f"    ✓ Submitted job {job_id}")
                    # Don't wait for this job, just log it
                else:
                    self.log(f"    ✗ Failed again")
                    retry_failures.append(exp)
                    self.failed_experiments.append(exp)

            print()
            if retry_failures:
                self.log(f"⚠ {len(retry_failures)} experiments still failed to submit")
                self.log("  Run with --resume to retry these experiments")

        # Final summary
        self.log("=" * 70)
        self.log("Rolling job execution completed!")
        self.log(f"  Total: {len(experiments)}")
        self.log(f"  Completed: {len(self.completed_experiments)}")
        self.log(f"  Failed: {len(self.failed_experiments)}")
        if self.failed_submissions:
            self.log(f"  Failed submissions: {len(self.failed_submissions)}")
        self.log("=" * 70)

        return self.completed_experiments, self.failed_experiments


class ClusterManager:
    """Manage cluster node allocation and execution"""

    def __init__(self, output_dir, resume=False):
        self.output_dir = output_dir
        self.project = "MYPROJECT"
        self.os_image = "ubuntu18-zsim"
        self.startup_script = Path(__file__).parent / "node_startup.sh"
        self.state_file = output_dir / ".cluster_state.json"
        self.log_file = output_dir / "cluster_execution.log"
        self.nodes = []

        if resume and self.state_file.exists():
            # Load previous state
            state = json.loads(self.state_file.read_text())
            self.exp_name = state['exp_name']
            self.log(f"Resuming from previous run: {self.exp_name}")
        else:
            self.exp_name = f"zsimSweep{int(time.time())}"
            self._save_state()

    def log(self, msg):
        """Log to both console and file"""
        timestamp = datetime.now().strftime('%H:%M:%S')
        log_msg = f"[{timestamp}] {msg}"
        print(log_msg)
        with open(self.log_file, 'a') as f:
            f.write(log_msg + '\n')

    def _save_state(self):
        """Save cluster state for resume"""
        state = {
            'exp_name': self.exp_name,
            'project': self.project,
            'timestamp': time.time()
        }
        with open(self.state_file, 'w') as f:
            json.dump(state, f, indent=2)

    def allocate_nodes(self, num_nodes, resume=False):
        """Allocate cluster nodes or resume with existing allocation"""

        if resume:
            self.log(f"Checking for existing allocation: {self.exp_name}")
            # For resume, we need to check if nodes are accessible
            # Try SSHing to reconstruct node list based on experiment name
            self.nodes = [f"h{i}.{self.exp_name}.{self.project}" for i in range(num_nodes)]
            self.log(f"  Assuming nodes: {', '.join(self.nodes)}")

            # Verify at least one node is accessible
            ssh_opts = "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5"
            test_cmd = f'ssh {ssh_opts} {self.nodes[0]} "echo ok"'
            result = subprocess.run(test_cmd, shell=True, capture_output=True, text=True)

            if result.returncode == 0 and 'ok' in result.stdout:
                self.log("  ✓ Nodes are accessible, resuming...")
                return True
            else:
                self.log("  Nodes not yet accessible, will wait...")
                # Wait up to 45 minutes for nodes to become accessible
                max_wait = 2700
                start_time = time.time()
                while time.time() - start_time < max_wait:
                    time.sleep(60)
                    elapsed = int(time.time() - start_time)
                    self.log(f"  Waiting for nodes... ({elapsed}s / {max_wait}s)")

                    result = subprocess.run(test_cmd, shell=True, capture_output=True, text=True)
                    if result.returncode == 0 and 'ok' in result.stdout:
                        self.log("  ✓ Nodes are now accessible!")
                        return True

                self.log("  Error: Nodes still not accessible after 45 minutes")
                return False

        # Normal allocation (not resume)
        self.log(f"Allocating {num_nodes} nodes on Wolf cluster...")
        self.log(f"  Experiment name: {self.exp_name}")
        self.log(f"  Project: {self.project}")

        # Build allocation command
        makebed_cmd = (
            f"/share/testbed/bin/wf-makebed "
            f"-n {num_nodes} "
            f"-e {self.exp_name} "
            f"-p {self.project} "
            f"-i {self.os_image} "
            f"-s {self.startup_script}"
        )

        # Run on ops node via SSH
        ssh_cmd = f"ssh ops.cluster.example.edu '{makebed_cmd}'"
        self.log(f"  Running: {ssh_cmd}")

        try:
            result = subprocess.run(ssh_cmd, shell=True, capture_output=True, text=True, timeout=2700)
            self.log(f"  Exit code: {result.returncode}")
            if result.stdout:
                self.log(f"  STDOUT: {result.stdout.strip()}")
            if result.stderr:
                self.log(f"  STDERR: {result.stderr.strip()}")

            if result.returncode != 0:
                self.log(f"Error: Failed to allocate nodes")
                return False

            # Parse hostnames from wf-makebed output
            if self._parse_hostnames_from_makebed_output(result.stdout):
                self.log(f"  ✓ Allocated {len(self.nodes)} nodes: {', '.join(self.nodes)}")
                return True
            else:
                self.log(f"  Error: Could not parse hostnames from wf-makebed output")
                return False

        except subprocess.TimeoutExpired:
            self.log("Error: Allocation timed out after 45 minutes")
            return False
        except Exception as e:
            self.log(f"Error: {e}")
            return False

    def _parse_hostnames_from_makebed_output(self, stdout):
        """Parse hostnames from wf-makebed output

        Looks for line like: all done (hostnames=h[0-3].zsimSweep1770089862.MYPROJECT)!
        and expands range notation to full list of hostnames.
        """
        import re

        # Look for the "all done (hostnames=...)" line
        for line in stdout.split('\n'):
            if 'all done' in line and 'hostnames=' in line:
                # Extract hostname pattern
                match = re.search(r'hostnames=([^\)]+)', line)
                if match:
                    hostname_pattern = match.group(1)

                    # Check if it's a range pattern like h[0-3].exp.project
                    range_match = re.match(r'([a-z]+)\[(\d+)-(\d+)\]\.(.+)', hostname_pattern)
                    if range_match:
                        prefix = range_match.group(1)  # "h"
                        start = int(range_match.group(2))  # 0
                        end = int(range_match.group(3))  # 3
                        suffix = range_match.group(4)  # "zsimSweep1770089862.MYPROJECT"

                        # Generate list: h0.exp.project, h1.exp.project, ...
                        self.nodes = [f"{prefix}{i}.{suffix}" for i in range(start, end + 1)]
                        return True
                    else:
                        # Single node or comma-separated list
                        self.nodes = [hostname_pattern.strip()]
                        return True

        return False

    def kill_and_clean_experiment(self, node, exp_name, output_dir):
        """Kill running experiment and clean output files for fresh start"""
        self.log(f"  Killing and cleaning {exp_name} on {node}")

        ssh_opts = "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

        # Kill zsim processes for this experiment
        # Use screen session name to target specific experiment
        screen_name = f"zsim-{exp_name}"
        kill_screen_cmd = f'ssh {ssh_opts} {node} "screen -S {screen_name} -X quit" 2>/dev/null'
        subprocess.run(kill_screen_cmd, shell=True, capture_output=True, text=True)

        # Also pkill any zsim processes (in case screen didn't work)
        kill_cmd = f'ssh {ssh_opts} {node} "pkill -9 -f \'zsim.*{output_dir.name}\'" 2>/dev/null'
        subprocess.run(kill_cmd, shell=True, capture_output=True, text=True)

        # Clean output files (keep config and metadata)
        files_to_remove = [
            'zsim.out', 'zsim.h5', 'zsim-ev.h5', 'zsim-cmp.h5',
            'heartbeat', 'out.cfg',
            '.start_time', '.end_time', '.completed',
            '*.csv'
        ]

        for pattern in files_to_remove:
            file_path = output_dir / pattern
            if '*' in pattern:
                # Handle wildcards
                import glob
                for f in glob.glob(str(file_path)):
                    Path(f).unlink(missing_ok=True)
            else:
                file_path.unlink(missing_ok=True)

        self.log(f"    ✓ Cleaned")

    def launch_experiment(self, node, exp_name, config_file, output_dir, zsim_bin=None):
        """Launch a single experiment on a node

        Since filesystem is shared, we just SSH to the node and run zsim
        pointing to the local paths. No need to copy files.

        Args:
            node: Node hostname
            exp_name: Experiment name
            config_file: Path to config file
            output_dir: Output directory
            zsim_bin: Path to zsim binary (if None, uses default location)
        """
        self.log(f"  Launching {exp_name} on {node}")

        # SSH options to avoid prompts and suppress warnings
        ssh_opts = "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

        # Use absolute path to the local directory (which is shared)
        local_dir = output_dir.resolve()

        # Use provided zsim binary or default to build directory
        if zsim_bin is None:
            zsim_bin = Path(__file__).parent.parent / "build/opt/zsim"
        zsim_bin = Path(zsim_bin).resolve()

        # Check if this is an ML workload (by reading config to see if command contains inference)
        is_ml_workload = False
        try:
            config_content = config_file.read_text()
            is_ml_workload = '/inference' in config_content
        except:
            pass

        # Build environment variables for zsim launch
        env_vars = []

        # On nvidia machines, set OMP env vars to disable thread affinity
        if is_nvidia_machine():
            env_vars.extend(['OMP_PROC_BIND=false', 'KMP_AFFINITY=disabled'])

        # For ML workloads, add conda's libstdc++ for GLIBCXX 3.4.34+ and CXXABI compatibility
        # ONNX Runtime 1.14.1 is compatible with GLIBC 2.17, so we only need newer C++ libs
        if is_ml_workload:
            conda_lib = "/path/to/scratch/MARS/tools/conda/lib"  # GLIBCXX 3.4.34, CXXABI
            onnx_lib = "/path/to/scratch/workloads/onnx-runtime/onnxruntime-linux-x64-1.14.1/lib"
            env_vars.append(f'LD_LIBRARY_PATH={conda_lib}:{onnx_lib}:$LD_LIBRARY_PATH')

        # Combine all environment variables
        env_prefix = ' '.join(env_vars) + ' ' if env_vars else ''

        # Build command with timing - use absolute paths for all output files
        # Use trap to ensure .end_time is created even on crashes/kills
        remote_cmd = (
            f"cd {local_dir} && "
            f"trap 'date +%s.%N > {local_dir}/.end_time' EXIT && "
            f"date +%s.%N > {local_dir}/.start_time && "
            f"{env_prefix}{zsim_bin} {local_dir}/config.cfg > {local_dir}/zsim.out 2>&1; "
            f"EXIT_CODE=$?; "
            f"echo $EXIT_CODE > {local_dir}/.exit_code; "
            f"if [ $EXIT_CODE -eq 0 ]; then "
            f"  touch {local_dir}/.completed; "
            f"else "
            f"  echo 'zsim exited with code '$EXIT_CODE > {local_dir}/.error; "
            f"fi; "
            f"exit $EXIT_CODE"
        )

        # Launch in screen
        screen_name = f"zsim-{exp_name}"
        ssh_cmd = f'ssh {ssh_opts} {node} "screen -S {screen_name} -dm bash -c \'{remote_cmd}\'"'

        result = subprocess.run(ssh_cmd, shell=True, capture_output=True, text=True)
        if result.returncode != 0:
            self.log(f"    Error launching: {result.stderr}")
            return False

        self.log(f"    ✓ Launched")
        return True

    def check_experiment_status(self, node, exp_name, local_dir):
        """Check experiment status using heartbeat files

        Args:
            node: Node hostname
            exp_name: Experiment name
            local_dir: Local directory path (shared filesystem)

        Returns: dict with status, progress info, and simulation speed
        """
        # Since filesystem is shared, we can check files locally
        completion_marker = local_dir / ".completed"
        end_time_marker = local_dir / ".end_time"
        heartbeat_file = local_dir / "heartbeat"

        # Check if simulation has finished (regardless of success)
        # .end_time is created when zsim process exits
        if end_time_marker.exists():
            # Check if it completed successfully by looking for output files
            # zsim.h5 is the main output file that's always created on successful completion
            zsim_output = local_dir / "zsim.h5"
            if zsim_output.exists():
                return {'status': 'completed', 'current_cycles': None, 'speed': None}
            else:
                # Finished but no output file - check for error information
                error_file = local_dir / ".error"
                exit_code_file = local_dir / ".exit_code"

                error_msg = "no output file"
                if error_file.exists():
                    try:
                        error_msg = error_file.read_text().strip()
                    except:
                        pass
                elif exit_code_file.exists():
                    try:
                        exit_code = exit_code_file.read_text().strip()
                        error_msg = f"exit code {exit_code}"
                    except:
                        pass

                return {'status': 'failed', 'current_cycles': None, 'speed': None, 'message': error_msg}

        # Try to read heartbeat file
        if heartbeat_file.exists():
            try:
                heartbeat_content = heartbeat_file.read_text()
            except Exception as e:
                # Log read error for debugging
                heartbeat_content = ""

            result = type('obj', (object,), {'returncode': 0, 'stdout': heartbeat_content})()

            if result.stdout.strip():
                # Parse heartbeat format:
                # Stats since start:
                #  3491 phases
                #  34910000 cycles
                #  286147 cycles/s
                # Stats since last heartbeat (1s):
                #  1710000 cycles/s
                try:
                    lines = result.stdout.strip().split('\n')
                    current_cycles = None
                    current_speed = None

                    for i, line in enumerate(lines):
                        # Look for total cycles
                        if 'cycles' in line and 'phases' not in line and 'cycles/s' not in line:
                            parts = line.strip().split()
                            if len(parts) >= 2 and parts[0].isdigit():
                                current_cycles = int(parts[0])

                        # Look for instantaneous speed (from last heartbeat)
                        if 'Stats since last heartbeat' in line and i + 1 < len(lines):
                            next_line = lines[i + 1].strip()
                            parts = next_line.split()
                            if len(parts) >= 2 and parts[0].isdigit():
                                current_speed = int(parts[0]) / 1e6  # Convert to Mcycles/s

                    if current_cycles is not None:
                        # Get heartbeat timestamp from local file
                        mtime = heartbeat_file.stat().st_mtime
                        current_time = time.time()
                        age = current_time - mtime

                        # If heartbeat is recent (< 5 minutes old)
                        if age < 300:
                            # If we don't have instantaneous speed, calculate from history
                            if current_speed is None:
                                if hasattr(self, '_prev_heartbeat'):
                                    prev_data = self._prev_heartbeat.get(exp_name, {})
                                    if 'cycles' in prev_data and 'time' in prev_data:
                                        cycles_diff = current_cycles - prev_data['cycles']
                                        time_diff = current_time - prev_data['time']
                                        if time_diff > 0:
                                            current_speed = cycles_diff / time_diff / 1e6  # Mcycles/sec
                                else:
                                    self._prev_heartbeat = {}

                            # Save current heartbeat for next calculation
                            if not hasattr(self, '_prev_heartbeat'):
                                self._prev_heartbeat = {}
                            self._prev_heartbeat[exp_name] = {
                                'cycles': current_cycles,
                                'time': current_time
                            }

                            return {
                                'status': 'running',
                                'current_cycles': current_cycles,
                                'speed': current_speed if current_speed else 0,
                                'progress': None  # Can't calculate without total
                            }
                        else:
                            # Heartbeat is stale - check if process is still running
                            ssh_opts = "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
                            check_cmd = f'ssh {ssh_opts} {node} "pgrep -f zsim" 2>/dev/null'
                            proc_result = subprocess.run(check_cmd, shell=True, capture_output=True, text=True)

                            if proc_result.returncode == 0 and proc_result.stdout.strip():
                                # Process still running but heartbeat stale - truly stalled
                                return {
                                    'status': 'stalled',
                                    'current_cycles': current_cycles,
                                    'speed': 0,
                                    'age': age
                                }
                            else:
                                # Process not running and heartbeat stale - simulation finished
                                return {
                                    'status': 'completed',
                                    'current_cycles': current_cycles,
                                    'speed': None
                                }

                except Exception as e:
                    # Failed to parse heartbeat, fall back to process check
                    pass

        # No heartbeat file or couldn't parse - check if process is running on the node
        ssh_opts = "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
        check_cmd = f'ssh {ssh_opts} {node} "pgrep -f zsim" 2>/dev/null'
        proc_result = subprocess.run(check_cmd, shell=True, capture_output=True, text=True)

        if proc_result.returncode == 0 and proc_result.stdout.strip():
            # Process running but no heartbeat yet
            return {'status': 'starting', 'current_cycles': 0, 'speed': None}

        # No process and no completion marker
        return {'status': 'failed', 'current_cycles': 0, 'speed': None}


    def free_nodes(self):
        """Free allocated nodes"""
        self.log("Freeing allocated nodes...")

        makebed_cmd = f"/share/testbed/bin/wf-makebed -p {self.project} -e {self.exp_name} -r"
        ssh_cmd = f"ssh ops.cluster.example.edu '{makebed_cmd}'"

        result = subprocess.run(ssh_cmd, shell=True, capture_output=True, text=True)

        if result.returncode == 0:
            self.log("  ✓ Nodes freed")
        else:
            self.log(f"  Warning: Error freeing nodes: {result.stderr}")


def cleanup_cluster_run():
    """Clean up cluster run: kill all experiments and deallocate nodes"""

    print("=" * 70)
    print("zsim Parameter Sweep - CLEANUP")
    print("=" * 70)
    print()

    # Find most recent results directory with .cluster_state.json
    results_dirs = sorted(Path('.').glob('results_*'), key=lambda p: p.stat().st_mtime, reverse=True)
    output_dir = None
    for d in results_dirs:
        if (d / '.cluster_state.json').exists():
            output_dir = d
            break

    if not output_dir:
        print("Error: No previous cluster run found to clean up")
        return

    print(f"Cleaning up: {output_dir}")

    # Load state
    state_file = output_dir / '.cluster_state.json'
    state = json.loads(state_file.read_text())
    exp_name = state['exp_name']
    project = state['project']

    print(f"Experiment name: {exp_name}")
    print(f"Project: {project}")
    print()

    # Reconstruct node list from experiment directories
    # Count how many experiment directories exist
    exp_dirs = [d for d in output_dir.iterdir() if d.is_dir() and not d.name.startswith('.')]
    num_nodes = min(len(exp_dirs), 4)  # Assume max 4 nodes

    if num_nodes == 0:
        print("No experiment directories found, nothing to clean")
        num_nodes = 1  # Still try to free nodes

    nodes = [f"h{i}.{exp_name}.{project}" for i in range(num_nodes)]
    print(f"Cleaning up {len(nodes)} nodes: {', '.join(nodes)}")
    print()

    # Kill all zsim processes on each node
    ssh_opts = "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5"

    for node in nodes:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Killing zsim processes on {node}...")
        kill_cmd = f'ssh {ssh_opts} {node} "pkill -9 zsim" 2>/dev/null'
        result = subprocess.run(kill_cmd, shell=True, capture_output=True, text=True)

        if result.returncode == 0:
            print(f"  ✓ Processes killed")
        else:
            print(f"  (no processes running or node not accessible)")

    # Free allocated nodes
    print()
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Freeing allocated nodes...")

    makebed_cmd = f"/share/testbed/bin/wf-makebed -p {project} -e {exp_name} -r"
    ssh_cmd = f"ssh ops.cluster.example.edu '{makebed_cmd}'"

    result = subprocess.run(ssh_cmd, shell=True, capture_output=True, text=True)

    if result.returncode == 0:
        print("  ✓ Nodes freed")
    else:
        print(f"  Warning: Error freeing nodes")
        if result.stderr:
            print(f"  {result.stderr}")

    print()
    print("=" * 70)
    print("Cleanup completed!")
    print("=" * 70)


def cleanup_rocky8_run(results_path=None):
    """Clean up rocky8 cluster run: cancel all PBS jobs

    Args:
        results_path: Optional path to specific results directory to clean up
    """

    print("=" * 70)
    print("zsim Parameter Sweep - ROCKY8 CLEANUP")
    print("=" * 70)
    print()

    # Find results directory
    if results_path:
        output_dir = Path(results_path)
        if not output_dir.exists():
            print(f"Error: Results directory not found: {results_path}")
            return
    else:
        # Find most recent results directory
        results_dirs = sorted(Path('.').glob('results_rocky8_*'), key=lambda p: p.stat().st_mtime, reverse=True)
        output_dir = None
        if results_dirs:
            output_dir = results_dirs[0]

        if not output_dir:
            print("Error: No previous rocky8 run found to clean up")
            print("Note: Looking for directories matching 'results_rocky8_*'")
            return

    print(f"Cleaning up: {output_dir}")
    print()

    # Collect all job IDs from .rocky8_job_ids.txt file (preferred) or .job_id files
    job_ids = []

    # First check for centralized job IDs file
    job_ids_file = output_dir / ".rocky8_job_ids.txt"
    if job_ids_file.exists():
        try:
            with open(job_ids_file, 'r') as f:
                for line in f:
                    job_id = line.strip()
                    if job_id:
                        job_ids.append(job_id)
        except:
            pass

    # Also collect from individual .job_id files in experiment directories
    for exp_dir in output_dir.rglob("*"):
        if exp_dir.is_dir():
            job_id_file = exp_dir / ".job_id"
            if job_id_file.exists():
                try:
                    job_id = job_id_file.read_text().strip()
                    if job_id and job_id not in job_ids:
                        job_ids.append(job_id)
                except:
                    pass

    if not job_ids:
        print("  No job IDs found in experiment directories")
        print()

        # Try to list all user's jobs as fallback
        try:
            import getpass
            username = getpass.getuser()
            print(f"  Checking for any running PBS jobs for user {username}...")
            result = subprocess.run(
                ['qstat', '-u', username],
                capture_output=True,
                text=True,
                timeout=600  # 10 minute timeout (farm can be slow)
            )
            if result.returncode == 0:
                print("  Current jobs:")
                print(result.stdout)
        except:
            pass

        print()
        print("=" * 70)
        print("Cleanup completed!")
        print("=" * 70)
        return

    print(f"  Found {len(job_ids)} job IDs from experiment directories")
    print()

    # Cancel all jobs using bkill
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Cancelling all jobs...")
    cancelled = 0
    already_done = 0
    failed = 0

    for job_id in job_ids:
        result = subprocess.run(
            ['bkill', job_id],
            capture_output=True,
            text=True,
            timeout=600  # 10 minute timeout (farm can be slow)
        )
        if result.returncode == 0:
            print(f"  ✓ Killed job {job_id}")
            cancelled += 1
        else:
            # Job might already be finished, don't count as error
            if 'not found' in result.stderr.lower() or 'already finished' in result.stderr.lower():
                print(f"  · Job {job_id} already finished")
                already_done += 1
            else:
                print(f"  ✗ Failed to kill job {job_id}: {result.stderr.strip()}")
                failed += 1

    print()
    print(f"  Summary: {cancelled} killed, {already_done} already finished, {failed} errors")
    print()
    print("=" * 70)
    print("Cleanup completed!")
    print("=" * 70)


def run_experiments_cluster(config_obj, config_file_path, resume=False, force_relaunch=False, results_dir=None, dry_run=False):
    """Run experiments on cluster - wrapper that handles single or multiple configs

    Args:
        config_obj: Experiment configuration object
        config_file_path: Path to experiment config file
        resume: If True, resume from previous run
        force_relaunch: If True, kill and relaunch experiments
        results_dir: Optional results directory to resume from (if None, searches for latest)
        dry_run: If True, generate configs/scripts but don't submit jobs

    Returns:
        output_dir: Path to results directory (parent dir if multiple configs)
    """

    # Check if BASE_CONFIG is a list or single config
    base_configs = config_obj.BASE_CONFIG
    if not isinstance(base_configs, list):
        # Single config - convert to list for uniform handling
        base_configs = [base_configs]

    if len(base_configs) == 1:
        # Single config - extract name from path
        config_path = Path(base_configs[0])
        config_name = config_path.stem  # e.g., "Tiering_SimpleDdrCxl_WeakSramCaches"
        return run_experiments_cluster_single_config(
            config_obj, config_file_path, base_configs[0], config_name,
            resume=resume, force_relaunch=force_relaunch, results_dir=results_dir, dry_run=dry_run
        )
    else:
        # Multiple configs - run each separately
        print("=" * 70)
        print(f"MULTIPLE CONFIG MODE: Running {len(base_configs)} configurations")
        print("=" * 70)
        print()

        parent_output_dir = None
        for i, base_cfg_str in enumerate(base_configs, 1):
            config_path = Path(base_cfg_str)
            config_name = config_path.stem  # e.g., "Tiering_SimpleDdrCxl_WeakSramCaches"

            print()
            print(f"{'=' * 70}")
            print(f"CONFIG {i}/{len(base_configs)}: {config_name}")
            print(f"{'=' * 70}")
            print()

            # For multiple configs, results_dir is None (each config gets its own dir)
            output_dir = run_experiments_cluster_single_config(
                config_obj, config_file_path, base_cfg_str, config_name,
                resume=resume, force_relaunch=force_relaunch, results_dir=None, dry_run=dry_run
            )

            if parent_output_dir is None:
                # Save parent directory (first config's parent)
                parent_output_dir = output_dir.parent

            print()
            print(f"Completed config {i}/{len(base_configs)}: {config_name}")
            print(f"Results: {output_dir.resolve()}")

        print()
        print("=" * 70)
        print(f"ALL {len(base_configs)} CONFIGS COMPLETED")
        print(f"Results parent directory: {parent_output_dir.resolve()}")
        print("=" * 70)

        return parent_output_dir


def run_experiments_cluster_single_config(config_obj, config_file_path, base_cfg_path_str, config_name, resume=False, force_relaunch=False, results_dir=None, dry_run=False):
    """Run experiments on cluster for a single config file

    Args:
        config_obj: Experiment configuration object
        config_file_path: Path to experiment config file
        base_cfg_path_str: Path to base config file (string)
        config_name: Short name for this config (for directory naming)
        resume: If True, resume from previous run
        force_relaunch: If True, kill and relaunch experiments
        results_dir: Optional results directory to resume from (if None, searches for latest)
        dry_run: If True, generate configs/scripts but don't submit jobs
    """

    if dry_run:
        mode_str = "CLUSTER MODE - DRY RUN"
    elif force_relaunch:
        mode_str = "CLUSTER MODE - FORCE RELAUNCH"
    elif resume:
        mode_str = "CLUSTER MODE - RESUME"
    else:
        mode_str = "CLUSTER MODE"

    print("=" * 70)
    print(f"zsim Parameter Sweep - {mode_str}")
    print(f"Config: {config_name}")
    print("=" * 70)
    print()

    # Load base config
    config_file_dir = Path(config_file_path).parent
    base_cfg_path = Path(base_cfg_path_str)
    if not base_cfg_path.is_absolute():
        base_cfg_path = (config_file_dir / base_cfg_path).resolve()

    if not base_cfg_path.exists():
        print(f"Error: Base config not found: {base_cfg_path}")
        sys.exit(1)

    print(f"Base config: {base_cfg_path}")
    base_config = base_cfg_path.read_text()

    # Create or find output directory
    if resume:
        # Use provided results_dir or search for latest
        if results_dir:
            output_dir = Path(results_dir)
            if not output_dir.exists():
                print(f"Error: Results directory not found: {output_dir}")
                sys.exit(1)
            if not (output_dir / '.cluster_state.json').exists():
                print(f"Error: No cluster state found in {output_dir}")
                print("This may not be a cluster run, or the state file is missing")
                sys.exit(1)
        else:
            # Find most recent results directory with .cluster_state.json
            results_dirs = sorted(Path('.').glob('results_*'), key=lambda p: p.stat().st_mtime, reverse=True)
            output_dir = None
            for d in results_dirs:
                if (d / '.cluster_state.json').exists():
                    output_dir = d
                    break

            if not output_dir:
                print("Error: No previous cluster run found to resume from")
                sys.exit(1)

        print(f"Resuming from: {output_dir.resolve()}")
    else:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        # Create output directory with config name
        if config_obj.OUTPUT_DIR == "results":
            output_dir = Path(f"results_{timestamp}")
        else:
            output_dir = Path(config_obj.OUTPUT_DIR)

        # Append config name to output directory
        output_dir = output_dir / config_name
        output_dir.mkdir(parents=True, exist_ok=True)
        print(f"Output directory: {output_dir.resolve()}")

        # Save experiment metadata
        exp_metadata = {
            'config_file': str(Path(config_file_path).resolve()),
            'execution_mode': 'cluster',
            'timestamp': timestamp,
            'experiment_name': getattr(config_obj, 'EXPERIMENT_NAME', None)
        }
        (output_dir / "experiment_metadata.json").write_text(json.dumps(exp_metadata, indent=2))

    print()

    # Copy zsim binary to freeze version (do this before any experiments)
    zsim_bin = copy_zsim_binary(output_dir)

    # Apply global knobs
    for global_knob in config_obj.GLOBAL_KNOBS:
        if '=' in global_knob:
            param, value = [x.strip() for x in global_knob.split('=', 1)]
            value = value.rstrip(';')
            is_string = '"' in value
            if is_string:
                value = value.strip('"')
            base_config = replace_param(base_config, param, value, is_string)

    # On nvidia/rocky8 machines, enable ASLR to avoid personality() requirement
    if is_nvidia_machine():
        print("Enabling ASLR (nvidia/rocky8 machine without personality() capability)")
        # Insert aslr parameter into sim section
        sim_pattern = r'(sim\s*=\s*\{)'
        if re.search(sim_pattern, base_config):
            # Check if aslr already exists
            if re.search(r'\baslr\s*=', base_config):
                # Replace existing value
                base_config = re.sub(r'\baslr\s*=\s*[^;]+;', 'aslr = true;', base_config)
            else:
                # Insert new parameter
                base_config = re.sub(sim_pattern, r'\1\n    aslr = true;', base_config)
        else:
            print("  Warning: No sim section found in config, aslr parameter not added")
        print()

    # Get workloads if defined
    workloads = getattr(config_obj, 'WORKLOADS', None)
    if workloads is None:
        # No workloads defined, use single run with base config
        workloads = [(None, None, False)]

    # Normalise to 3-tuples for backwards compatibility with (cmd, name) lists
    workloads = [
        (w[0], w[1], w[2] if len(w) > 2 else False)
        for w in workloads
    ]

    # Count total experiments
    total_experiments = sum(len(knob[1]) for knob in config_obj.KNOBS) * len(workloads)

    # Prepare all experiments
    all_experiments = []
    for workload_cmd, workload_name, workload_has_roi in workloads:
        for knob in config_obj.KNOBS:
            knob_params = knob[0]
            knob_values = knob[1]
            knob_fixed = knob[2]
            knob_name = knob[3]

            for values in knob_values:
                # Create experiment name using substituted sweep name
                config_label = substitute_sweep_name(knob_name, values)
                exp_name = config_label
                if workload_name:
                    exp_name = f"{workload_name}_{config_label}"

                # Create filesystem-safe directory name (replace '=' with '_')
                exp_dir_name = sanitize_name_for_filesystem(exp_name)
                exp_dir = output_dir / exp_dir_name
                exp_dir.mkdir(parents=True, exist_ok=True)

                # Generate config with workload command if specified
                config_content = base_config
                if workload_cmd:
                    config_content = replace_param(config_content, "command", workload_cmd, is_string=True)

                # Inject startFastForwarded into every processN block
                ff_value = "true" if workload_has_roi else "false"
                config_content = inject_start_fast_forwarded(config_content, ff_value)

                config_content = generate_config(config_content, knob_params, values, knob_fixed)
                config_file = exp_dir / "config.cfg"
                config_file.write_text(config_content)

                # Save metadata (config_label already computed above)
                metadata = {
                    'sweep': knob_name,
                    'parameters': dict(zip(knob_params, values)),
                    'fixed': knob_fixed,
                    'workload': workload_name,
                    'workload_cmd': workload_cmd,
                    'config_label': config_label,
                }
                (exp_dir / "metadata.json").write_text(json.dumps(metadata, indent=2))

                all_experiments.append({
                    'name': exp_name,
                    'dir': exp_dir,
                    'config': config_file,
                    'params': dict(zip(knob_params, values)),
                    'workload': workload_name,
                    'sweep': knob_name,
                    'config_label': config_label,
                    'node': None,
                    'status': 'pending'
                })

    if dry_run:
        print()
        print("=" * 70)
        print("DRY RUN MODE - Configuration Summary")
        print("=" * 70)
        print(f"Generated {len(all_experiments)} experiment configurations")
        print(f"Configs saved to: {output_dir.resolve()}")
        print()
        print("Sample experiments:")
        for i, exp in enumerate(all_experiments[:5]):
            print(f"  {i+1}. {exp['name']}")
            print(f"     Dir: {exp['dir']}")
            print(f"     Config: {exp['config']}")
        if len(all_experiments) > 5:
            print(f"  ... and {len(all_experiments) - 5} more")
        print()
        print("To actually run these experiments, remove --dry-run flag")
        print("=" * 70)
        return output_dir

    # Calculate batch size and split experiments
    batch_size = calculate_batch_size(len(all_experiments), max_batch_size=128)
    batches = [all_experiments[i:i+batch_size]
               for i in range(0, len(all_experiments), batch_size)]

    print()
    print("=" * 70)
    print(f"BATCHING: {len(all_experiments)} total experiments")
    print(f"  Batch size: {batch_size}")
    print(f"  Number of batches: {len(batches)}")
    print(f"  Reason: Narwhal 120-hour node limit requires batching")
    print("=" * 70)
    print()

    # Track all results across batches
    all_results = []

    # Process each batch
    for batch_idx, batch in enumerate(batches, 1):
        print()
        print("=" * 70)
        print(f"BATCH {batch_idx}/{len(batches)}: Running {len(batch)} experiments")
        print("=" * 70)
        print()

        # Initialize cluster manager for this batch
        cluster = ClusterManager(output_dir, resume=resume and batch_idx == 1)

        # Allocate nodes for this batch (one node per experiment)
        num_nodes = len(batch)
        print(f"Allocating {num_nodes} nodes for batch {batch_idx}...")
        if not cluster.allocate_nodes(num_nodes, resume=resume and batch_idx == 1):
            print("Failed to allocate nodes")
            sys.exit(1)

        # Assign experiments to nodes (round-robin)
        for i, exp in enumerate(batch):
            exp['node'] = cluster.nodes[i % len(cluster.nodes)]

        # Launch experiments
        print()
        if resume and batch_idx == 1:
            if force_relaunch:
                cluster.log("Force relaunching all experiments (killing existing ones)...")
                for exp in batch:
                    # Kill and clean existing experiment
                    cluster.kill_and_clean_experiment(exp['node'], exp['name'], exp['dir'])
                    # Launch fresh
                    cluster.log(f"  Relaunching {exp['name']} on {exp['node']}")
                    cluster.launch_experiment(exp['node'], exp['name'], exp['config'], exp['dir'], zsim_bin)
            else:
                cluster.log("Checking experiment status before launching...")
                for exp in batch:
                    # Check if experiment is already running or completed
                    status = cluster.check_experiment_status(exp['node'], exp['name'], exp['dir'])

                    if status['status'] == 'completed':
                        cluster.log(f"  {exp['name']}: already completed, skipping")
                        exp['status'] = status
                    elif status['status'] in ['running', 'starting']:
                        cluster.log(f"  {exp['name']}: already running on {exp['node']}, skipping launch")
                        exp['status'] = status
                    else:
                        # Not running, need to launch
                        cluster.log(f"  Launching {exp['name']} on {exp['node']}")
                        cluster.launch_experiment(exp['node'], exp['name'], exp['config'], exp['dir'], zsim_bin)
        else:
            cluster.log(f"Launching batch {batch_idx} experiments...")
            for exp in batch:
                cluster.launch_experiment(exp['node'], exp['name'], exp['config'], exp['dir'], zsim_bin)

        # Parse max instructions from config files for this batch
        exp_max_instrs = {}
        for exp in batch:
            config_file = exp['dir'] / "config.cfg"
            max_instrs = parse_max_instrs_from_config(config_file)
            exp_max_instrs[exp['name']] = max_instrs

        # Monitor batch until completion
        print()
        cluster.log(f"Monitoring batch {batch_idx} experiments...")
        print()  # Extra newline before progress display

        all_completed = False
        num_lines_printed = 0

        while not all_completed:
            all_completed = True
            status_summary = {}

            # Update status for batch experiments
            for exp in batch:
                # Consider both 'completed' and 'failed' as terminal states
                current_status = exp['status'].get('status') if isinstance(exp['status'], dict) else exp['status']
                if current_status not in ['completed', 'failed', 'error']:
                    status_info = cluster.check_experiment_status(exp['node'], exp['name'], exp['dir'])
                    exp['status'] = status_info

                    # Still monitoring if not in a terminal state
                    if status_info['status'] not in ['completed', 'failed', 'error']:
                        all_completed = False

                # Count by status
                status_key = exp['status']['status'] if isinstance(exp['status'], dict) else exp['status']
                status_summary[status_key] = status_summary.get(status_key, 0) + 1

            # Move cursor up to overwrite previous output (if not first iteration)
            if num_lines_printed > 0 and sys.stdout.isatty():
                sys.stdout.write(f"\033[{num_lines_printed}A")
                sys.stdout.flush()

            # Clear lines and rewrite
            lines = []
            timestamp = datetime.now().strftime("%H:%M:%S")

            # Summary line
            summary_str = ", ".join(f"{k}: {v}" for k, v in sorted(status_summary.items()))
            lines.append(f"[{timestamp}] Batch {batch_idx}/{len(batches)}: {summary_str}")

            # Detailed progress for each experiment
            for exp in batch:
                status_info = exp['status']
                if isinstance(status_info, dict):
                    status = status_info['status']

                    if status == 'running':
                        cycles = status_info.get('current_cycles', 0)
                        speed = status_info.get('speed', 0)

                        # Get progress from h5 file
                        h5_file = exp['dir'] / "zsim.h5"
                        max_instrs = exp_max_instrs.get(exp['name'])
                        current_instrs, progress_pct, instrs_str = get_simulation_progress(h5_file, max_instrs)

                        # Color code based on speed
                        if speed is not None and speed > 0:
                            if speed >= 0.5:
                                speed_color = Colors.GREEN
                            else:
                                speed_color = Colors.PINK
                            speed_str = Colors.colorize(f"{speed:.2f} Mcycles/sec", speed_color)

                            # Progress string
                            if progress_pct > 0:
                                progress_str = f" ({instrs_str} instrs, {progress_pct:.0f}%)"
                            else:
                                progress_str = f" ({instrs_str} instrs)"

                            lines.append(f"  {exp['name']}: {cycles/1e6:.1f}M cycles @ {speed_str}{progress_str}")
                        else:
                            speed_str = Colors.colorize("0.00 Mcycles/sec", Colors.RED)
                            lines.append(f"  {exp['name']}: {cycles/1e6:.1f}M cycles @ {speed_str} (calculating...)")

                    elif status == 'starting':
                        heartbeat_file = exp['dir'] / "heartbeat"
                        if heartbeat_file.exists():
                            lines.append(f"  {exp['name']}: {Colors.colorize('initializing', Colors.YELLOW)} (heartbeat found)")
                        else:
                            lines.append(f"  {exp['name']}: {Colors.colorize('starting', Colors.YELLOW)} (waiting for heartbeat)")

                    elif status == 'stalled':
                        age = status_info.get('age', 0)
                        cycles = status_info.get('current_cycles', 0)
                        lines.append(f"  {exp['name']}: {Colors.colorize('STALLED', Colors.RED)} at {cycles/1e6:.1f}M cycles (heartbeat age: {age:.0f}s)")

                    elif status == 'completed':
                        lines.append(f"  {exp['name']}: {Colors.colorize('✓ completed', Colors.GREEN)}")

                    elif status == 'failed':
                        error_msg = status_info.get('message', 'no output file')
                        lines.append(f"  {exp['name']}: {Colors.colorize('✗ FAILED', Colors.RED)} ({error_msg})")

                    elif status == 'error':
                        error_msg = status_info.get('message', 'unknown error')
                        lines.append(f"  {exp['name']}: {Colors.colorize('✗ ERROR', Colors.RED)} ({error_msg})")

            # Print all lines (clearing to end of line for each)
            for line in lines:
                if sys.stdout.isatty():
                    print(f"\r{line}\033[K")  # \r to start of line, \033[K to clear to end
                else:
                    print(line)

            num_lines_printed = len(lines)
            sys.stdout.flush()

            if not all_completed:
                time.sleep(60)

        # Don't overwrite the last monitoring state - it's useful for debugging
        # Just print the final status summary
        print()  # Final newline after last monitoring output
        print()

        # Detect and report any crashed/failed experiments in this batch
        failed_experiments = []
        for exp in batch:
            if isinstance(exp['status'], dict):
                status = exp['status']['status']
                if status in ['failed', 'error', 'stalled']:
                    # Check for crash causes
                    crash_status, error_msg = detect_crash_cause(exp['dir'])
                    failed_experiments.append({
                        'name': exp['name'],
                        'status': status,
                        'crash_status': crash_status,
                        'error': error_msg
                    })
                elif status == 'completed':
                    # Double-check completion by looking at output
                    crash_status, error_msg = detect_crash_cause(exp['dir'])
                    if crash_status != 'completed':
                        failed_experiments.append({
                            'name': exp['name'],
                            'status': 'incomplete',
                            'crash_status': crash_status,
                            'error': error_msg
                        })

        if failed_experiments:
            print()
            cluster.log(Colors.colorize(f"Failed/Incomplete Experiments in Batch {batch_idx}:", Colors.RED))
            for exp_info in failed_experiments:
                print()
                cluster.log(f"  {Colors.colorize(exp_info['name'], Colors.YELLOW)}:")
                cluster.log(f"    Status: {exp_info['status']}")
                cluster.log(f"    Cause: {exp_info['crash_status']}")
                error_lines = exp_info['error'].split('\n')
                for line in error_lines[:10]:  # Limit to 10 lines
                    if line.strip():
                        cluster.log(f"    {line}")
            print()

        # Parse results for this batch (files are already local on shared filesystem)
        cluster.log(f"Parsing results for batch {batch_idx}...")
        batch_results = []

        for exp in batch:
            # Parse results from h5 file
            h5_file = exp['dir'] / "zsim.h5"
            metrics = parse_zsim_h5(h5_file, config_obj)

            if metrics and 'ipc' in metrics:
                ipc = metrics['ipc']
                cluster.log(f"  {exp['name']}: IPC = {ipc:.4f}")
                # Get config_label from experiment metadata
                config_label = exp.get('config_label', '')
                result_entry = {
                    'name': exp['name'],
                    'params': exp['params'],
                    'workload': exp.get('workload'),
                    'sweep': exp.get('sweep'),
                    'config_label': config_label,
                    'config_name': config_name,  # Add config name for plotting
                    'ipc': ipc
                }
                # Add all additional metrics from h5 file
                for metric_name, metric_value in metrics.items():
                    if metric_name != 'ipc':  # IPC already added
                        result_entry[metric_name] = metric_value
                batch_results.append(result_entry)
            else:
                cluster.log(f"  {exp['name']}: Could not parse metrics")

        # Add batch results to all results
        all_results.extend(batch_results)

        # Save incremental results after each batch
        results_file = output_dir / "results.json"
        with open(results_file, 'w') as f:
            json.dump(all_results, f, indent=2)
        cluster.log(f"Results saved to: {results_file}")

        # Free nodes after this batch
        print()
        cluster.log(f"Freeing nodes for batch {batch_idx}...")
        cluster.free_nodes()

        print()
        print("=" * 70)
        print(f"BATCH {batch_idx}/{len(batches)} COMPLETED")
        print("=" * 70)
        print()

    # Generate plots after all batches complete
    if all_results:
        print()
        print("=" * 70)
        print("All batches completed! Generating plots...")
        print("=" * 70)
        print()
        plot_results(all_results, output_dir, config_obj)

    return output_dir


def run_experiments_rocky8_single_config(config_obj, config_file_path, base_cfg_path_str, config_name, parent_output_dir, zsim_bin):
    """Generate experiments for a single config file (for Rocky8 cluster)

    Args:
        config_obj: Experiment configuration object
        config_file_path: Path to experiment config file
        base_cfg_path_str: Path to base config file (string)
        config_name: Short name for this config (for directory naming)
        parent_output_dir: Parent output directory
        zsim_bin: Path to zsim binary/wrapper

    Returns:
        tuple: (output_dir, all_experiments) where all_experiments is list of experiment dicts
    """

    # Load base config
    config_file_dir = Path(config_file_path).parent
    base_cfg_path = Path(base_cfg_path_str)
    if not base_cfg_path.is_absolute():
        base_cfg_path = (config_file_dir / base_cfg_path).resolve()

    if not base_cfg_path.exists():
        print(f"Error: Base config not found: {base_cfg_path}")
        sys.exit(1)

    print(f"  Config: {base_cfg_path}")
    base_config = base_cfg_path.read_text()

    # Create output directory for this config
    output_dir = parent_output_dir / config_name
    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"  Output directory: {output_dir.resolve()}")

    # Apply global knobs
    for global_knob in config_obj.GLOBAL_KNOBS:
        if '=' in global_knob:
            param, value = [x.strip() for x in global_knob.split('=', 1)]
            value = value.rstrip(';')
            is_string = '"' in value
            if is_string:
                value = value.strip('"')
            base_config = replace_param(base_config, param, value, is_string)

    # On nvidia/rocky8 machines, enable ASLR to avoid personality() requirement
    if is_nvidia_machine():
        # Insert aslr parameter into sim section
        sim_pattern = r'(sim\s*=\s*\{)'
        if re.search(sim_pattern, base_config):
            # Check if aslr already exists
            if re.search(r'\baslr\s*=', base_config):
                # Replace existing value
                base_config = re.sub(r'\baslr\s*=\s*[^;]+;', 'aslr = true;', base_config)
            else:
                # Insert new parameter
                base_config = re.sub(sim_pattern, r'\1\n    aslr = true;', base_config)

    # Get workloads if defined
    workloads = getattr(config_obj, 'WORKLOADS', None)
    if workloads is None:
        workloads = [(None, None, False)]

    # Prepare all experiments for this config
    all_experiments = []
    for workload_tuple in workloads:
        # Handle 2-tuple, 3-tuple, and 4-tuple formats
        # 4-tuple: (cmd, name, has_roi, should_plot)
        # 3-tuple: (cmd, name, has_roi) - should_plot defaults to True
        # 2-tuple: (cmd, name) - has_roi=False, should_plot=True
        if len(workload_tuple) >= 4:
            workload_cmd, workload_name, has_roi, workload_should_plot = workload_tuple[:4]
        elif len(workload_tuple) == 3:
            workload_cmd, workload_name, has_roi = workload_tuple
            workload_should_plot = True
        else:
            workload_cmd, workload_name = workload_tuple
            has_roi = False
            workload_should_plot = True

        for knob in config_obj.KNOBS:
            knob_params = knob[0]
            knob_values = knob[1]
            knob_fixed = knob[2]
            knob_name = knob[3]

            for values in knob_values:
                # Extract method plot flag if present
                # Values can be: ["policy", "metric"] or ["policy", "metric", should_plot]
                if isinstance(values, (list, tuple)) and len(values) > len(knob_params):
                    # Extra element is the plot flag
                    method_should_plot = values[len(knob_params)]
                    param_values = values[:len(knob_params)]
                else:
                    method_should_plot = True
                    param_values = values

                # Overall plot flag: only plot if both method and workload are enabled
                should_plot = method_should_plot and workload_should_plot

                # Create experiment name
                config_label = substitute_sweep_name(knob_name, param_values)
                exp_name = config_label
                if workload_name:
                    exp_name = f"{workload_name}_{config_label}"

                # Create filesystem-safe directory name
                exp_dir_name = sanitize_name_for_filesystem(exp_name)
                exp_dir = output_dir / exp_dir_name
                exp_dir.mkdir(parents=True, exist_ok=True)

                # Generate config
                config_content = base_config
                if workload_cmd:
                    config_content = replace_param(config_content, "command", workload_cmd, is_string=True)

                # Set startFastForwarded based on has_roi
                # has_roi=True means use fast-forward, has_roi=False means detailed from start
                ff_value = "true" if has_roi else "false"
                config_content = inject_start_fast_forwarded(config_content, ff_value)

                config_content = generate_config(config_content, knob_params, param_values, knob_fixed)
                config_file = exp_dir / "config.cfg"
                config_file.write_text(config_content)

                # Save metadata
                metadata = {
                    'sweep': knob_name,
                    'parameters': dict(zip(knob_params, param_values)),
                    'fixed': knob_fixed,
                    'workload': workload_name,
                    'workload_cmd': workload_cmd,
                    'config_label': config_label,
                    'config_name': config_name,
                    'should_plot': should_plot,
                }
                (exp_dir / "metadata.json").write_text(json.dumps(metadata, indent=2))

                all_experiments.append({
                    'name': exp_name,
                    'dir': exp_dir,
                    'config': config_file,
                    'params': dict(zip(knob_params, param_values)),
                    'workload': workload_name,
                    'sweep': knob_name,
                    'config_label': config_label,
                    'config_name': config_name,
                    'zsim_bin': zsim_bin,
                    'should_plot': should_plot,
                })

    print(f"  Generated {len(all_experiments)} experiments for {config_name}")
    return output_dir, all_experiments


def run_experiments_rocky8(config_obj, config_file_path, max_concurrent_jobs=200, dry_run=False, results_dir=None):
    """Run experiments on Rocky8 cluster with rolling job submission

    Supports multiple BASE_CONFIGs - all experiments from all configs are submitted
    to one shared job pool (respecting max_concurrent_jobs limit). Results are
    processed independently per config.

    Args:
        config_obj: Experiment configuration object
        config_file_path: Path to experiment config file
        max_concurrent_jobs: Maximum number of concurrent SLURM jobs (across all configs)
        dry_run: If True, generate configs/scripts but don't submit jobs

    Returns:
        output_dir: Path to results directory (parent dir if multiple configs)
    """

    mode_str = "ROCKY8 CLUSTER MODE - DRY RUN" if dry_run else "ROCKY8 CLUSTER MODE"
    print("=" * 70)
    print(f"zsim Parameter Sweep - {mode_str}")
    print("=" * 70)
    print()

    # Check if BASE_CONFIG is a list or single config
    base_configs = config_obj.BASE_CONFIG
    if not isinstance(base_configs, list):
        base_configs = [base_configs]

    # Determine parent output directory
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    if results_dir is not None:
        # Resume mode: reuse existing results directory
        parent_output_dir = Path(results_dir)
        print(f"Resuming from existing results directory: {parent_output_dir.resolve()}")
    else:
        if config_obj.OUTPUT_DIR == "results":
            parent_output_dir = Path(f"results_rocky8_{timestamp}")
        else:
            parent_output_dir = Path(config_obj.OUTPUT_DIR)

    parent_output_dir.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {parent_output_dir.resolve()}")
    print()

    # Save experiment metadata
    exp_metadata = {
        'config_file': str(Path(config_file_path).resolve()),
        'execution_mode': 'rocky8',
        'timestamp': timestamp,
        'experiment_name': getattr(config_obj, 'EXPERIMENT_NAME', None),
        'max_concurrent_jobs': max_concurrent_jobs,
        'base_configs': base_configs,
    }
    (parent_output_dir / "experiment_metadata.json").write_text(json.dumps(exp_metadata, indent=2))

    # Create zsim wrapper (always use wrapper for rocky8)
    print("Creating zsim wrapper script...")
    zsim_bin = create_zsim_wrapper(parent_output_dir)

    if is_nvidia_machine():
        print("Enabling ASLR (nvidia/rocky8 machine without personality() capability)")
    print()

    # Generate experiments for all configs
    print(f"Generating experiments for {len(base_configs)} config(s)...")
    print()

    all_experiments = []
    config_output_dirs = []  # Track output dirs for each config for results processing

    for i, base_cfg_str in enumerate(base_configs, 1):
        config_path = Path(base_cfg_str)
        config_name = config_path.stem

        print(f"Config {i}/{len(base_configs)}: {config_name}")

        output_dir, experiments = run_experiments_rocky8_single_config(
            config_obj, config_file_path, base_cfg_str, config_name,
            parent_output_dir, zsim_bin
        )

        config_output_dirs.append((config_name, output_dir, len(experiments)))
        all_experiments.extend(experiments)
        print()

    print("=" * 70)
    print(f"Total experiments across all configs: {len(all_experiments)}")
    print(f"Max concurrent jobs: {max_concurrent_jobs}")
    print("=" * 70)
    print()

    # Initialize Rocky8 cluster manager (using parent dir for status/logs)
    cluster = Rocky8ClusterManager(parent_output_dir, max_concurrent_jobs=max_concurrent_jobs)

    if dry_run:
        print("=" * 70)
        print("DRY RUN MODE - Configuration Summary")
        print("=" * 70)
        print(f"Generated {len(all_experiments)} experiment configurations")
        print(f"Configs saved to: {parent_output_dir.resolve()}")
        print()

        # Generate submit.sh scripts for all experiments
        print("Generating job submission scripts...")
        for exp in all_experiments:
            cluster.submit_job(exp, dry_run=True)

        print()
        print("Sample experiments:")
        for i, exp in enumerate(all_experiments[:5]):
            print(f"  {i+1}. {exp['name']} (config: {exp.get('config_name', 'N/A')})")
            print(f"     Dir: {exp['dir']}")
            print(f"     Config: {exp['config']}")
            print(f"     Submit script: {exp['dir']}/submit.sh")
        if len(all_experiments) > 5:
            print(f"  ... and {len(all_experiments) - 5} more")
        print()
        print("To actually run these experiments, remove --dry-run flag")
        print("=" * 70)
        return parent_output_dir

    # Run ALL experiments with rolling submission (pooled across all configs)
    print("Submitting all experiments to shared job pool...")
    completed_experiments, failed_experiments = cluster.run_experiments_rolling(all_experiments, zsim_bin, dry_run=dry_run)

    # Parse results PER CONFIG (independent processing)
    print()
    print("=" * 70)
    print("Processing results per config...")
    print("=" * 70)
    print()

    for config_name, output_dir, num_experiments in config_output_dirs:
        print(f"Config: {config_name}")
        cluster.log(f"Parsing results for {config_name}...")

        # Filter completed experiments for this config
        config_experiments = [exp for exp in completed_experiments if exp.get('config_name') == config_name]

        all_results = []
        for exp in config_experiments:
            h5_file = exp['dir'] / "zsim.h5"
            metrics = parse_zsim_h5(h5_file, config_obj)

            if metrics and 'ipc' in metrics:
                ipc = metrics['ipc']
                cluster.log(f"  {exp['name']}: IPC = {ipc:.4f}")

                result_entry = {
                    'name': exp['name'],
                    'params': exp['params'],
                    'workload': exp.get('workload'),
                    'sweep': exp.get('sweep'),
                    'config_label': exp.get('config_label', ''),
                    'config_name': config_name,
                    'ipc': ipc
                }
                # Add all additional metrics
                for metric_name, metric_value in metrics.items():
                    if metric_name != 'ipc':
                        result_entry[metric_name] = metric_value
                all_results.append(result_entry)
            else:
                cluster.log(f"  {exp['name']}: Could not parse metrics")

        # Save results for this config
        results_file = output_dir / "results.json"
        with open(results_file, 'w') as f:
            json.dump(all_results, f, indent=2)
        cluster.log(f"Results saved to: {results_file}")

        # Generate plots for this config
        if all_results:
            print()
            cluster.log(f"Generating plots for {config_name}...")
            plot_results(all_results, output_dir, config_obj)

        print(f"Completed processing for {config_name}: {len(all_results)}/{num_experiments} successful")
        print()

    return parent_output_dir


def find_latest_results(config_file):
    """Find the latest results directory for a given experiment config"""
    config_path = Path(config_file).resolve()

    # Look for results directories in current directory
    results_dirs = []
    for d in Path('.').glob('results_*'):
        if not d.is_dir():
            continue

        # Check if this results directory is for our config
        metadata_file = d / "experiment_metadata.json"
        if metadata_file.exists():
            try:
                metadata = json.loads(metadata_file.read_text())
                if Path(metadata.get('config_file', '')).resolve() == config_path:
                    # Get timestamp from directory name
                    timestamp_str = d.name.split('_')[-1]
                    results_dirs.append((d, timestamp_str))
            except:
                pass

    if not results_dirs:
        return None

    # Sort by timestamp and return latest
    results_dirs.sort(key=lambda x: x[1], reverse=True)
    return results_dirs[0][0]

def load_results_metadata(results_dir):
    """Load experiment metadata from results directory"""
    metadata_file = results_dir / "experiment_metadata.json"
    if not metadata_file.exists():
        return None

    try:
        return json.loads(metadata_file.read_text())
    except:
        return None

def get_should_plot_from_config(config_obj, params, workload):
    """Determine if an experiment should be plotted based on config_obj KNOBS and workloads

    Args:
        config_obj: Experiment configuration object
        params: Dict of parameter names to values (e.g., {'migrationPolicy': 'freqBased', 'migrationMetric': 'NA'})
        workload: Workload name (e.g., 'bc')

    Returns:
        bool: True if should plot, False otherwise
    """
    # Find workload plot flag
    workload_should_plot = True  # Default if workload not found

    # Try WORKLOADS first (flattened list), then WORKLOAD_GROUPS (dict of groups)
    workload_list = []
    if hasattr(config_obj, 'WORKLOADS'):
        workload_list = config_obj.WORKLOADS
    elif hasattr(config_obj, 'WORKLOAD_GROUPS'):
        # WORKLOAD_GROUPS is a dict, iterate through values
        if isinstance(config_obj.WORKLOAD_GROUPS, dict):
            for group in config_obj.WORKLOAD_GROUPS.values():
                workload_list.extend(group)
        else:
            # List of groups
            for group in config_obj.WORKLOAD_GROUPS:
                workload_list.extend(group)

    for workload_tuple in workload_list:
        # Handle different workload tuple formats
        if isinstance(workload_tuple, str):
            # Single string - just the workload name
            wl_name = workload_tuple
            wl_plot = True
        elif isinstance(workload_tuple, (list, tuple)):
            if len(workload_tuple) >= 4:
                _, wl_name, _, wl_plot = workload_tuple[:4]
            elif len(workload_tuple) == 3:
                _, wl_name, _ = workload_tuple
                wl_plot = True
            elif len(workload_tuple) == 2:
                _, wl_name = workload_tuple
                wl_plot = True
            elif len(workload_tuple) == 1:
                wl_name = workload_tuple[0]
                wl_plot = True
            else:
                continue
        else:
            continue

        if wl_name == workload:
            workload_should_plot = wl_plot
            break

    # Find method plot flag from KNOBS
    method_should_plot = True  # Default if not found
    if hasattr(config_obj, 'KNOBS'):
        for knob in config_obj.KNOBS:
            knob_params = knob[0]
            knob_values = knob[1]

            # Check if this knob's parameters match our experiment params
            for values in knob_values:
                # Extract method plot flag if present
                if isinstance(values, (list, tuple)) and len(values) > len(knob_params):
                    method_plot_flag = values[len(knob_params)]
                    param_values = values[:len(knob_params)]
                else:
                    method_plot_flag = True
                    param_values = values

                # Check if params match
                if not isinstance(param_values, (list, tuple)):
                    param_values = [param_values]

                if not isinstance(knob_params, (list, tuple)):
                    knob_params = [knob_params]

                # Build dict of this configuration's params
                config_params = dict(zip(knob_params, param_values))

                # Check if all config params match our experiment params
                if all(params.get(k) == v for k, v in config_params.items()):
                    method_should_plot = method_plot_flag
                    break

    return method_should_plot and workload_should_plot


def process_results_only(results_dir, config_obj):
    """Regenerate plots and metrics from existing results without running experiments

    If h5 files exist, re-parse them to get updated metrics.
    Otherwise, use existing results.json.
    """
    print(f"Processing results from: {results_dir.resolve()}")
    print()

    # Try to re-parse from h5 files first
    all_results = []
    h5_files_found = False

    # Check if this is a multi-config results directory (has subdirectories that are config names)
    # Structure: results_dir/config_name/exp_dir/ OR results_dir/exp_dir/
    potential_config_dirs = []
    has_multi_config = False

    # First pass: check if we have config subdirectories
    for item in sorted(results_dir.iterdir()):
        if not item.is_dir():
            continue
        # Check if this looks like a config directory (contains experiment subdirectories)
        # Note: item.iterdir() returns full Path objects, so use subdir directly
        has_exp_subdirs = any(subdir.is_dir() and (subdir / "metadata.json").exists()
                             for subdir in item.iterdir())
        if has_exp_subdirs:
            has_multi_config = True
            potential_config_dirs.append(item)

    if has_multi_config:
        # Multi-config structure: iterate over config directories
        search_dirs = [(config_dir, config_dir.name) for config_dir in potential_config_dirs]
    else:
        # Single config or flat structure
        search_dirs = [(results_dir, 'default')]

    # Look for experiment directories with h5 files
    for search_dir, config_name in search_dirs:
        for exp_dir in sorted(search_dir.iterdir()):
            if not exp_dir.is_dir():
                continue

            h5_file = exp_dir / "zsim.h5"
            metadata_file = exp_dir / "metadata.json"

            if h5_file.exists() and metadata_file.exists():
                h5_files_found = True
                try:
                    # Load metadata
                    metadata = json.loads(metadata_file.read_text())

                    # Parse h5 file
                    metrics = parse_zsim_h5(h5_file, config_obj)

                    if metrics and 'ipc' in metrics:
                        # Reconstruct proper experiment name (with '=' for display)
                        workload = metadata.get('workload')
                        config_label = metadata.get('config_label', '')
                        if workload and config_label:
                            exp_name = f"{workload}_{config_label}"
                        elif config_label:
                            exp_name = config_label
                        else:
                            exp_name = exp_dir.name  # Fallback to directory name

                        # Determine should_plot from config_obj (not from old metadata)
                        params = metadata.get('parameters', {})
                        should_plot = get_should_plot_from_config(config_obj, params, workload)

                        result_entry = {
                            'name': exp_name,
                            'params': params,
                            'workload': workload,
                            'sweep': metadata.get('sweep'),
                            'config_label': config_label,
                            'config_name': config_name,  # Add config name
                            'should_plot': should_plot,  # Use updated value from config_obj
                            'ipc': metrics['ipc']
                        }
                        # Add all additional metrics from h5 file
                        for metric_name, metric_value in metrics.items():
                            if metric_name != 'ipc':  # IPC already added
                                result_entry[metric_name] = metric_value
                        all_results.append(result_entry)
                except Exception as e:
                    print(f"Warning: Could not parse {exp_dir.name}: {e}")
                    import traceback
                    traceback.print_exc()

    # If no h5 files found, fall back to results.json
    if not h5_files_found:
        results_file = results_dir / "results.json"
        if not results_file.exists():
            print("Error: No h5 files or results.json found")
            return None

        try:
            all_results = json.loads(results_file.read_text())
            print("Loaded results from results.json (no h5 files found)")

            # Update should_plot flags from current config_obj
            for result in all_results:
                params = result.get('params', {})
                workload = result.get('workload')
                result['should_plot'] = get_should_plot_from_config(config_obj, params, workload)
            print(f"Updated {len(all_results)} should_plot flags from config")
        except Exception as e:
            print(f"Error loading results.json: {e}")
            return None
    else:
        print(f"Re-parsed {len(all_results)} results from h5 files")
        # Save updated results.json
        results_file = results_dir / "results.json"
        with open(results_file, 'w') as f:
            json.dump(all_results, f, indent=2)
        print(f"Updated results saved to: {results_file.resolve()}")

    print(f"Total results: {len(all_results)}")

    # Regenerate plots
    if all_results:
        print()
        print("Regenerating plots...")
        plot_results(all_results, results_dir, config_obj)

    # Print summary
    print()
    print("=" * 70)
    print("Results processing completed!")
    print(f"Results in: {results_dir.resolve()}")
    print("=" * 70)

    return results_dir

def main():
    parser = argparse.ArgumentParser(
        description="Run zsim parameter sweeps",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run experiments
  python scripts/run_sweep.py experiments/migration_study.py --local
  python scripts/run_sweep.py experiments/migration_study.py --cluster lsf
  python scripts/run_sweep.py experiments/migration_study.py --cluster rocky8

  # Resume experiments (auto-detects local or cluster)
  python scripts/run_sweep.py experiments/migration_study.py --resume
  python scripts/run_sweep.py experiments/migration_study.py --resume --results-path results_local_20260203_014535

  # Regenerate plots only (no experiments run)
  python scripts/run_sweep.py experiments/migration_study.py --results-only
  python scripts/run_sweep.py experiments/migration_study.py --results-only --results-path results_local_20260203_014535
        """
    )

    parser.add_argument('config', nargs='?', help='Experiment configuration file (.py)')

    # Mutually exclusive group for execution mode (required unless --clean)
    exec_group = parser.add_mutually_exclusive_group(required=False)
    exec_group.add_argument('--local', action='store_true',
                           help='Run locally')
    exec_group.add_argument('--cluster', type=str, choices=['lsf', 'rocky8'],
                           metavar='CLUSTER_NAME',
                           help='Run on cluster (lsf or rocky8)')

    # Resume flag
    parser.add_argument('--resume', action='store_true',
                       help='Resume from previous run (auto-detects if it was local or cluster)')

    # Results-only flag
    parser.add_argument('--results-only', action='store_true',
                       help='Only regenerate plots/metrics from existing results (no experiments run)')

    # Results path
    parser.add_argument('--results-path', type=str,
                       help='Path to results directory (auto-detects latest if not specified)')

    # Force relaunch flag (only works with --cluster --resume)
    parser.add_argument('--force-relaunch', action='store_true',
                       help='Kill existing experiments and relaunch from scratch (use with --resume)')

    # Clean flag (only works with --cluster)
    parser.add_argument('--clean', action='store_true',
                       help='Kill all running experiments and deallocate nodes')

    # Rocky8 concurrent jobs parameter
    parser.add_argument('--max-jobs', type=int, default=200,
                       help='Maximum concurrent SLURM jobs for rocky8 cluster (default: 200)')

    # Dry-run flag
    parser.add_argument('--dry-run', action='store_true',
                       help='Generate configs and scripts but do not submit/run experiments')

    args = parser.parse_args()

    # Handle --clean mode first
    if args.clean:
        if not args.cluster:
            print("Error: --clean can only be used with --cluster CLUSTER_NAME")
            sys.exit(1)
        if args.cluster == 'lsf':
            cleanup_cluster_run()
        elif args.cluster == 'rocky8':
            cleanup_rocky8_run(results_path=args.results_path)
        else:
            print(f"Error: Unknown cluster type: {args.cluster}")
            sys.exit(1)
        sys.exit(0)

    # Validate required arguments
    if not args.config:
        print("Error: config argument is required")
        parser.print_help()
        sys.exit(1)

    if not os.path.exists(args.config):
        print(f"Error: Config file not found: {args.config}")
        sys.exit(1)

    # Load config
    config_obj = load_experiment_config(args.config)

    # Handle --results-only mode
    if args.results_only:
        # Find results directory
        if args.results_path:
            results_dir = Path(args.results_path)
        else:
            results_dir = find_latest_results(args.config)
            if not results_dir:
                print(f"Error: No results directory found for {args.config}")
                print("Please specify --results-path")
                sys.exit(1)

        if not results_dir.exists():
            print(f"Error: Results directory not found: {results_dir}")
            sys.exit(1)

        process_results_only(results_dir, config_obj)
        sys.exit(0)

    # Handle --resume mode
    if args.resume:
        # Find results directory
        if args.results_path:
            results_dir = Path(args.results_path)
        else:
            results_dir = find_latest_results(args.config)
            if not results_dir:
                print(f"Error: No results directory found for {args.config}")
                print("Please specify --results-path")
                sys.exit(1)

        if not results_dir.exists():
            print(f"Error: Results directory not found: {results_dir}")
            sys.exit(1)

        # Load metadata to determine execution mode
        metadata = load_results_metadata(results_dir)
        if not metadata:
            print(f"Error: Could not load metadata from {results_dir}")
            sys.exit(1)

        exec_mode = metadata.get('execution_mode', 'local')

        # Allow user to override with --local or --cluster
        if args.local:
            exec_mode = 'local'
        elif args.cluster:
            exec_mode = args.cluster  # 'lsf' or 'rocky8'

        print(f"Resuming in {exec_mode} mode from: {results_dir.resolve()}")

        if exec_mode in ['cluster', 'lsf']:
            # Validate --force-relaunch requires --cluster and --resume
            if args.force_relaunch and not args.cluster:
                print("Warning: --force-relaunch ignored (not in cluster mode)")

            # For cluster resume, we need to check for .cluster_state.json
            if not (results_dir / '.cluster_state.json').exists():
                print("Error: No cluster state found in results directory")
                print("Cannot resume cluster run")
                sys.exit(1)

            output_dir = run_experiments_cluster(config_obj, args.config, resume=True, force_relaunch=args.force_relaunch, results_dir=results_dir)
        elif exec_mode == 'rocky8':
            output_dir = run_experiments_rocky8(
                config_obj, args.config,
                max_concurrent_jobs=args.max_jobs,
                results_dir=results_dir,
            )
        else:
            print("Error: Local resume not yet implemented")
            print("Use --results-only to regenerate plots from existing results")
            sys.exit(1)
    else:
        # Normal run mode - requires --local or --cluster
        if not args.local and not args.cluster:
            print("Error: Either --local or --cluster must be specified")
            parser.print_help()
            sys.exit(1)

        # Validate --force-relaunch requires --resume
        if args.force_relaunch:
            print("Error: --force-relaunch requires --resume")
            sys.exit(1)

        # Run experiments based on mode
        if args.cluster == 'lsf':
            output_dir = run_experiments_cluster(config_obj, args.config, resume=False, force_relaunch=False, dry_run=args.dry_run)
        elif args.cluster == 'rocky8':
            output_dir = run_experiments_rocky8(config_obj, args.config, max_concurrent_jobs=args.max_jobs, dry_run=args.dry_run)
        elif args.local:
            output_dir = run_experiments(config_obj, args.config, dry_run=args.dry_run)
        else:
            print("Error: Invalid execution mode")
            sys.exit(1)

    print()
    print("=" * 70)
    print("All experiments completed!")
    print(f"Results in: {output_dir.resolve()}")
    print("=" * 70)

if __name__ == "__main__":
    main()

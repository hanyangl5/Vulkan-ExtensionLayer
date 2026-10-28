#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: MIT
"""Bounded Windows startup diagnostics; never attach to an existing process.

Actual CTS launches require an explicit summary, exact case(s), repeat count,
and --allow-cts-launch. Refuse to launch if any deqp-vk.exe is already running.
CDB output and child output share redirected stdio: combined-stdout.txt is NOT
an uncontaminated CTS stdout measurement. The original runner files stay intact.
All generated commands, probes, executables and raw logs live in the ignored
build-rt-cts-evidence directory. No symbol server, full memory dump, installation,
IFEO/Application Verifier registry setting or production changes are used.
"""
import argparse
import ctypes
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[3]
ARTIFACTS = ROOT / "build-rt-cts-evidence"
DEFAULT_CDB = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits/10/Debuggers/x64/cdb.exe"
TARGET_STATUS = 0xC0000008
PROBE_EXCEPTION = 0xE0424242
LAYER_ENV = (
    "VK_INSTANCE_LAYERS", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_LOADER_LAYERS_ENABLE",
    "VK_LOADER_LAYERS_ALLOW", "VK_LOADER_LAYERS_DISABLE", "ENABLE_VKRT_SOFTWARE_RAY_QUERY",
    "DISABLE_VKRT_SOFTWARE_RAY_QUERY",
)
MARKER = "VKRT_STARTUP_"


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for data in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def env_hash(env):
    # Record identity, not arbitrary environment contents (which may be private).
    return hashlib.sha256(json.dumps(sorted(env.items()), ensure_ascii=True).encode()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True), encoding="utf-8")


def inside_artifacts(path):
    path = path.resolve()
    if not path.is_relative_to(ARTIFACTS.resolve()) or path == ARTIFACTS.resolve():
        raise ValueError("Output must be a new subdirectory of build-rt-cts-evidence")
    return path


class Win32:
    """Only process/thread handles created here are controlled or terminated."""
    def __init__(self):
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        for name, result, args in (
            ("CreateJobObjectW", w.HANDLE, [ctypes.c_void_p, w.LPCWSTR]),
            ("SetInformationJobObject", w.BOOL, [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]),
            ("AssignProcessToJobObject", w.BOOL, [w.HANDLE, w.HANDLE]),
            ("CloseHandle", w.BOOL, [w.HANDLE]),
            ("CreateToolhelp32Snapshot", w.HANDLE, [w.DWORD, w.DWORD]),
            ("Thread32First", w.BOOL, [w.HANDLE, ctypes.c_void_p]),
            ("Thread32Next", w.BOOL, [w.HANDLE, ctypes.c_void_p]),
            ("Process32FirstW", w.BOOL, [w.HANDLE, ctypes.c_void_p]),
            ("Process32NextW", w.BOOL, [w.HANDLE, ctypes.c_void_p]),
            ("OpenThread", w.HANDLE, [w.DWORD, w.BOOL, w.DWORD]),
            ("ResumeThread", w.DWORD, [w.HANDLE]),
            ("GetErrorMode", w.UINT, []),
            ("SetErrorMode", w.UINT, [w.UINT]),
        ):
            function = getattr(self.k, name)
            function.restype, function.argtypes = result, args
        self.k.SetErrorMode(self.k.GetErrorMode() | 0x8003)

    def job(self):
        job = self.k.CreateJobObjectW(None, None)
        if not job:
            raise ctypes.WinError(ctypes.get_last_error())
        # JOBOBJECT_EXTENDED_LIMIT_INFORMATION, Windows x64 SDK layout.
        limits = ctypes.create_string_buffer(144)
        struct.pack_into("<I", limits, 16, 0x2000)  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.k.SetInformationJobObject(job, 9, limits, len(limits)):
            error = ctypes.get_last_error()
            self.k.CloseHandle(job)
            raise ctypes.WinError(error)
        return job

    def resume_owned_initial_thread(self, pid):
        class Thread(ctypes.Structure):
            _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("tid", w.DWORD), ("pid", w.DWORD),
                        ("priority", w.LONG), ("delta", w.LONG), ("flags", w.DWORD)]
        snapshot = self.k.CreateToolhelp32Snapshot(4, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = Thread()
            entry.size = ctypes.sizeof(entry)
            more = self.k.Thread32First(snapshot, ctypes.byref(entry))
            while more:
                if entry.pid == pid:
                    thread = self.k.OpenThread(2, False, entry.tid)  # THREAD_SUSPEND_RESUME
                    if not thread:
                        raise ctypes.WinError(ctypes.get_last_error())
                    try:
                        if self.k.ResumeThread(thread) == 0xFFFFFFFF:
                            raise ctypes.WinError(ctypes.get_last_error())
                        return
                    finally:
                        self.k.CloseHandle(thread)
                more = self.k.Thread32Next(snapshot, ctypes.byref(entry))
            raise RuntimeError("Owned suspended debugger has no initial thread")
        finally:
            self.k.CloseHandle(snapshot)

    def processes(self):
        class Process(ctypes.Structure):
            _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("pid", w.DWORD), ("heap", ctypes.c_size_t),
                        ("module", w.DWORD), ("threads", w.DWORD), ("parent", w.DWORD), ("priority", w.LONG),
                        ("flags", w.DWORD), ("name", w.WCHAR * 260)]
        snapshot = self.k.CreateToolhelp32Snapshot(2, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = Process()
            entry.size = ctypes.sizeof(entry)
            more = self.k.Process32FirstW(snapshot, ctypes.byref(entry))
            found = {}
            while more:
                found[entry.pid] = entry.name
                more = self.k.Process32NextW(snapshot, ctypes.byref(entry))
            return found
        finally:
            self.k.CloseHandle(snapshot)

    def cts_running(self):
        return [pid for pid, name in self.processes().items() if name.lower() == "deqp-vk.exe"]


def hidden_options():
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    return {"creationflags": subprocess.CREATE_NO_WINDOW, "startupinfo": startup}


def capture_commands(path, handle_trace=False):
    # Explicit command-line -xe also covers an exception before the startup script.
    # Live debugging already selects the exception thread's register context.
    # .ecxr is a dump-context command and fails on this installed live engine.
    exception = ".lastevent; .exr -1; r; kv 0n48; ~* kv 0n16; lmv"
    first = f".echo {MARKER}FIRST; {exception}; gn"
    second = f".echo {MARKER}SECOND; {exception}; gn"
    lines = [".symopt+ 0x80000", f'sxd -c2 "{second}" *',
             f'sxe -c "{first}" -c2 "{second}" 0xc0000008',
             f'sxd -c "{first}" -c2 "{second}" 0xe0424242']
    if handle_trace:
        lines.append("!htrace -enable")
        first = first.removesuffix("; gn") + "; !htrace 0 0n64; gn"
        second = second.removesuffix("; gn") + "; !htrace 0 0n64; gn"
        lines += [f'sxe -c "{first}" -c2 "{second}" 0xc0000008']
    for symbol, register, kind in (
        ("kernel32!ExitProcess", "ecx", "ExitProcess"),
        ("ntdll!RtlExitUserProcess", "ecx", "RtlExitUserProcess"),
        ("kernelbase!TerminateProcess", "edx", "TerminateProcess"),
        ("ntdll!NtTerminateProcess", "edx", "NtTerminateProcess"),
    ):
        block = f".echo {MARKER}EXIT_CALL_{kind}; .lastevent; r; kv 0n48; ~* kv 0n16; lmv"
        lines.append(f'bu {symbol} ".if ((@{register} & 0xffffffff) == 0xc0000008) {{ {block}; }}; gc"')
    lines += [f'sxe -c ".echo {MARKER}PROCESS_EXIT; .lastevent; q" epr',
              f".echo {MARKER}ARMED", "g"]
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def run_one(api, cdb, command, cwd, environment, output, timeout, trace=False):
    output.mkdir()
    symbols = ";".join(dict.fromkeys([str(Path(command[0]).parent), str(output)]))
    env = environment.copy()
    env["_NT_SYMBOL_PATH"], env["_NT_ALT_SYMBOL_PATH"] = symbols, ""
    setup, transcript, combined = output / "capture.cmd", output / "debugger.log", output / "combined-stdout.txt"
    capture_commands(setup, trace)
    invocation = [str(cdb), "-y", symbols, "-logo", str(transcript), "-cf", str(setup), "-xe", "0xc0000008", *command]
    record = {"command": command, "cwd": str(cwd), "target_sha256": sha256(command[0]),
              "debugger": str(cdb), "debugger_sha256": sha256(cdb), "debugger_command": invocation,
              "original_environment_sha256": env_hash(environment), "launcher_environment_sha256": env_hash(env),
              "symbol_policy": "local directories only; _NT_SYMBOL_PATH/_NT_ALT_SYMBOL_PATH overridden",
              "stdio": "stdin NUL; stdout file; stderr same file; debugger and child output combined",
              "handle_tracing": trace, "timeout_seconds": timeout, "timed_out": False}
    job = api.job()
    process = None
    start = time.monotonic()
    try:
        with combined.open("wb") as stdout:
            options = hidden_options()
            options["creationflags"] |= 4  # CREATE_SUSPENDED: assign job before ANY child can be created.
            process = subprocess.Popen(invocation, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                       stdout=stdout, stderr=subprocess.STDOUT, **options)
            if not api.k.AssignProcessToJobObject(job, int(process._handle)):
                error = ctypes.get_last_error()
                process.kill()  # Only our still-suspended debugger; it has no target child yet.
                process.wait(timeout=5)
                raise ctypes.WinError(error)
            api.resume_owned_initial_thread(process.pid)
            try:
                record["debugger_exit_code"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                record["timed_out"] = True
                api.k.CloseHandle(job)  # Atomically terminate this owned job tree, never a process-name match.
                job = None
                record["debugger_exit_code"] = process.wait(timeout=5)
    finally:
        if job:
            api.k.CloseHandle(job)
        if process is not None and process.poll() is None:
            process.wait(timeout=5)
    record["elapsed_seconds"] = round(time.monotonic() - start, 4)
    text = transcript.read_text(encoding="utf-8", errors="replace") if transcript.exists() else ""
    # CDB echoes startup commands too: only standalone .echo output is an event.
    record["events"] = re.findall(r"^VKRT_STARTUP_(FIRST|SECOND|EXIT_CALL_\w+|PROCESS_EXIT|ARMED)\s*$", text, re.MULTILINE)
    record["process_exit_statuses"] = re.findall(r"Exit process .*?code\s+([0-9a-fA-F`]+)", text, re.IGNORECASE)
    record["combined_stdout_bytes"] = combined.stat().st_size
    record["debugger_log_bytes"] = transcript.stat().st_size if transcript.exists() else 0
    record["contains_exception_record"] = "ExceptionCode:" in text
    record["contains_registers"] = re.search(r"\brip=[0-9a-f]+", text) is not None
    record["contains_module_listing"] = "start" in text and "module name" in text
    record["contains_probe_exception_parameters"] = all(v in text for v in ("NumberParameters: 2", "11223344", "55667788"))
    record["exception_threads"] = re.findall(r"Last event:\s+([0-9a-f]+\.[0-9a-f]+):", text, re.IGNORECASE)
    record["stack_threads"] = sorted(set(re.findall(r"\bId:\s+([0-9a-f]+\.[0-9a-f]+)\s+Suspend:", text, re.IGNORECASE)))
    record["module_load_events"] = [line for line in text.splitlines() if line.startswith(("ModLoad:", "ModUnload:"))]
    write_json(output / "capture.json", record)
    return record


PROBE_SOURCE = r'''
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef LONG (NTAPI *NtTerminate)(HANDLE, LONG);
typedef void (NTAPI *RtlExit)(LONG);
static HANDLE ready;
static DWORD WINAPI worker(void *event) {
    SetEvent(ready);
    WaitForSingleObject(event, INFINITE);
    return 0;
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "handled";
    ULONG_PTR parameters[2] = {0x11223344, 0x55667788};
    HANDLE event = CreateEventW(0, TRUE, FALSE, 0);
    ready = CreateEventW(0, TRUE, FALSE, 0);
    if (!event || !ready || !CreateThread(0, 0, worker, event, 0, 0) ||
        WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0) return 4;
    puts("CPU_PROBE_ENTER"); fflush(stdout);
    if (!strcmp(mode, "handled")) {
        __try { RaiseException(0xe0424242, 0, 2, parameters); }
        __except (GetExceptionCode() == 0xe0424242 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            puts("CPU_PROBE_HANDLED"); fflush(stdout);
        }
        return 0;
    }
    if (!strcmp(mode, "unhandled")) RaiseException(0xe0424242, 0, 2, parameters);
    if (!strcmp(mode, "hang")) Sleep(INFINITE);
    if (!strcmp(mode, "exit")) ExitProcess(0xc0000008);
    if (!strcmp(mode, "rtl-exit")) ((RtlExit)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserProcess"))(0xc0000008);
    if (!strcmp(mode, "terminate")) TerminateProcess(GetCurrentProcess(), 0xc0000008);
    if (!strcmp(mode, "ntterminate")) ((NtTerminate)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtTerminateProcess"))(GetCurrentProcess(), 0xc0000008);
    if (!strcmp(mode, "invalid-close")) {
        BOOL result = CloseHandle((HANDLE)(ULONG_PTR)0x12345678);
        printf("CPU_PROBE_CLOSE=%d ERROR=%lu\n", result, GetLastError()); fflush(stdout);
        return result ? 2 : 0;
    }
    return 3;
}
'''


def self_test(api, args, output):
    source, executable = output / "cpu_probe.c", output / "cpu_probe.exe"
    source.write_text(PROBE_SOURCE, encoding="ascii")
    compiler = shutil.which("clang.exe")
    if not compiler:
        raise RuntimeError("Self-test needs the existing Windows clang.exe; no installation is performed")
    compile_command = [compiler, "-std=c11", "-O0", "-gcodeview", "-fms-extensions", str(source),
                       "-fuse-ld=lld", "-Wl,/debug", "-o", str(executable)]
    with (output / "compile.log").open("wb") as log:
        compiled = subprocess.run(compile_command, cwd=output, stdin=subprocess.DEVNULL, stdout=log,
                                  stderr=subprocess.STDOUT, timeout=60, **hidden_options())
    if compiled.returncode:
        raise RuntimeError("CPU probe compilation failed; see compile.log")
    records = []
    for mode in ("handled", "unhandled", "exit", "rtl-exit", "terminate", "ntterminate", "invalid-close"):
        record = run_one(api, args.cdb, [str(executable), mode], output, os.environ.copy(), output / mode, args.timeout)
        record["probe_mode"] = mode
        records.append(record)
        print(json.dumps({"probe": mode, "events": record["events"], "timeout": record["timed_out"]}), flush=True)
    with (output / "invalid-close-plain.txt").open("wb") as stdout:
        plain = subprocess.run([str(executable), "invalid-close"], cwd=output, stdin=subprocess.DEVNULL,
                               stdout=stdout, stderr=subprocess.STDOUT, timeout=args.timeout, **hidden_options())
    deadline = run_one(api, args.cdb, [str(executable), "hang"], output, os.environ.copy(), output / "bounded-timeout", min(args.timeout, 10))
    deadline_text = (output / "bounded-timeout/debugger.log").read_text(errors="replace")
    target_pids = {int(pid, 16) for pid in re.findall(r"^\(([0-9a-fA-F]+)\.[0-9a-fA-F]+\):", deadline_text, re.MULTILINE)}
    remaining = target_pids.intersection(api.processes())
    checks = {
        "all_armed_and_bounded": all("ARMED" in r["events"] and not r["timed_out"] for r in records),
        "handled_first_without_second": "FIRST" in records[0]["events"] and "SECOND" not in records[0]["events"],
        "handled_reaches_handler": "CPU_PROBE_HANDLED" in (output / "handled/combined-stdout.txt").read_text(errors="replace"),
        "unhandled_first_and_second": all(e in records[1]["events"] for e in ("FIRST", "SECOND")),
        "exit_call_detected": "EXIT_CALL_ExitProcess" in records[2]["events"],
        "rtl_exit_call_detected": "EXIT_CALL_RtlExitUserProcess" in records[3]["events"],
        "terminate_call_detected": "EXIT_CALL_TerminateProcess" in records[4]["events"],
        "ntterminate_call_detected": "EXIT_CALL_NtTerminateProcess" in records[5]["events"],
        "exceptions_have_context": all(r["contains_exception_record"] and r["contains_registers"] and
                                       r["contains_module_listing"] for r in records[:2]),
        "exception_parameters_preserved": all(r["contains_probe_exception_parameters"] for r in records[:2]),
        "other_thread_stack_captured": all(len(r["stack_threads"]) >= 2 for r in records[:2]),
        "exit_statuses_preserved": records[0]["process_exit_statuses"] == ["0"] and
                                   records[1]["process_exit_statuses"] == ["e0424242"] and
                                   all(r["process_exit_statuses"] == ["c0000008"] for r in records[2:6]),
        "explicit_exit_is_not_exception": all("FIRST" not in r["events"] and "SECOND" not in r["events"] for r in records[2:6]),
        "deadline_terminates_owned_target": deadline["timed_out"] and "ARMED" in deadline["events"] and
                                             bool(target_pids) and not remaining,
        "plain_invalid_close_is_not_cts_evidence": plain.returncode == 0,
    }
    summary = {"kind": "CPU-only synthetic calibration; NOT CTS/GPU evidence", "checks": checks,
               "runs": records, "plain_invalid_close_exit": plain.returncode, "deadline_run": deadline,
               "deadline_target_pids_observed": sorted(target_pids), "deadline_target_pids_remaining": sorted(remaining),
               "warning": "Debugger-only invalid CloseHandle behavior cannot identify the original CTS cause."}
    write_json(output / "summary.json", summary)
    if not all(checks.values()):
        raise RuntimeError("Calibration failed: " + ", ".join(k for k, ok in checks.items() if not ok))


def cts_runs(api, args, output):
    if not args.allow_cts_launch or not args.summary or not args.case or args.repeats is None:
        raise ValueError("CTS requires --allow-cts-launch --summary --case EXACT_CASE --repeats N")
    running = api.cts_running()
    if running:
        raise RuntimeError(f"Refusing to launch while existing deqp-vk.exe process(es) run: {running}")
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    selected = []
    for name in args.case:
        matches = [case for case in summary["cases"] if case["case"] == name]
        if len(matches) != 1:
            raise ValueError(f"Case must match exactly once in the supplied summary: {name}")
        selected.append(matches[0])
    env = os.environ.copy()
    for key in LAYER_ENV:
        env.pop(key, None)
    env.update(summary["environment"]["effective_driver_environment"])
    layer = Path(summary["layer"]["snapshot_library"])
    expected_hash = summary["layer"]["library_sha256"]
    if sha256(layer) != expected_hash:
        raise ValueError("Layer DLL no longer matches the immutable original summary")
    cts = Path(summary["cts"])
    expected_cts = summary["cts_sha256"]
    if sha256(cts) != expected_cts:
        raise ValueError("CTS executable no longer matches the immutable original summary")
    runs = []
    write_json(output / "source-summary.json", summary)
    for iteration in range(args.repeats):
        for index, case in enumerate(selected):
            directory = output / f"{iteration + 1:02d}-{index + 1:02d}"
            command = [f"--deqp-log-filename={directory / 'case.qpa'}" if arg.startswith("--deqp-log-filename=") else arg
                       for arg in case["command"]]
            if Path(command[0]).name.lower() != "deqp-vk.exe":
                raise ValueError("Summary target must be deqp-vk.exe")
            if Path(command[0]).resolve() != cts.resolve() or sha256(command[0]) != expected_cts:
                raise ValueError("Case target differs from the original summary CTS executable")
            if api.cts_running():
                raise RuntimeError("Another CTS process started; refusing concurrent GPU use")
            if sha256(layer) != expected_hash:
                raise ValueError("Layer DLL changed during diagnostics")
            record = run_one(api, args.cdb, command, Path(case["cwd"]), env, directory, args.timeout, args.handle_trace)
            record.update(case=case["case"], original_stdout=case["stdout"], original_qpa=case["qpa"],
                          layer_sha256=expected_hash, changed_arguments=["--deqp-log-filename"])
            write_json(directory / "capture.json", record)
            runs.append(record)
            write_json(output / "summary.json", {"kind": "startup diagnostic, not a CTS pass matrix", "runs": runs})
            print(json.dumps({"case": case["case"], "iteration": iteration + 1, "events": record["events"]}), flush=True)
            if any(e in record["events"] for e in ("FIRST", "SECOND")) or any(e.startswith("EXIT_CALL_") for e in record["events"]):
                return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cdb", type=Path, default=DEFAULT_CDB)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--self-test", action="store_true", help="Compile/run only the embedded CPU probes")
    parser.add_argument("--allow-cts-launch", action="store_true")
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--case", action="append")
    parser.add_argument("--repeats", type=int, choices=range(1, 11))
    parser.add_argument("--handle-trace", action="store_true", help="Optional, more intrusive process-local handle history")
    args = parser.parse_args()
    if os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8:
        parser.error("Windows x64 is required")
    if not 1 <= args.timeout <= 120:
        parser.error("--timeout must be finite and between 1 and 120 seconds")
    if args.case and len(args.case) > 8:
        parser.error("At most eight explicit cases may be selected")
    if args.case and args.repeats and len(args.case) * args.repeats > 32:
        parser.error("At most 32 explicitly requested launches per invocation")
    if not args.cdb.is_file():
        parser.error("Existing x64 cdb.exe not found; specify --cdb")
    if args.self_test and (args.allow_cts_launch or args.summary or args.case or args.repeats):
        parser.error("--self-test cannot be combined with CTS launch arguments")
    output = inside_artifacts(args.output)
    output.mkdir(parents=True, exist_ok=False)
    write_json(output / "harness.json", {"script": str(Path(__file__).resolve()), "sha256": sha256(__file__)})
    api = Win32()
    if args.self_test:
        self_test(api, args, output)
    else:
        cts_runs(api, args, output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"startup capture: {error}", file=sys.stderr)
        sys.exit(1)

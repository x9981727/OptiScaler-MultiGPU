"""Bounded XeFG LAB controller. No game, driver, resolution or FPS-setting writes.
The optional control moves one output within a two-output period. This is not a
shipping DXGI implementation. PresentMon data are display events, not optical
measurements. Only the known 2.5.1 legacy QPCTime-seconds schema is accepted.
"""
from __future__ import annotations
import argparse
import bisect
import collections
import csv
import ctypes
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import queue
import statistics
import subprocess
import threading
import time


@dataclass(frozen=True)
class Frame:
    id: int
    present_ms: float
    display_ms: float
    period_ms: float
    phase_ms: float
    mode: str


class MidpointController:
    """Observe odd/even/odd displayed times, then propose a bounded phase step.

    A proposal is not applied internally until the control-file write succeeds.
    Thus a sharing violation cannot leave the observer waiting for an unapplied
    phase. No observation may be reused across a command generation.
    """
    def __init__(self) -> None:
        self.phase = 0.0
        self.command_ms = -math.inf
        self.last_valid_ms = -math.inf
        self.frames: collections.deque[Frame] = collections.deque(maxlen=3)
        self.samples: collections.deque[tuple[float, float]] = collections.deque(maxlen=96)
        self.invalid = collections.Counter()
        self.last_proposal: dict | None = None

    def observe(self, f: Frame, now_ms: float) -> dict | None:
        values = (f.present_ms, f.display_ms, f.period_ms, f.phase_ms, now_ms)
        if not all(math.isfinite(x) for x in values) or not 2 <= f.period_ms <= 100:
            self.invalid['nonfinite_or_period'] += 1
            self.frames.clear()
            return None
        if not 0 <= now_ms - f.present_ms <= 2500 or f.display_ms < f.present_ms:
            self.invalid['stale_or_invalid_clock'] += 1
            self.frames.clear()
            return None
        if self.frames and f.id != self.frames[-1].id + 1:
            self.invalid['sequence_gap'] += 1
            self.frames.clear()
        self.frames.append(f)
        if len(self.frames) != 3:
            return None
        left, mid, right = self.frames
        if mid.id % 2 != 0:
            return None
        if len({x.mode for x in self.frames}) != 1 or not f.mode.startswith('Hardware'):
            self.invalid['display_mode'] += 1
            self.samples.clear()
            return None
        if max(x.period_ms for x in self.frames) / min(x.period_ms for x in self.frames) > 1.03:
            self.invalid['period_transition'] += 1
            self.samples.clear()
            return None
        if abs(mid.phase_ms - self.phase) > 0.015 or mid.present_ms < self.command_ms + 200:
            self.invalid['old_generation'] += 1
            return None
        span = right.display_ms - left.display_ms
        if not left.display_ms < mid.display_ms < right.display_ms or not .75*f.period_ms < span < 1.25*f.period_ms:
            self.invalid['stall_or_reorder'] += 1
            return None
        residual = mid.display_ms - (left.display_ms + right.display_ms)*.5
        self.samples.append((mid.present_ms, residual))
        self.last_valid_ms = now_ms
        fresh = [v for t, v in self.samples if mid.present_ms - t <= 1600]
        if len(fresh) < 24 or now_ms - self.command_ms < 1000:
            return None
        residual = statistics.median(fresh)
        mad = statistics.median(abs(x-residual) for x in fresh)
        if mad > .65:
            self.invalid['unstable_observation'] += 1
            return None
        bound = min(4.0, f.period_ms*.24)
        step = max(-.35, min(.35, -.45*residual))
        proposed = max(-bound, min(bound, self.phase + step))
        report = {'old_phase_ms': self.phase, 'new_phase_ms': proposed,
                  'midpoint_error_ms': residual, 'mad_ms': mad, 'samples': len(fresh),
                  'last_native_id': f.id, 'observed_qpc_ms': mid.present_ms,
                  'bound_ms': bound}
        self.last_proposal = report
        if abs(residual) < .12 or abs(proposed-self.phase) < .025:
            return None
        return report

    def commit(self, proposal: dict, now_ms: float) -> None:
        if abs(proposal['old_phase_ms']-self.phase) > 1e-9:
            raise ValueError('Obsolete proposal')
        self.phase = proposal['new_phase_ms']
        self.command_ms = now_ms
        self.samples.clear()


class NativeIndex:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.offset = 0
        self.records: list[tuple[int, float, float, float]] = []
        self.times: list[float] = []
        self.malformed = 0

    def refresh(self) -> None:
        if not self.path.exists():
            return
        with self.path.open('rb') as stream:
            stream.seek(self.offset)
            while True:
                start = stream.tell()
                line = stream.readline()
                if not line or not line.endswith(b'\n'):
                    stream.seek(start)
                    break
                try:
                    i, t, p, h, result = line.decode('ascii').strip().split(',')
                    record = (int(i), float(t), float(p), float(h))
                    if int(result) != 0:
                        continue
                    if not all(math.isfinite(v) for v in record[1:]) or (self.times and record[1] <= self.times[-1]):
                        raise ValueError('Invalid native clock')
                    self.records.append(record)
                    self.times.append(record[1])
                except (ValueError, UnicodeError):
                    self.malformed += 1
            self.offset = stream.tell()
        if len(self.times) > 40000:
            del self.times[:10000]
            del self.records[:10000]

    def match(self, when: float) -> tuple[int, float, float, float] | None:
        j = bisect.bisect_left(self.times, when)
        ids = [i for i in (j-1,j) if 0 <= i < len(self.times)]
        if not ids:
            return None
        i = min(ids, key=lambda x: abs(self.times[x]-when))
        return self.records[i] if abs(self.times[i]-when) <= .5 else None


def atomic_control(out: Path, phase: float, clock) -> float:
    if not math.isfinite(phase) or abs(phase) > 4:
        raise ValueError('Unsafe phase')
    target = out/'phase-control.txt'
    pending = out/f'phase-control.{os.getpid()}.pending'
    for attempt in range(4):
        stamp = clock()
        pending.write_text(f'1 {phase:.8f} {stamp:.6f}\n', encoding='ascii')
        try:
            os.replace(pending, target)
            return stamp
        except PermissionError:
            if attempt == 3:
                raise
            time.sleep(.002)
    raise RuntimeError('Unreachable')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('pid', type=int)
    parser.add_argument('seconds', type=int)
    parser.add_argument('presentmon', type=Path)
    parser.add_argument('--observe-only', action='store_true')
    args = parser.parse_args()
    if os.name != 'nt' or not 5 <= args.seconds <= 180 or args.pid <= 0 or not args.directory.is_dir() or not args.presentmon.is_file():
        parser.error('Requires Windows, existing lab directory/PresentMon and a bounded 5..180 second capture')
    out = args.directory
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    freq = ctypes.c_longlong()
    if not k.QueryPerformanceFrequency(ctypes.byref(freq)) or freq.value <= 0:
        raise OSError('QPC frequency unavailable')
    def clock() -> float:
        current = ctypes.c_longlong()
        if not k.QueryPerformanceCounter(ctypes.byref(current)):
            raise OSError('QPC read failed')
        return current.value*1000.0/freq.value
    control = MidpointController()
    index = NativeIndex(out/'native-live.csv')
    counts = collections.Counter()
    updates: list[dict] = []
    errors: list[str] = []
    mode = 'observe_only' if args.observe_only else 'midpoint_feedback'
    cmd = [str(args.presentmon), '--process_id', str(args.pid), '--output_stdout',
           '--v1_metrics', '--qpc_time_ms', '--no_console_stats', '--session_name',
           f'XeFGMidpoint-{args.pid}', '--timed', str(args.seconds), '--terminate_after_timed']
    channel: queue.Queue = queue.Queue(maxsize=8192)
    stopped = threading.Event()
    proc = None
    last_write = atomic_control(out, 0, clock)
    begun = clock()
    pending_rows = collections.deque()
    match_errors: list[float] = []
    failure = None
    try:
        with (out/'feedback-pm-stderr.txt').open('wb') as err, (out/'capture.csv').open('w', newline='', encoding='utf-8') as raw:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err, text=True,
                                    encoding='utf-8-sig', errors='strict', bufsize=1)
            def read_stdout():
                try:
                    for line in proc.stdout:
                        while not stopped.is_set():
                            try:
                                channel.put(line, timeout=.2)
                                break
                            except queue.Full:
                                continue
                        if stopped.is_set():
                            return
                finally:
                    stopped.set()
            reader = threading.Thread(target=read_stdout, daemon=True)
            reader.start()
            header = None
            last_flush = time.monotonic()
            deadline = time.monotonic()+args.seconds+10
            while not stopped.is_set() or not channel.empty() or pending_rows:
                if time.monotonic() > deadline:
                    raise TimeoutError('Bounded PresentMon watchdog expired')
                batch = []
                try:
                    batch.append(channel.get(timeout=.03))
                    while len(batch) < 512:
                        batch.append(channel.get_nowait())
                except queue.Empty:
                    pass
                index.refresh()
                now = clock()
                for line in batch:
                    raw.write(line)
                    fields = next(csv.reader([line]))
                    if header is None:
                        required = {'Application','ProcessID','QPCTime','msUntilDisplayed','Dropped','PresentMode'}
                        if not required.issubset(fields):
                            raise ValueError('Unexpected legacy PresentMon header')
                        header = fields
                    elif len(fields) == len(header):
                        pending_rows.append((dict(zip(header, fields)), now))
                        counts['rows'] += 1
                    else:
                        counts['malformed_rows'] += 1
                while pending_rows:
                    row, received = pending_rows[0]
                    try:
                        when = float(row['QPCTime'])*1000.0
                        delay = float(row['msUntilDisplayed'])
                        valid = (int(row['ProcessID']) == args.pid and row['Dropped'] == '0'
                                 and math.isfinite(when) and math.isfinite(delay) and delay >= 0)
                    except (ValueError, KeyError):
                        valid = False
                    if not valid:
                        pending_rows.popleft()
                        counts['invalid_or_dropped'] += 1
                        control.frames.clear()
                        continue
                    native = index.match(when)
                    if native is None and now-received < 600 and not stopped.is_set():
                        break  # native log can lag the ETW row by a partial flush
                    pending_rows.popleft()
                    if native is None:
                        counts['unmatched'] += 1
                        control.frames.clear()
                        continue
                    counts['matched'] += 1
                    match_errors.append(abs(native[1]-when))
                    f = Frame(native[0], when, when+delay, native[2], native[3], row['PresentMode'])
                    proposal = control.observe(f, now)
                    if proposal is not None and not args.observe_only:
                        try:
                            stamp = atomic_control(out, proposal['new_phase_ms'], clock)
                            control.commit(proposal, stamp)
                            updates.append(dict(proposal, written_qpc_ms=stamp))
                            last_write = stamp
                            with (out/'feedback-updates.jsonl').open('a', encoding='utf-8') as log:
                                log.write(json.dumps(updates[-1])+'\n')
                        except PermissionError as error:
                            counts['control_write_failures'] += 1
                            if len(errors) < 16:
                                errors.append(str(error))
                if now-last_write >= 1000 and now-control.last_valid_ms < 1800:
                    try:
                        last_write = atomic_control(out, control.phase, clock)
                        counts['heartbeats'] += 1
                    except PermissionError:
                        counts['heartbeat_write_failures'] += 1
                if time.monotonic()-last_flush > .5:
                    raw.flush()
                    last_flush = time.monotonic()
            reader.join(timeout=1)
            if proc.wait(timeout=3) != 0:
                raise RuntimeError('PresentMon returned an error')
            if counts['matched'] < 48:
                raise RuntimeError('Insufficient matching native display observations')
    except Exception as error:
        failure = f'{type(error).__name__}: {error}'
    finally:
        stopped.set()
        if proc is not None and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=3)
        report = {'kind': 'isolated_midpoint_feedback_v2', 'mode': mode, 'pid': args.pid,
                  'command': cmd, 'requested_seconds': args.seconds, 'counts': dict(counts),
                  'rejected_observations': dict(control.invalid), 'changes': updates,
                  'errors': errors, 'failure': failure, 'final_phase_ms': control.phase,
                  'last_proposal': control.last_proposal, 'start_qpc_ms': begun,
                  'end_qpc_ms': clock(), 'native_match_max_error_ms': max(match_errors, default=None),
                  'presentmon_exit': proc.returncode if proc else None,
                  'mean_period_modified': False, 'game_modified': False,
                  'certifies_optical_pixels_or_game': False}
        (out/'feedback-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(json.dumps({k:v for k,v in report.items() if k not in ('changes','command')}), flush=True)
        try:
            atomic_control(out, 0, clock)
        except OSError:
            pass
    return 1 if failure else 0


if __name__ == '__main__':
    raise SystemExit(main())

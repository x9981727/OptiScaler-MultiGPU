"""Conservative, session-local ETW/native correlation for the v26 controller.

This is measured stream correlation, NOT a proof of COM object identity. Never
use pointer arithmetic or a fixed address from a previous process/session.
"""
from __future__ import annotations
import bisect
from collections import Counter
import math

class NativeMatcher:
    MAX_DELAY_MS = 0.125
    REQUIRED_PAIRS = 96
    MIN_SPAN_MS = 500.0
    MAX_GAP_MS = 100.0

    def __init__(self, pid: int):
        self.pid = pid
        self.counts = Counter()
        self.key = None
        self.last_id = None
        self.last_event = None
        self.first_event = None
        self.streak = 0
        self.locked = False
        self.epochs = 0
        self.generation = None
        self.last_reason = 'learning session-local stream association'
        self.mapping = None
        self.events = []
        self.fatal = None

    def _reject(self, reason, reset=False):
        self.counts[reason] += 1
        self.last_reason = reason
        if reset:
            self.locked = False
            self.streak = 0
            self.first_event = None
        return None

    def _fail(self, reason):
        self.fatal = reason
        return self._reject(reason, reset=True)

    def match(self, row, tail):
        if self.fatal:
            return self._reject('association_latched_off')
        try:
            if int(row['ProcessID']) != self.pid:
                return self._reject('wrong_process')
            if row['Application'].lower() != 'wwm.exe' or row['Runtime'] != 'DXGI':
                return self._reject('wrong_application_or_runtime')
            if row['Dropped'] != '0':
                return self._reject('dropped_event')
            t = float(row['QPCTime']) * 1000.0
            latency = float(row['msUntilDisplayed'])
            if not math.isfinite(t) or not math.isfinite(latency) or latency < 0:
                return self._reject('invalid_event_time')
            etw_address = int(row['SwapChainAddress'], 16)
            if etw_address <= 0:
                return self._reject('invalid_address')
            if self.last_event is not None and t <= self.last_event:
                return self._reject('duplicate_or_reordered_event')
            # Only earlier native calls are causal candidates. Requiring exactly
            # one candidate avoids choosing arbitrarily between closely spaced calls.
            lo = bisect.bisect_left(tail.times, t-self.MAX_DELAY_MS)
            hi = bisect.bisect_right(tail.times, t)
            if hi == lo:
                return self._reject('no_causal_native_in_125us')
            if hi-lo != 1:
                return self._reject('ambiguous_native_candidates', reset=True)
            r = dict(tail.records[lo])
            native_id = int(r['id'])
            native_address = int(r['swapchain'])
            generation = int(r['generation'])
            native_time = float(r['qpc_ms'])
            if int(r['hresult']) != 0:
                return self._reject('native_present_not_success', reset=True)
            if native_address <= 0 or native_id <= 0 or not math.isfinite(native_time):
                return self._reject('invalid_native_record', reset=True)
            if self.last_id is not None and native_id <= self.last_id:
                return self._reject('duplicate_or_reordered_native')
            key = (native_address, etw_address)
            if self.mapping is not None and key != self.mapping:
                return self._fail('competing_or_changed_swapchain_association')
            if self.key is not None and key != self.key:
                return self._fail('competing_swapchain_during_learning')
            consecutive = (self.key == key and self.last_id is not None
                           and native_id == self.last_id+1
                           and t-self.last_event <= self.MAX_GAP_MS
                           and (self.locked or generation == self.generation))
            if generation != self.generation:
                self.epochs += 1
            self.generation = generation
            if not consecutive:
                self.streak = 0
                self.first_event = t
                # Before binding, a gap restarts learning. Once bound, every
                # match must still satisfy unique causality and exact mapping.
                # Do NOT revoke association on the DLL paced/unpaced generation
                # change: that would itself disable pacing and cause oscillation.
                if self.mapping is None: self.locked = False
            self.key = key
            self.last_id = native_id
            self.last_event = t
            self.streak += 1
            self.counts['causal_unique_candidates'] += 1
            if not self.locked and self.streak >= self.REQUIRED_PAIRS and t-self.first_event >= self.MIN_SPAN_MS:
                self.locked = True
                self.mapping = key
                if len(self.events) < 256:
                    self.events.append({'event':'association_locked','qpc_ms':t,
                                        'native_address':hex(native_address),
                                        'etw_address':hex(etw_address),'generation':generation,
                                        'consecutive_pairs':self.streak,
                                        'span_ms':t-self.first_event})
            if not self.locked:
                return self._reject('association_warmup')
            self.counts['matched'] += 1
            self.last_reason = 'matched_session_local_causal_stream'
            # This preserves the v26 controller's latency convention. The ETW
            # offset is reported separately; no synthetic FPS/FrameType labels.
            r['latency'] = latency
            r['etw_offset_ms'] = t-native_time
            return r
        except (ValueError, KeyError, OverflowError, TypeError):
            return self._reject('malformed_record')

    def summary(self):
        return {'method':'session_local_unique_causal_sequence_not_com_identity_proof',
                'max_native_to_etw_ms':self.MAX_DELAY_MS,
                'required_consecutive_pairs':self.REQUIRED_PAIRS,
                'minimum_learning_span_ms':self.MIN_SPAN_MS,
                'locked':self.locked,'fatal':self.fatal,'last_reason':self.last_reason,
                'counts':dict(self.counts),'events':self.events}

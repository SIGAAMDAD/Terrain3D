"""Static guards and formula cases for the editor's rolling CPU cost column.

This is not a Godot editor runtime test; it checks the implementation contract
and the arithmetic used for the per-frame estimator.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
text = (ROOT / 'src/runtime_profiler_window.gd').read_text()

for required in (
    '"Est CPU ms/frame"',
    'Engine.get_frames_drawn()',
    'interval_frames: int = drawn_frames - _streaming_previous_drawn_frames',
    'calls - previous_calls',
    'samples - previous_samples',
    '(total_usec - previous_total_usec) / float(interval_samples)',
    'mean_usec * float(interval_calls) / (1000.0 * float(interval_frames))',
    '_streaming_previous_stages = stages.duplicate(true)',
    'sample_every == _streaming_previous_sample_every',
    'if calls >= previous_calls and samples >= previous_samples and total_usec >= previous_total_usec:',
    'if interval_calls == 0:',
    '_reset_streaming_interval()',
):
    assert required in text, f'Missing CPU/frame guard: {required}'
assert 'float(values.get("avg_usec_sampled", 0.0)) * float(calls)' not in text
assert 'mean x calls' not in text
assert '"%.3f" % ms_per_frame if ms_per_frame >= 0.0 else "—"' in text


def estimate(prev_calls, now_calls, prev_samples, now_samples,
             prev_total_us, now_total_us, frame_delta, lifetime_avg_us):
    if frame_delta <= 0 or now_calls < prev_calls or now_samples < prev_samples or now_total_us < prev_total_us:
        return None
    calls = now_calls - prev_calls
    sampled = now_samples - prev_samples
    mean_us = (now_total_us - prev_total_us) / sampled if sampled else lifetime_avg_us
    return mean_us * calls / (1000 * frame_delta) if sampled or prev_samples else (0.0 if calls == 0 else None)

# 40 callbacks, 10 sampled calls of 100us, over 20 frames = 0.2ms/frame.
assert estimate(1000, 1040, 125, 135, 12500, 13500, 20, 100) == 0.2
# Repeated UI refreshes with no stage invocations must not accumulate prior work.
assert estimate(1040, 1040, 135, 135, 13500, 13500, 20, 100) == 0.0
# An unsampled interval can fall back to the prior per-invocation sample mean.
assert estimate(1040, 1043, 135, 135, 13500, 13500, 20, 100) == 0.015
# A profiler reset or no elapsed rendered frames cannot be graphed as a CPU cost.
assert estimate(1040, 20, 135, 2, 13500, 200, 20, 100) is None
assert estimate(1040, 1043, 135, 135, 13500, 13500, 0, 100) is None
print('PASS: rolling estimated CPU ms/frame source and arithmetic guards')

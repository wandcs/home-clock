"""Original versioned alarm synthesis; no playback or network access.

Generate ignored PCM/WAV candidates and verify the proposed digital behavior.
Uses only the Python standard library; never opens an audio device or network.
"""
from array import array
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
import sys
import wave

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SPEC_PATH = ROOT / "assets/alarm-audio/candidate.json"
SPEC = json.loads(SPEC_PATH.read_text(encoding="utf-8"))
RATE = SPEC["sample_rate"]


def gain(seconds):
    u = min(1.0, max(0.0, seconds / SPEC["rise_seconds"]))
    return 10 ** ((SPEC["gain_start_db"] + u *
                   (SPEC["gain_max_db"] - SPEC["gain_start_db"])) / 20)


def reference():
    samples = [0.0] * round(RATE * SPEC["period_seconds"])
    count = round(RATE * SPEC["note_seconds"])
    weights = SPEC["harmonic_weights"]
    for start, frequency in zip(SPEC["note_onsets_seconds"],
                                SPEC["note_frequencies_hz"]):
        offset = round(start * RATE)
        for n in range(count):
            t = n / RATE
            attack = 0.5 - 0.5 * math.cos(math.pi * min(1, t / SPEC["attack_seconds"]))
            remaining = (count - 1 - n) / RATE
            release = 0.5 - 0.5 * math.cos(math.pi * min(1, remaining / SPEC["release_seconds"]))
            envelope = attack * math.exp(-t / SPEC["decay_tau_seconds"]) * release
            tone = sum(w * math.sin(2 * math.pi * frequency * (h + 1) * t)
                       for h, w in enumerate(weights)) / sum(weights)
            samples[offset + n] += envelope * tone
    scale = SPEC["reference_peak_fs"] / max(map(abs, samples))
    return [v * scale for v in samples]


def pcm(values):
    values = [round(v * 32767) for v in values]
    assert all(-32768 < v < 32767 for v in values), "Digital clipping"
    data = array("h", values)
    if sys.byteorder != "little":
        data.byteswap()
    return data.tobytes()


def write_wav(path, data):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(data)
    with wave.open(str(path), "rb") as check:
        assert (check.getnchannels(), check.getsampwidth(), check.getframerate(),
                check.getcomptype()) == (1, 2, RATE, "NONE")
        assert check.readframes(check.getnframes()) == data
    return {"file": path.name, "bytes": path.stat().st_size,
            "frames": len(data) // 2, "seconds": len(data) / (2 * RATE),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / ".local/generated/alarm-audio")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    allowed = (ROOT / ".local/generated/alarm-audio").resolve()
    if output != allowed and allowed not in output.parents:
        parser.error("Generated files must remain under .local/generated/alarm-audio/")
    output.mkdir(parents=True, exist_ok=True)

    base = reference()
    period = len(base)
    ramp = [base[n % period] * gain(n / RATE) for n in range(40 * RATE)]
    # One reference session: stop while sounding, stay silent, restart from low gain.
    stop = 9.23
    restart = 12.0
    faded = round(SPEC["stop_fade_seconds"] * RATE)
    stop_sample, restart_sample = round(stop * RATE), round(restart * RATE)
    stopped = []
    for n in range(16 * RATE):
        if n < stop_sample:
            value = base[n % period] * gain(n / RATE)
        elif n < stop_sample + faded:
            u = (n - stop_sample) / (faded - 1)
            value = base[n % period] * gain(n / RATE) * (0.5 + 0.5 * math.cos(math.pi * u))
        elif n < restart_sample:
            value = 0.0
        else:
            age = n - restart_sample
            value = base[age % period] * gain(age / RATE)
        stopped.append(value)

    assert base[0] == base[-1] == 0
    for onset in SPEC["note_onsets_seconds"]:
        first = round(onset * RATE)
        assert base[first] == base[first + round(SPEC["note_seconds"] * RATE) - 1] == 0
    note_end = max(SPEC["note_onsets_seconds"]) + SPEC["note_seconds"]
    assert note_end < SPEC["period_seconds"]
    assert all(v == 0 for v in base[round(note_end * RATE):])
    assert math.isclose(max(map(abs, base)), 0.8)
    assert SPEC["note_frequencies_hz"][-1] * len(SPEC["harmonic_weights"]) < RATE / 2
    assert all(gain(n / RATE) <= gain((n + 1) / RATE) for n in range(30 * RATE))
    assert math.isclose(gain(30), gain(40))
    assert gain(4) > gain(0), "The gain must not reset every phrase"
    assert all(v == 0 for v in stopped[stop_sample + faded:restart_sample])
    assert pcm(stopped[restart_sample:]) == pcm(ramp[:4 * RATE])
    assert stopped[stop_sample + faded - 1] == 0
    data = [pcm(base), pcm(ramp), pcm(stopped)]
    measurements = []
    for n in range(10):
        phrase = ramp[n * period:(n + 1) * period]
        measurements.append(math.sqrt(sum(v * v for v in phrase) / period))
    assert all(a <= b + 1e-12 for a, b in zip(measurements, measurements[1:]))
    assert data[1][32 * RATE * 2:36 * RATE * 2] == data[1][36 * RATE * 2:40 * RATE * 2]
    files = [write_wav(output / name, payload)
             for name, payload in zip(SPEC["outputs"], data)]
    report = {
        "candidate_version": SPEC["version"],
        "format": "22050 Hz mono signed 16-bit little-endian PCM in WAV",
        "spec_sha256_lf": hashlib.sha256(SPEC_PATH.read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
        "script_sha256_lf": hashlib.sha256(Path(__file__).read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
        "gain_points": [{"seconds": t, "db": 20 * math.log10(gain(t)), "multiplier": gain(t)}
                        for t in [0, 10, 20, 30, 40]],
        "reference_peak_fs": max(map(abs, base)),
        "tail_silence_seconds": SPEC["period_seconds"] - note_end,
        "ramp_peak_fs": max(map(abs, ramp)),
        "ramp_pcm_peak_integer": max(abs(v[0]) for v in struct.iter_unpack("<h", data[1])),
        "phrase_rms_fs": measurements,
        "checks": {"wav_roundtrip": True, "no_digital_clipping": True,
                   "zero_note_and_loop_edges": True, "tail_silence_matches_spec": True,
                   "monotonic_gain_to_30s": True, "gain_not_reset_per_loop": True,
                   "ceiling_cycles_identical": True, "stop_fades_to_silence": True,
                   "restart_matches_new_low_start": True},
        "stop_reference": {"stop_at_seconds": stop, "fade_seconds": SPEC["stop_fade_seconds"],
                           "restart_at_seconds": restart},
        "storage": {"reference_pcm_bytes": len(data[0]), "reference_wav_bytes": files[0]["bytes"],
                    "mono_20ms_bytes": round(RATE * 0.02) * 2, "two_blocks_bytes": round(RATE * 0.02) * 4},
        "files": files,
        "limits": ["No playback or board access", "No SPL, timbre, wake-up or physical stop evidence",
                   "Reference event behavior only; no firmware driver/queue validation",
                   "Codec reference is engineering configuration, not SPL calibration; listening pending"]
    }
    (output / "host-checks.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()

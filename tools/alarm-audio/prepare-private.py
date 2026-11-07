"""Prepare an explicitly supplied PCM WAV inside an ignored build directory.

Format conversion is deliberately separate: this tool never normalizes, filters,
compresses or changes gain. No source filename or audio enters public sources.
"""
from pathlib import Path
import argparse
import hashlib
import io
import json
import os
import struct
import wave

ROOT = Path(__file__).resolve().parents[2]
RATE = 22050
MAX_FRAMES = 60 * RATE
ENV_WAV = 'HOME_CLOCK_ALARM_AUDIO_WAV'


def read_wav(path: Path):
    try:
        # Bound memory before reading a user-provided file, including RIFF extras.
        if not path.is_file() or path.stat().st_size > 2 * MAX_FRAMES + 65536:
            raise ValueError()
        raw = path.read_bytes()
        with wave.open(io.BytesIO(raw), 'rb') as wav:
            count = wav.getnframes()
            if (wav.getnchannels() != 1 or wav.getsampwidth() != 2 or
                    wav.getframerate() != RATE or wav.getcomptype() != 'NONE' or
                    not 1 <= count <= MAX_FRAMES):
                raise ValueError()
            pcm = wav.readframes(count)
        if len(pcm) != 2 * count:
            raise ValueError()
    except (OSError, EOFError, ValueError, wave.Error):
        raise ValueError('Explicit alarm WAV must exist and be mono 22050Hz PCM16, at most 60 seconds') from None
    return pcm, {
        'format': 'mono 22050Hz s16le', 'frames': count, 'bytes': len(pcm),
        'duration_seconds': count / RATE,
        'wav_sha256': hashlib.sha256(raw).hexdigest(),
        'pcm_sha256': hashlib.sha256(pcm).hexdigest(),
        'processing': 'none; samples copied unchanged',
        'license': 'Externally supplied private audio; not covered by the project MIT License',
    }


def prepare(path: Path, output: Path):
    if not any(output.resolve().is_relative_to((ROOT / folder).resolve())
               for folder in ('firmware/build', '.local')):
        raise ValueError('Private audio output must remain in an ignored build directory')
    pcm, metadata = read_wav(path)
    values = struct.unpack('<' + str(metadata['frames']) + 'h', pcm)
    text = '#include "alarm_audio.h"\n/* Private build input; never publish this generated file or firmware. */\n'
    text += 'const uint32_t aa_reference_count = ' + str(len(values)) + 'u;\n'
    text += 'const int16_t aa_reference[] = {\n'
    text += ''.join('    ' + ','.join(map(str, values[i:i+16])) + ',\n'
                    for i in range(0, len(values), 16))
    text += '};\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.is_file() or output.read_bytes() != text.encode():
        output.write_text(text, encoding='utf8', newline='\n')
    report = output.with_suffix('.json')
    manifest = json.dumps(metadata, indent=2) + '\n'
    if not report.is_file() or report.read_text(encoding='utf8') != manifest:
        report.write_text(manifest, encoding='utf8', newline='\n')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    selected = args.input or (Path(os.environ[ENV_WAV]) if os.environ.get(ENV_WAV) else None)
    try:
        if selected is None:
            raise ValueError('No explicit private alarm WAV was supplied')
        if args.check:
            _, metadata = read_wav(selected)
        elif args.output is not None:
            metadata = prepare(selected, args.output)
        else:
            raise ValueError('An ignored build output path is required')
    except ValueError as error:
        raise SystemExit(str(error)) from None
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()

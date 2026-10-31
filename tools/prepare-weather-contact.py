"""Prepare a private binary configuration resource; never print its contents."""
from pathlib import Path
import argparse
import json
import os

ROOT = Path(__file__).resolve().parents[1]


ENV_CONFIG = 'HOME_CLOCK_MET_CONTACT_CONFIG'


def read_contact(config: Path | None, required: bool = False) -> bytes:
    """Validate an explicitly selected configuration without printing its value."""
    value = ''
    if config is not None and config.is_file():
        try:
            value = json.loads(config.read_text(encoding='utf8'))['met_user_agent']
            if not isinstance(value, str) or not 8 <= len(value) <= 240 or any(ord(c) < 32 or ord(c) > 126 for c in value):
                raise ValueError()
            if '@' not in value and 'https://' not in value:
                raise ValueError()
        except (ValueError, KeyError, TypeError, UnicodeError):
            raise ValueError('Invalid private MET contact configuration') from None
    elif required:
        raise ValueError('Explicit MET contact configuration is missing')
    return value.encode('ascii') + b'\0'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--config', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if not any(output.is_relative_to((ROOT / folder).resolve()) for folder in ('firmware/build', '.local')):
        raise SystemExit('Private MET resource output must remain in an ignored build directory')
    if args.config is not None:
        config, required = args.config, False
    elif ENV_CONFIG in os.environ:
        selected = os.environ[ENV_CONFIG]
        config, required = (Path(selected), True) if selected else (None, False)
    else:
        config, required = ROOT / '.local/weather-provider-contact.json', False
    try:
        content = read_contact(config, required)
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from None
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.is_file() or output.read_bytes() != content:
        output.write_bytes(content)


if __name__ == '__main__':
    main()

"""Read-only prerequisites for the public ESP-IDF build; no SDK invocation or downloads."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
CORE = ('brightness_policy', 'comfort', 'solar_core', 'alarm_core')
REQUIRED = [
    'LICENSE', 'firmware/CMakeLists.txt', 'firmware/main/CMakeLists.txt',
    'firmware/main/idf_component.yml', 'firmware/main/app_main.c',
    'firmware/sdkconfig', 'firmware/sdkconfig.defaults', 'firmware/sdkconfig.development',
    'firmware/dependencies.lock', 'firmware/dependency-constraints.txt', 'firmware/partitions.csv',
    'components/waveshare__esp32_p4_wifi6_touch_lcd_7b/CMakeLists.txt',
    'components/waveshare__esp32_p4_wifi6_touch_lcd_7b/idf_component.yml',
    'components/waveshare__esp32_p4_wifi6_touch_lcd_7b/LICENSE',
    'assets/application/manifest.json', 'assets/application/OFL.txt',
    'assets/application/NotoSans-LICENSE.txt', 'assets/application/Fluent-LICENSE.txt',
    'assets/application/Unicode-LICENSE.txt', 'assets/application/comfort-LICENSE.txt',
    'assets/application/comfort-NOTICE.txt', 'assets/application/Location-NOTICE.txt',
    'assets/application/Location-OGDL.txt', 'assets/application/comfort_images.c',
    'assets/application/pinyin_dict.c', 'assets/application/weather_forecast_icons.c',
    'assets/weather-background/fields.c', 'assets/weather-background/fields.h',
    'assets/weather-background/NOTICE.md', 'tools/prepare-weather-contact.py',
] + [f'firmware/core/{name}.{ext}' for name in CORE for ext in ('c', 'h')]


def inspect(args):
    checks = []
    context = {'python': sys.executable, 'project': str(ROOT),
               'sdk_version_required': '6.1.0',
               'met_contact': 'explicit opt-in' if args.met_contact_config else 'disabled'}

    def check(name, ok, detail):
        checks.append({'name': name, 'ok': bool(ok), 'detail': detail})

    check('python', sys.version_info >= (3, 10), 'Python 3.10 or newer')
    missing = [name for name in REQUIRED if not (ROOT / name).is_file()]
    check('source files and license notices', not missing,
          ', '.join(missing) if missing else 'required tracked inputs present')
    try:
        manifest = json.loads((ROOT / 'assets/application/manifest.json').read_text(encoding='utf8'))
        bad = []
        for name, entry in manifest.items():
            if not isinstance(entry, dict) or 'bytes' not in entry or 'sha256' not in entry:
                continue
            path = ROOT / 'assets/application' / name
            if not path.is_file() or path.stat().st_size != entry['bytes'] or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
                bad.append(name)
        check('committed application assets', not bad, ', '.join(bad) if bad else 'manifest size/hash checks passed')
    except (OSError, ValueError, TypeError, KeyError):
        check('committed application assets', False, 'manifest could not be checked')

    build = (ROOT / args.build_directory).resolve()
    context['build_directory'] = str(build)
    check('isolated output', build.is_relative_to((ROOT / '.local').resolve()) and build != (ROOT / '.local').resolve(),
          'build directory must be a child of the repository .local directory')
    check('project/output paths', not any(re.search(r'\s|"', str(p)) for p in (ROOT, build)),
          'ESP-IDF requires paths without spaces; choose a short checkout/output path')

    if args.met_contact_config:
        try:
            spec = importlib.util.spec_from_file_location('public_build_contact', ROOT / 'tools/prepare-weather-contact.py')
            helper = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(helper)
            helper.read_contact(Path(args.met_contact_config).resolve(), required=True)
            check('MET contact opt-in', True, 'valid configuration; contents are not printed')
        except (OSError, ValueError):
            check('MET contact opt-in', False, 'explicit file is missing or invalid; contents are not printed')

    if not args.source_only:
        sdk_text = os.environ.get('IDF_PATH', '')
        sdk = Path(sdk_text).resolve() if sdk_text else None
        context['IDF_PATH'] = str(sdk) if sdk else None
        version = None
        if sdk:
            try:
                text = (sdk / 'tools/cmake/version.cmake').read_text(encoding='utf8')
                version = tuple(int(re.search(r'set\(IDF_VERSION_' + part + r'\s+(\d+)\)', text)[1]) for part in ('MAJOR', 'MINOR', 'PATCH'))
            except (OSError, TypeError, ValueError):
                pass
        check('ESP-IDF', version == (6, 1, 0), 'activate the installed ESP-IDF v6.1 environment (IDF_PATH required)')
        if sdk:
            check('SDK path', not re.search(r'\s|"', str(sdk)), 'ESP-IDF path must not contain spaces; keep SDK/tools paths short on Windows')
        for name in ('cmake', 'ninja', 'riscv32-esp-elf-gcc'):
            path = shutil.which(name)
            context[name] = path
            check(name, path is not None, 'available in the activated ESP-IDF environment')
        try:
            manager = importlib.util.find_spec('idf_component_manager') is not None
        except (ImportError, ValueError):
            manager = False
        check('SDK Python packages', manager, 'use Python from the activated ESP-IDF environment')
        for name in ('IDF_COMPONENT_CONSTRAINTS', 'IDF_COMPONENT_CONSTRAINT_FILES'):
            check(name, not os.environ.get(name), 'unset inherited component constraints; this build uses firmware/dependency-constraints.txt')

    return {'ok': all(item['ok'] for item in checks), 'source_only': args.source_only,
            'checks': checks, 'context': context, 'SDK_invoked': False,
            'files_written': False, 'device_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-only', action='store_true', help='check source/assets without requiring an installed SDK')
    parser.add_argument('--build-directory', default='.local/public-build')
    parser.add_argument('--met-contact-config')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    report = inspect(args)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        for item in report['checks']:
            print(('OK   ' if item['ok'] else 'MISS ') + item['name'] + ': ' + item['detail'])
        print('Ready.' if report['ok'] else 'Resolve the missing prerequisites above; see docs/building.md.')
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

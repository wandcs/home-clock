"""Offline fault injection of the locked SDIO reader plus its actual IDF port."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from prepare import EXPECTED_SOURCE_SHA256, function, prepare

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
HOSTED = ROOT / 'firmware/managed_components/espressif__esp_hosted'
SOURCE = HOSTED / 'host/mcu/eh_host_mcu_transport/src/eh_host_bus_sdio.c'
PORT = HOSTED / 'port/os/idf/src/eh_host_port_sdio.c'
CONFIG = HOSTED / 'host/eh_host_config/include/eh_host_port_master_config.h'
REG = HOSTED / 'port/os/include/eh_host_port_sdio_reg.h'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True, help='new repository .local directory')
    args = ap.parse_args()
    out = (ROOT / args.output).resolve()
    out.relative_to(ROOT / '.local')
    if out.exists():
        ap.error('Output already exists; preserve earlier evidence')
    raw = SOURCE.read_bytes()
    assert digest(raw) == EXPECTED_SOURCE_SHA256
    prepared = prepare(raw)
    assert prepare(raw) == prepared
    try:
        prepare(raw + b'\n')
    except ValueError:
        pass
    else:
        raise AssertionError('Unknown source hash was accepted')
    out.mkdir(parents=True)
    original = raw.decode('utf8')
    port = PORT.read_text(encoding='utf8')
    config = CONFIG.read_text(encoding='utf8')
    reg = REG.read_text(encoding='utf8')
    constants = []
    for name in ('ESP_BLOCK_SIZE', 'ESP_SLAVE_CMD53_END_ADDR', 'ESP_SLAVE_LEN_MASK', 'ESP_RX_BYTE_MAX'):
        constants += [next(line for line in reg.splitlines() if line.startswith('#define ' + name + ' '))]
    for name in ('EH_HOST_PORT_SDIO_RX_LEN_TO_TRANSFER', 'EH_HOST_PORT_SDIO_RX_BLOCK_ONLY_XFER', 'EH_HOST_PORT_SDIO_RX_BLOCKS_TO_TRANSFER'):
        lines = [line.strip() for line in config.splitlines() if line.strip().startswith('#define ' + name)]
        constants += [lines[-1]]  # Current P4 disables one-block workaround.
    (out / 'port-constants.inc').write_text('\n'.join(constants) + '\n', newline='\n')
    port_functions = function(port, 'static esp_err_t sdio_read_fromio(') + '\n'
    port_functions += function(port, 'int eh_host_port_sdio_read_block(') + '\n'
    (out / 'port-functions.inc').write_text(port_functions, newline='\n')
    ring = original[original.index('/* Double-buffer:'):original.index('static eh_host_port_sem_t *sem_double_buf_xfer_data;')]
    (out / 'ring.inc').write_text(ring, newline='\n')
    start = original.index('/* Free must match alloc family:')
    end = original.index('\tsdio_rx_ring_reset();', start) + len('\tsdio_rx_ring_reset();')
    (out / 'teardown.inc').write_text(original[start:end] + '\n', newline='\n')
    for name, source in (('baseline', original), ('green', prepared.decode('utf8'))):
        path = out / name
        path.mkdir()
        snippets = []
        if name == 'green':
            snippets += [(HERE / 'streaming-rx.inc').read_text(encoding='utf8')]
        else:
            snippets += [function(source, 'static uint8_t * sdio_rx_get_buffer(')]
        snippets += [function(source[source.rindex('static void sdio_rx_free_buffer(uint8_t * buf)\n{'):], 'static void sdio_rx_free_buffer(uint8_t * buf)\n{'),
                     function(source, 'static inline bool sdio_pkt_len_reg_is_bus_fault('),
                     function(source, 'static int sdio_get_len_from_slave(uint32_t *rx_size, uint32_t reg_val,'),
                     function(source, 'static void sdio_data_to_rx_buf_task(void *pvParameters)\n{'),
                     function(source, 'static void sdio_read_task(void *pvParameters)\n{')]
        (path / 'rx-functions.inc').write_text('\n'.join(snippets) + '\n', newline='\n')
    (out / 'eh_host_bus_sdio.c').write_bytes(prepared)
    if os.name == 'nt':
        mapped = subprocess.run(['wsl.exe', '--exec', 'wslpath', '-a', str(ROOT)], capture_output=True, text=True, check=True, timeout=15)
        host_root = mapped.stdout.strip()
        prefix = ['wsl.exe', '--cd', host_root, '--exec']
    else:
        host_root = str(ROOT)
        prefix = []

    def host(path):
        return host_root + '/' + path.resolve().relative_to(ROOT).as_posix()

    commands = []
    def run(name, argv):
        cmd = prefix + argv
        completed = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=45)
        output = (completed.stdout + completed.stderr).decode('utf8', errors='replace')
        (out / (name + '.log')).write_text(output, encoding='utf8')
        commands.append({'name': name, 'argv': cmd, 'exit_code': completed.returncode})
        if completed.returncode:
            raise RuntimeError(name + ' failed; inspect ' + str(out / (name + '.log')))
        return output

    results = {}
    for name in ('baseline', 'green'):
        exe = out / name / 'sdio-test'
        flags = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
                 '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                 '-I' + host(out / name), '-I' + host(out)]
        if name == 'baseline':
            flags += ['-DHC_BASELINE=1']
        run(name + '-build', flags + [host(HERE / 'sdio-test.c'), '-o', host(exe)])
        log = run(name + '-run', ['env', 'ASAN_OPTIONS=detect_leaks=1:halt_on_error=1',
                                 'UBSAN_OPTIONS=halt_on_error=1', host(exe)])
        results[name] = json.loads(log.strip().splitlines()[-1])
        print(json.dumps(results[name]))
    assert results['baseline']['baseline_stalls_after_one_dma_failure'] is True
    assert results['green']['passed'] is True and results['green']['groups'] == 7
    paths = [SOURCE, PORT, CONFIG, REG, HERE / 'prepare.py', HERE / 'streaming-rx.inc', HERE / 'sdio-test.c', Path(__file__)]
    assert SOURCE.read_bytes() == raw
    report = {'scope': 'Actual locked Hosted reader/drain/teardown and actual port block/byte splitting; FIFO/DMA/RTOS modeled, not physical C6',
              'results': results, 'prepared_sha256': digest(prepared),
              'sha256': {p.relative_to(ROOT).as_posix(): digest(p.read_bytes()) for p in paths}, 'commands': commands}
    (out / 'checks.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
    print('Evidence: ' + str(out / 'checks.json'))


if __name__ == '__main__':
    main()

"""Prepare one build-local SDIO source; never edit the managed component."""
import argparse
import hashlib
from pathlib import Path

EXPECTED_SOURCE_SHA256 = '860b49c0f1bb177cf2cbdf99c4e6b992717efffde99c0324f340a99d2f5b93ae'
HERE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for position in range(opening, len(source)):
        depth += (source[position] == '{') - (source[position] == '}')
        if not depth:
            return source[start:position + 1]
    raise ValueError('Unterminated function: ' + signature)


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('Patch context no longer matches exactly once: ' + old[:80])
    return source.replace(old, new, 1)


def prepare(raw):
    if digest(raw) != EXPECTED_SOURCE_SHA256:
        raise ValueError('Unsupported esp-hosted SDIO source SHA256; review the dependency before rebasing this patch')
    source = raw.decode('utf8')
    source = replace_once(source, function(source, 'static uint8_t * sdio_rx_get_buffer('),
                          (HERE / 'streaming-rx.inc').read_text(encoding='utf8').rstrip())
    init = function(source, 'static void *bus_init_internal(')
    reset = '\tmemset(&double_buf, 0, sizeof(double_buf_t));\n\tsdio_rx_ring_reset();\n'
    updated = replace_once(init, reset, '')
    updated = replace_once(updated, '\tshow_config();\n',
        '\tshow_config();\n\n' + reset +
        '#if EH_HOST_PORT_SDIO_HOST_RX_MODE == EH_HOST_PORT_SDIO_HOST_STREAMING_MODE\n'
        '\thc_sdio_rx_oom_reported = false;\n\tif (hc_sdio_rx_reserve() != 0)\n\t\treturn NULL;\n#endif\n')
    source = replace_once(source, init, updated)
    reader = function(source, 'static void sdio_read_task(void *pvParameters)\n{')
    old = '''\t\trxbuff = sdio_rx_get_buffer(len_from_slave);
\t\tif (!rxbuff) {
\t\t\tESP_LOGW(TAG, "rx_get_buffer(%lu) failed; skipping read",
\t\t\t         (unsigned long)len_from_slave);
\t\t\tSDIO_DRV_UNLOCK();
\t\t\tcontinue;
\t\t}'''
    new = '''\t\trxbuff = sdio_rx_get_buffer(len_from_slave);
#if EH_HOST_PORT_SDIO_HOST_RX_MODE == EH_HOST_PORT_SDIO_HOST_STREAMING_MODE
\t\t/* The interrupt is already cleared. Retry the same pending window,
\t\t * outside the lock, rather than waiting for an interrupt that may never
\t\t * recur. Preserve the head reserve if the larger allocation still fails. */
\t\tif (!rxbuff && len_from_slave <= HC_SDIO_RX_LINEAR_MAX) {
\t\t\tSDIO_DRV_UNLOCK();
\t\t\teh_host_port_task_delay_ms(HC_SDIO_RX_ALLOC_RETRY_MS);
\t\t\tSDIO_DRV_LOCK();
\t\t\tif (s_running)
\t\t\t\trxbuff = sdio_rx_get_buffer(len_from_slave);
\t\t}
\t\tif (!rxbuff) {
\t\t\tret = hc_sdio_rx_discard_pending(len_from_slave);
\t\t\tSDIO_DRV_UNLOCK();
\t\t\tif (ret != ESP_OK && s_running)
\t\t\t\tesp_event_post(EH_HOST_EVENT, (int32_t)EH_HOST_EVENT_TRANSPORT_FAILURE, NULL, 0, 0);
\t\t\tcontinue;
\t\t}
#else
\t\tif (!rxbuff) {
\t\t\tSDIO_DRV_UNLOCK();
\t\t\tcontinue;
\t\t}
#endif'''
    source = replace_once(source, reader, replace_once(reader, old, new))
    return source.encode('utf8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve() or 'managed_components' in args.output.resolve().parts:
        parser.error('Output must be build-local; managed component/source overwrite is forbidden')
    result = prepare(args.source.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != result:
        args.output.write_bytes(result)
    print('SDIO prepared SHA256=' + digest(result))


if __name__ == '__main__':
    main()

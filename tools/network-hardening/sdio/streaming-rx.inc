/* HomeClock S25: retain a small, always-owned DMA receive/discard path.
 * These are host policy/port limits, not guessed co-processor capacity. */
#define HC_SDIO_RX_LINEAR_MAX ((UINT16_MAX / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE)
#define HC_SDIO_RX_ALLOC_RETRY_MS 20
static bool hc_sdio_rx_oom_reported; /* One shortage warning per bus init. */
_Static_assert(SDIO_RX_STAGING_SLOTS >= 2, "streaming ring requires two slots");
_Static_assert(MAX_TRANSPORT_BUFFER_SIZE % ESP_BLOCK_SIZE == 0,
               "reserve must hold a padded discard transfer");

/* Run before queues/threads/card resources exist. Failure rolls back only the
 * allocations made here, so bus_init_internal can return NULL cleanly. */
static int hc_sdio_rx_reserve(void)
{
    for (int i = 0; i < SDIO_RX_STAGING_SLOTS; i++) {
        uint8_t *buf = eh_host_port_dma_alloc_aligned(
                MAX_TRANSPORT_BUFFER_SIZE, HOSTED_MEM_ALIGNMENT_64);
        if (!buf) {
            eh_host_port_task_delay_ms(HC_SDIO_RX_ALLOC_RETRY_MS);
            buf = eh_host_port_dma_alloc_aligned(
                    MAX_TRANSPORT_BUFFER_SIZE, HOSTED_MEM_ALIGNMENT_64);
        }
        if (!buf) {
            for (int j = 0; j < i; j++) {
                eh_host_port_dma_free(double_buf.buffer[j].buf);
                double_buf.buffer[j].buf = NULL;
                double_buf.buffer[j].buf_size = 0;
            }
            ESP_LOGE(TAG, "cannot reserve minimum SDIO RX DMA buffers");
            return -1;
        }
        double_buf.buffer[i].buf = buf;
        double_buf.buffer[i].buf_size = MAX_TRANSPORT_BUFFER_SIZE;
    }
    ESP_LOGI(TAG, "reserved %d SDIO RX slots, %u bytes minimum each",
             SDIO_RX_STAGING_SLOTS, (unsigned)MAX_TRANSPORT_BUFFER_SIZE);
    return 0;
}

static uint8_t * sdio_rx_get_buffer(uint32_t len)
{
    /* The port takes uint16_t and then pads. Do not truncate a large stream. */
    if (!len || len > HC_SDIO_RX_LINEAR_MAX)
        return NULL;
#if EH_HOST_PORT_SDIO_RX_BLOCK_ONLY_XFER
    len = ((len + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;
#endif
    int index = double_buf.head;
    buf_info_t *slot = &double_buf.buffer[index];
    if (len > slot->buf_size) {
        /* Allocate first: an OOM must leave the reserved discard path intact. */
        uint8_t *replacement = eh_host_port_dma_alloc_aligned(len, HOSTED_MEM_ALIGNMENT_64);
        if (!replacement) {
            if (!hc_sdio_rx_oom_reported) {
                hc_sdio_rx_oom_reported = true;
                ESP_LOGW(TAG, "SDIO RX DMA growth to %lu failed; retaining discard path",
                         (unsigned long)len);
            } else {
                ESP_LOGD(TAG, "SDIO RX DMA growth failed (%lu)", (unsigned long)len);
            }
            return NULL;
        }
        if (slot->buf)
            eh_host_port_dma_free(slot->buf);
        slot->buf = replacement;
        slot->buf_size = len;
    }
    return slot->buf;
}

/* Streaming FIFO permits a smaller logical read than the advertised PKT_LEN
 * (official essl_sdio_get_packet). Each small logical transfer requests chunk,
 * NOT pending, bytes in its address. Block padding is not consumed FIFO data.
 * The head slot is unpublished: the drain task can only own a different slot. */
static int hc_sdio_rx_discard_pending(uint32_t pending)
{
    buf_info_t *slot = &double_buf.buffer[double_buf.head];
    if (!slot->buf || slot->buf_size < MAX_TRANSPORT_BUFFER_SIZE ||
            !pending || pending > ESP_SLAVE_LEN_MASK)
        return ESP_ERR_INVALID_STATE;
    uint32_t discarded = 0;
    while (pending && s_running) {
        uint32_t chunk = pending > MAX_TRANSPORT_BUFFER_SIZE ? MAX_TRANSPORT_BUFFER_SIZE : pending;
        uint32_t transfer = chunk;
#if EH_HOST_PORT_SDIO_RX_BLOCK_ONLY_XFER
        transfer = ((chunk + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;
#endif
        int ret = eh_host_port_sdio_read_block(sdio_handle,
                ESP_SLAVE_CMD53_END_ADDR - chunk, slot->buf, transfer, ACQUIRE_LOCK);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SDIO discard failed after %lu bytes: %d",
                     (unsigned long)discarded, ret);
            return ret; /* Failed CMD53 consumption is unknown: never invent it. */
        }
        sdio_rx_byte_count = (sdio_rx_byte_count + chunk) % ESP_RX_BYTE_MAX;
        pending -= chunk;
        discarded += chunk;
    }
    if (pending)
        return ESP_ERR_INVALID_STATE;
    if (!hc_sdio_rx_oom_reported) {
        hc_sdio_rx_oom_reported = true;
        ESP_LOGW(TAG, "SDIO RX staging unavailable: discarded %lu real FIFO bytes",
                 (unsigned long)discarded);
    } else {
        ESP_LOGD(TAG, "SDIO RX discarded %lu real FIFO bytes", (unsigned long)discarded);
    }
    return ESP_OK;
}

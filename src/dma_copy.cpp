/* Asynchronous SRAM -> PSRAM copies on a GDMA channel pair (see dma_copy.h).
 * This is what ESP-IDF's esp_async_memcpy does, which Arduino-ESP32's
 * prebuilt IDF 4.4 libraries leave out; the GDMA driver itself is there. */
#include "dma_copy.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_private/gdma.h>
#include <soc/soc_memory_layout.h>
#include <hal/dma_types.h>

namespace {
constexpr size_t CHUNK = 4032;       // per descriptor: <= 4095, multiple of 64
constexpr uintptr_t PSRAM_ALIGN = 64; // external-memory burst size
}

struct DmaCopy {
  gdma_channel_handle_t tx = nullptr, rx = nullptr;
  dma_descriptor_t *txd = nullptr, *rxd = nullptr;
  size_t maxDesc = 0;
  volatile bool busy = false;
  void (*done)(void *) = nullptr;
  void *arg = nullptr;
};

static bool IRAM_ATTR on_rx_eof(gdma_channel_handle_t, gdma_event_data_t *,
                                void *user) {
  DmaCopy *d = (DmaCopy *)user;
  d->busy = false;
  if (d->done) d->done(d->arg);
  return false;
}

DmaCopy *dma_copy_create(size_t maxBytes) {
  DmaCopy *d = new DmaCopy();
  d->maxDesc = (maxBytes + CHUNK - 1) / CHUNK;
  d->txd = (dma_descriptor_t *)heap_caps_calloc(d->maxDesc, sizeof(dma_descriptor_t),
                                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  d->rxd = (dma_descriptor_t *)heap_caps_calloc(d->maxDesc, sizeof(dma_descriptor_t),
                                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  gdma_channel_alloc_config_t txc = {};
  txc.direction = GDMA_CHANNEL_DIRECTION_TX;
  txc.flags.reserve_sibling = 1;
  gdma_channel_alloc_config_t rxc = {};
  rxc.direction = GDMA_CHANNEL_DIRECTION_RX;
  if (!d->txd || !d->rxd || gdma_new_channel(&txc, &d->tx) != ESP_OK) goto fail;
  rxc.sibling_chan = d->tx;
  if (gdma_new_channel(&rxc, &d->rx) != ESP_OK) goto fail;
  {
    // On the S3, TX and RX of an M2M pair must select the same unused
    // peripheral ID as their trigger (the M2M0 placeholder is -1, which
    // leaves the pair unconnected and the copy never starts).
    gdma_trigger_t trig = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
    uint32_t freeIds = 0;
    gdma_get_free_m2m_trig_id_mask(d->tx, &freeIds);
    if (!freeIds) goto fail;
    trig.instance_id = __builtin_ctz(freeIds);
    if (gdma_connect(d->rx, trig) != ESP_OK || gdma_connect(d->tx, trig) != ESP_OK) goto fail;
    gdma_strategy_config_t strategy = {};
    strategy.owner_check = true;
    strategy.auto_update_desc = true;
    gdma_apply_strategy(d->tx, &strategy);
    gdma_apply_strategy(d->rx, &strategy);
    gdma_transfer_ability_t ability = {};
    ability.sram_trans_align = 4;
    ability.psram_trans_align = PSRAM_ALIGN;
    gdma_set_transfer_ability(d->tx, &ability);
    gdma_set_transfer_ability(d->rx, &ability);
    gdma_rx_event_callbacks_t cbs = {};
    cbs.on_recv_eof = on_rx_eof;
    if (gdma_register_rx_event_callbacks(d->rx, &cbs, d) != ESP_OK) goto fail;
  }
  return d;
fail:
  Serial.println("[dma_copy] GDMA setup failed");
  if (d->rx) gdma_del_channel(d->rx);
  if (d->tx) gdma_del_channel(d->tx);
  heap_caps_free(d->txd);
  heap_caps_free(d->rxd);
  delete d;
  return nullptr;
}

bool IRAM_ATTR dma_copy_start(DmaCopy *d, void *dst, const void *src, size_t n,
                              void (*done)(void *), void *arg) {
  if (!d || d->busy || n == 0 || (n % PSRAM_ALIGN)) return false;
  // whichever side is in PSRAM must be burst-aligned; SRAM needs words
  const uintptr_t sa = (uintptr_t)src, da = (uintptr_t)dst;
  if ((esp_ptr_external_ram(src) ? sa % PSRAM_ALIGN : sa & 3) ||
      (esp_ptr_external_ram(dst) ? da % PSRAM_ALIGN : da & 3))
    return false;
  const size_t nd = (n + CHUNK - 1) / CHUNK;
  if (nd > d->maxDesc) return false;
  uint8_t *s = (uint8_t *)src, *t = (uint8_t *)dst;
  for (size_t i = 0; i < nd; i++) {
    const size_t off = i * CHUNK, len = (n - off < CHUNK) ? n - off : CHUNK;
    const bool last = i + 1 == nd;
    dma_descriptor_t &tx = d->txd[i], &rx = d->rxd[i];
    tx.dw0.size = len;
    tx.dw0.length = len;
    tx.dw0.err_eof = 0;
    tx.dw0.suc_eof = last;
    tx.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    tx.buffer = s + off;
    tx.next = last ? nullptr : &d->txd[i + 1];
    rx.dw0.size = len;
    rx.dw0.length = 0;
    rx.dw0.err_eof = 0;
    rx.dw0.suc_eof = 0;
    rx.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    rx.buffer = t + off;
    rx.next = last ? nullptr : &d->rxd[i + 1];
  }
  d->done = done;
  d->arg = arg;
  d->busy = true;
  gdma_start(d->rx, (intptr_t)d->rxd);
  gdma_start(d->tx, (intptr_t)d->txd);
  return true;
}

bool dma_copy_busy(const DmaCopy *d) { return d && d->busy; }

void dma_copy_wait(const DmaCopy *d) {
  while (d && d->busy) {
  }
}

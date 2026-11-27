#pragma once
#include <stddef.h>

/* Asynchronous memory-to-memory copies on an ESP32-S3 GDMA channel pair.
 *
 * Used to move data between internal SRAM and PSRAM without the CPU: a CPU
 * memcpy into PSRAM is slow because every destination cache line is first
 * fetched (write-allocate) on a bus the LCD DMA is already busy with, while
 * the DMA engine streams straight to/from PSRAM in 64-byte bursts.
 *
 * Rules: n a multiple of 64; a PSRAM side 64-byte aligned, an SRAM side
 * word aligned. DMA bypasses the data cache, so PSRAM that DMA writes should
 * only ever be read back by DMA (or after invalidating the cache). */
struct DmaCopy;

/* Allocates a channel pair plus descriptors for copies of up to maxBytes. */
DmaCopy *dma_copy_create(size_t maxBytes);

/* Starts a copy. `done(arg)` runs in interrupt context when it completes
 * (may be NULL). Returns false if a copy is still running or the arguments
 * don't meet the alignment rules (nothing is started). */
bool dma_copy_start(DmaCopy *d, void *dst, const void *src, size_t n,
                    void (*done)(void *), void *arg);

bool dma_copy_busy(const DmaCopy *d);
void dma_copy_wait(const DmaCopy *d);

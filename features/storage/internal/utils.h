#ifndef STORAGE_UTILS_H
#define STORAGE_UTILS_H

#include <stdint.h>

uint32_t align_up(uint64_t val, uint64_t size);
uint32_t align_down(uint64_t val, uint64_t size);
uint32_t calc_crc(uint32_t init_crc, uint32_t data_size, const void *data_buf);

#endif

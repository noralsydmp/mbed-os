#include "utils.h"
#include "MbedCRC.h"

using namespace mbed;

uint32_t align_up(uint64_t val, uint64_t size)
{
    return (((val - 1) / size) + 1) * size;
}

uint32_t align_down(uint64_t val, uint64_t size)
{
    return (((val) / size)) * size;
}

uint32_t calc_crc(uint32_t init_crc, uint32_t data_size, const void *data_buf)
{
    uint32_t crc;
    MbedCRC<POLY_32BIT_ANSI, 32> ct(init_crc, 0x0, true, false);
    ct.compute(data_buf, data_size, &crc);
    return crc;
}

/* mbed Microcontroller Library
 *******************************************************************************
 * Copyright (c) 2015, STMicroelectronics
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of STMicroelectronics nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *******************************************************************************
 */


#include "mbed_assert.h"
#include "i2c_api.h"
#include "platform/mbed_wait_api.h"

#if DEVICE_I2C

#include <string.h>
#include "cmsis.h"
#include "pinmap.h"
#include "PeripheralPins.h"
#include "i2c_device.h" // family specific defines
#include <limits.h>

#ifdef I2C_IP_VERSION_V2
#include <stdlib.h>
#endif // I2C_IP_VERSION_V2

#ifndef DEBUG_STDIO
#   define DEBUG_STDIO 0
#endif

#if DEBUG_STDIO
#   include <stdio.h>
#   define DEBUG_PRINTF(...) do { printf(__VA_ARGS__); } while(0)
#else
#   define DEBUG_PRINTF(...) {}
#endif

/*  Family specific description for I2C */
#define I2C_NUM (5)
static I2C_HandleTypeDef *i2c_handles[I2C_NUM];

/* Timeout values are based on core clock and I2C clock.
   The BYTE_TIMEOUT is computed as twice the number of cycles it would
   take to send 10 bits over I2C. Most Flags should take less than that.
   This is for immediate FLAG or ACK check.
*/
#define BYTE_TIMEOUT ((SystemCoreClock / obj_s->hz) * 2 * 10)

/* Timeout values are based on I2C clock. The BYTE_TIMEOUT_US is computed
   as triple the number of cycles it would take to send 10 bits over I2C.
   300 us for 100kHz
   75 us for 400kHz
   30 us for 1MHz
   ...
*/
#define BYTE_TIMEOUT_US   ((1000000 * 10 * 3) / obj_s->hz)

/* Timeout values for flags and events waiting loops. These timeouts are
   not based on accurate values, they just guarantee that the application will
   not remain stuck if the I2C communication is corrupted.
*/
#define FLAG_TIMEOUT ((int)0x1000)

#define MINIMUM_FREQUENCY 100000
#define MAXIMUM_FREQUENCY 1000000

/* GENERIC INIT and HELPERS FUNCTIONS */

#if defined(I2C1_BASE)
static void i2c1_irq(void)
{
    I2C_HandleTypeDef *handle = i2c_handles[0];
    HAL_I2C_EV_IRQHandler(handle);
    HAL_I2C_ER_IRQHandler(handle);
}
#endif
#if defined(I2C2_BASE)
static void i2c2_irq(void)
{
    I2C_HandleTypeDef *handle = i2c_handles[1];
    HAL_I2C_EV_IRQHandler(handle);
    HAL_I2C_ER_IRQHandler(handle);
}
#endif
#if defined(I2C3_BASE)
static void i2c3_irq(void)
{
    I2C_HandleTypeDef *handle = i2c_handles[2];
    HAL_I2C_EV_IRQHandler(handle);
    HAL_I2C_ER_IRQHandler(handle);
}
#endif
#if defined(I2C4_BASE)
static void i2c4_irq(void)
{
    I2C_HandleTypeDef *handle = i2c_handles[3];
    HAL_I2C_EV_IRQHandler(handle);
    HAL_I2C_ER_IRQHandler(handle);
}
#endif
#if defined(FMPI2C1_BASE)
static void i2c5_irq(void)
{
    I2C_HandleTypeDef *handle = i2c_handles[4];
    HAL_I2C_EV_IRQHandler(handle);
    HAL_I2C_ER_IRQHandler(handle);
}
#endif

void i2c_ev_err_enable(i2c_t *obj, uint32_t handler)
{
    struct i2c_s *obj_s = &obj->i2c;
    IRQn_Type irq_event_n = obj_s->event_i2cIRQ;
    IRQn_Type irq_error_n = obj_s->error_i2cIRQ;
    /*  default prio in master case is set to 2 */
    uint32_t prio = 2;

    /* Set up ITs using IRQ and handler tables */
    NVIC_SetVector(irq_event_n, handler);
    NVIC_SetVector(irq_error_n, handler);

#if DEVICE_I2CSLAVE
    /*  Set higher priority to slave device than master.
     *  In case a device makes use of both master and slave, the
     *  slave needs higher responsiveness.
     */
    if (obj_s->slave) {
        prio = 1;
    }
#endif

    NVIC_SetPriority(irq_event_n, prio);
    NVIC_SetPriority(irq_error_n, prio);
    NVIC_EnableIRQ(irq_event_n);
    NVIC_EnableIRQ(irq_error_n);
}

void i2c_ev_err_disable(i2c_t *obj)
{
    struct i2c_s *obj_s = &obj->i2c;
    IRQn_Type irq_event_n = obj_s->event_i2cIRQ;
    IRQn_Type irq_error_n = obj_s->error_i2cIRQ;

    HAL_NVIC_DisableIRQ(irq_event_n);
    HAL_NVIC_DisableIRQ(irq_error_n);
}

uint32_t i2c_get_irq_handler(i2c_t *obj)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);
    uint32_t handler = 0;

    switch (obj_s->index) {
#if defined(I2C1_BASE)
        case 0:
            handler = (uint32_t)&i2c1_irq;
            break;
#endif
#if defined(I2C2_BASE)
        case 1:
            handler = (uint32_t)&i2c2_irq;
            break;
#endif
#if defined(I2C3_BASE)
        case 2:
            handler = (uint32_t)&i2c3_irq;
            break;
#endif
#if defined(I2C4_BASE)
        case 3:
            handler = (uint32_t)&i2c4_irq;
            break;
#endif
#if defined(FMPI2C1_BASE)
        case 4:
            handler = (uint32_t)&i2c5_irq;
            break;
#endif
    }

    i2c_handles[obj_s->index] = handle;
    return handler;
}

void i2c_hw_reset(i2c_t *obj)
{
    int timeout;
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    handle->Instance = (I2C_TypeDef *)(obj_s->i2c);

    // wait before reset
    timeout = BYTE_TIMEOUT;
    while ((__HAL_I2C_GET_FLAG(handle, I2C_FLAG_BUSY)) && (--timeout != 0));
#if defined I2C1_BASE
    if (obj_s->i2c == I2C_1) {
        __HAL_RCC_I2C1_FORCE_RESET();
        __HAL_RCC_I2C1_RELEASE_RESET();
    }
#endif
#if defined I2C2_BASE
    if (obj_s->i2c == I2C_2) {
        __HAL_RCC_I2C2_FORCE_RESET();
        __HAL_RCC_I2C2_RELEASE_RESET();
    }
#endif
#if defined I2C3_BASE
    if (obj_s->i2c == I2C_3) {
        __HAL_RCC_I2C3_FORCE_RESET();
        __HAL_RCC_I2C3_RELEASE_RESET();
    }
#endif
#if defined I2C4_BASE
    if (obj_s->i2c == I2C_4) {
        __HAL_RCC_I2C4_FORCE_RESET();
        __HAL_RCC_I2C4_RELEASE_RESET();
    }
#endif
#if defined FMPI2C1_BASE
    if (obj_s->i2c == FMPI2C_1) {
        __HAL_RCC_FMPI2C1_FORCE_RESET();
        __HAL_RCC_FMPI2C1_RELEASE_RESET();
    }
#endif
}

void i2c_sw_reset(i2c_t *obj)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);
    /*  SW reset procedure:
     *  PE must be kept low during at least 3 APB clock cycles
     *  in order to perform the software reset.
     *  This is ensured by writing the following software sequence:
     *  - Write PE=0
     *  - Check PE=0
     *  - Write PE=1.
     */
    handle->Instance->CR1 &=  ~I2C_CR1_PE;
    while (handle->Instance->CR1 & I2C_CR1_PE);
    handle->Instance->CR1 |=  I2C_CR1_PE;
}

#ifdef DEVICE_I2CSLAVE

static int i2c_slave_read(i2c_t *obj, uint8_t *data, int length)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);
    int ret = 0;
    uint32_t timeout = 0;

    /*  Always use I2C_NEXT_FRAME as slave will just adapt to master requests */
    ret = HAL_I2C_Slave_Sequential_Receive_IT(handle, data, length, I2C_NEXT_FRAME);

    if (ret == HAL_OK) {
        timeout = obj_s->timeout != UINT32_MAX ? obj_s->timeout : BYTE_TIMEOUT_US * length;
        while (obj_s->pending_slave_rx_maxter_tx && (--timeout != 0)) {
            wait_ns(1000);
        }

        if (timeout == 0) {
            DEBUG_PRINTF("TIMEOUT or error in i2c_slave_read\r\n");
        }
    }

    return (length - handle->XferCount);
}

static int i2c_slave_write(i2c_t *obj, const uint8_t *data, int length)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);
    int ret = 0;
    uint32_t timeout = 0;

    /*  Always use I2C_NEXT_FRAME as slave will just adapt to master requests */
    ret = HAL_I2C_Slave_Sequential_Transmit_IT(handle, (uint8_t *) data, length, I2C_NEXT_FRAME);

    if (ret == HAL_OK) {
        timeout = obj_s->timeout != UINT32_MAX ? obj_s->timeout : BYTE_TIMEOUT_US * length;
        while (obj_s->pending_slave_tx_master_rx && (--timeout != 0)) {
            wait_ns(1000);
        }

        if (timeout == 0) {
            DEBUG_PRINTF("TIMEOUT or error in i2c_slave_write\r\n");
        }
    }

    return (length - handle->XferCount);
}

#endif

void i2c_get_capabilities(i2c_capabilities_t *capabilities)
{
    if (capabilities == NULL) {
        return;
    }

    capabilities->minimum_frequency = MINIMUM_FREQUENCY;
    capabilities->maximum_frequency = MAXIMUM_FREQUENCY;
    capabilities->supports_slave_mode = true;
    capabilities->supports_10bit_addressing = false;
    capabilities->supports_multi_master = true;
}

void i2c_init(i2c_t *obj, PinName sda, PinName scl, bool is_slave)
{
    struct i2c_s *obj_s = &obj->i2c;

    // Determine the I2C to use
    I2CName i2c_sda = (I2CName)pinmap_peripheral(sda, PinMap_I2C_SDA);
    I2CName i2c_scl = (I2CName)pinmap_peripheral(scl, PinMap_I2C_SCL);
    obj_s->sda = sda;
    obj_s->scl = scl;

    obj_s->i2c = (I2CName)pinmap_merge(i2c_sda, i2c_scl);
    MBED_ASSERT(obj_s->i2c != (I2CName)NC);

#if defined I2C1_BASE
    // Enable I2C1 clock and pinout if not done
    if (obj_s->i2c == I2C_1) {
        obj_s->index = 0;
        __HAL_RCC_I2C1_CLK_ENABLE();
        // Configure I2C pins
        obj_s->event_i2cIRQ = I2C1_EV_IRQn;
        obj_s->error_i2cIRQ = I2C1_ER_IRQn;
    }
#endif
#if defined I2C2_BASE
    // Enable I2C2 clock and pinout if not done
    if (obj_s->i2c == I2C_2) {
        obj_s->index = 1;
        __HAL_RCC_I2C2_CLK_ENABLE();
        obj_s->event_i2cIRQ = I2C2_EV_IRQn;
        obj_s->error_i2cIRQ = I2C2_ER_IRQn;
    }
#endif
#if defined I2C3_BASE
    // Enable I2C3 clock and pinout if not done
    if (obj_s->i2c == I2C_3) {
        obj_s->index = 2;
        __HAL_RCC_I2C3_CLK_ENABLE();
        obj_s->event_i2cIRQ = I2C3_EV_IRQn;
        obj_s->error_i2cIRQ = I2C3_ER_IRQn;
    }
#endif
#if defined I2C4_BASE
    // Enable I2C3 clock and pinout if not done
    if (obj_s->i2c == I2C_4) {
        obj_s->index = 3;
        __HAL_RCC_I2C4_CLK_ENABLE();
        obj_s->event_i2cIRQ = I2C4_EV_IRQn;
        obj_s->error_i2cIRQ = I2C4_ER_IRQn;
    }
#endif
#if defined FMPI2C1_BASE
    // Enable I2C3 clock and pinout if not done
    if (obj_s->i2c == FMPI2C_1) {
        obj_s->index = 4;
        __HAL_RCC_FMPI2C1_CLK_ENABLE();
        obj_s->event_i2cIRQ = FMPI2C1_EV_IRQn;
        obj_s->error_i2cIRQ = FMPI2C1_ER_IRQn;
    }
#endif

    // Configure I2C pins
    pinmap_pinout(sda, PinMap_I2C_SDA);
    pinmap_pinout(scl, PinMap_I2C_SCL);
    pin_mode(sda, OpenDrainNoPull);
    pin_mode(scl, OpenDrainNoPull);

    // I2C configuration
    // Default hz value used for timeout computation
    if (!obj_s->hz) {
        obj_s->hz = 100000;    // 100 kHz per default
    }

    // Reset to clear pending flags if any
    i2c_hw_reset(obj);
    i2c_frequency(obj, obj_s->hz);

#if DEVICE_I2CSLAVE
    // I2C master by default
    obj_s->slave = (is_slave ? 1 : 0);
    obj_s->pending_slave_tx_master_rx = 0;
    obj_s->pending_slave_rx_maxter_tx = 0;
#endif

#if DEVICE_I2C_ASYNCH
    obj_s->tx_complete = 0;
#endif // DEVICE_I2C_ASYNCH

    // I2C Xfer operation init
    obj_s->event = 0;
    obj_s->XferOperation = I2C_FIRST_AND_LAST_FRAME;
    obj_s->clock_stretching_enabled = I2C_NOSTRETCH_DISABLE;
    obj_s->timeout = UINT32_MAX;
#ifdef I2C_IP_VERSION_V2
    obj_s->pending_start = 0;
#endif

    I2C_HandleTypeDef *handle = &(obj_s->handle);

    if (is_slave) {
        HAL_I2C_EnableListen_IT(handle);
    } else {
        HAL_I2C_DisableListen_IT(handle);
    }
}

void i2c_free(i2c_t *obj)
{
    struct i2c_s *obj_s = &obj->i2c;

    I2C_HandleTypeDef *handle = &(obj_s->handle);

    HAL_I2C_DeInit(handle);
}

uint32_t i2c_frequency(i2c_t *obj, uint32_t frequency)
{
    int timeout;
    uint32_t selected_frequency = frequency;
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    // wait before init
    timeout = BYTE_TIMEOUT;
    while ((__HAL_I2C_GET_FLAG(handle, I2C_FLAG_BUSY)) && (--timeout != 0));

#ifdef I2C_IP_VERSION_V1
    handle->Init.ClockSpeed = selected_frequency;
    handle->Init.DutyCycle       = I2C_DUTYCYCLE_2;
#endif
#ifdef I2C_IP_VERSION_V2
    // Find the closest supported frequency
    static const int supported_hz[] = {100000, 400000, 1000000};
    uint32_t minimum_delta = -1;

    for (size_t i = 0; i < (sizeof(supported_hz) / sizeof(int)); ++i)
    {
        const uint32_t current_delta = abs(supported_hz[i] - frequency);

        if (current_delta < minimum_delta)
            continue;

        selected_frequency = supported_hz[i];
        minimum_delta = current_delta;
    }

    handle->Init.Timing = get_i2c_timing(selected_frequency);

    // Enable the Fast Mode Plus capability
    if (selected_frequency == 1000000) {
#if defined(I2C1_BASE) && defined(__HAL_SYSCFG_FASTMODEPLUS_ENABLE) && defined (I2C_FASTMODEPLUS_I2C1)
        if (obj_s->i2c == I2C_1) {
            HAL_I2CEx_EnableFastModePlus(I2C_FASTMODEPLUS_I2C1);
        }
#endif
#if defined(I2C2_BASE) && defined(__HAL_SYSCFG_FASTMODEPLUS_ENABLE) && defined (I2C_FASTMODEPLUS_I2C2)
        if (obj_s->i2c == I2C_2) {
            HAL_I2CEx_EnableFastModePlus(I2C_FASTMODEPLUS_I2C2);
        }
#endif
#if defined(I2C3_BASE) && defined(__HAL_SYSCFG_FASTMODEPLUS_ENABLE) && defined (I2C_FASTMODEPLUS_I2C3)
        if (obj_s->i2c == I2C_3) {
            HAL_I2CEx_EnableFastModePlus(I2C_FASTMODEPLUS_I2C3);
        }
#endif
#if defined(I2C4_BASE) && defined(__HAL_SYSCFG_FASTMODEPLUS_ENABLE) && defined (I2C_FASTMODEPLUS_I2C4)
        if (obj_s->i2c == I2C_4) {
            HAL_I2CEx_EnableFastModePlus(I2C_FASTMODEPLUS_I2C4);
        }
#endif
    }
#endif //I2C_IP_VERSION_V2

    /*##-1- Configure the I2C clock source. The clock is derived from the SYSCLK #*/
#if defined(I2C1_BASE) && defined (__HAL_RCC_I2C1_CONFIG)
    if (obj_s->i2c == I2C_1) {
        __HAL_RCC_I2C1_CONFIG(I2CAPI_I2C1_CLKSRC);
    }
#endif
#if defined(I2C2_BASE) && defined(__HAL_RCC_I2C2_CONFIG)
    if (obj_s->i2c == I2C_2) {
        __HAL_RCC_I2C2_CONFIG(I2CAPI_I2C2_CLKSRC);
    }
#endif
#if defined(I2C3_BASE) && defined(__HAL_RCC_I2C3_CONFIG)
    if (obj_s->i2c == I2C_3) {
        __HAL_RCC_I2C3_CONFIG(I2CAPI_I2C3_CLKSRC);
    }
#endif
#if defined(I2C4_BASE) && defined(__HAL_RCC_I2C4_CONFIG)
    if (obj_s->i2c == I2C_4) {
        __HAL_RCC_I2C4_CONFIG(I2CAPI_I2C4_CLKSRC);
    }
#endif

#ifdef I2C_ANALOGFILTER_ENABLE
    /* Enable the Analog I2C Filter */
    HAL_I2CEx_ConfigAnalogFilter(handle, I2C_ANALOGFILTER_ENABLE);
#endif

    // I2C configuration
    handle->Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    handle->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    handle->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    handle->Init.NoStretchMode   = obj_s->clock_stretching_enabled;
    handle->Init.OwnAddress1     = 0;
    handle->Init.OwnAddress2     = 0;
    HAL_I2C_Init(handle);

    /*  store frequency for timeout computation */
    obj_s->hz = selected_frequency;

    return obj_s->hz;
}

void i2c_set_clock_stretching(i2c_t *obj, bool enabled)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    obj_s->clock_stretching_enabled =
        enabled ? I2C_NOSTRETCH_DISABLE : I2C_NOSTRETCH_ENABLE;

    // wait before init
    int wait = BYTE_TIMEOUT;
    while ((__HAL_I2C_GET_FLAG(handle, I2C_FLAG_BUSY)) && (--wait != 0));

    // I2C configuration
    handle->Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    handle->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    handle->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    handle->Init.NoStretchMode   = obj_s->clock_stretching_enabled;

    HAL_I2C_Init(handle);
}

void i2c_timeout(i2c_t *obj, uint32_t timeout)
{
    struct i2c_s *obj_s = &obj->i2c;
    // UINT32_MAX is reserved for init value
    obj_s->timeout = timeout == UINT32_MAX ? UINT32_MAX - 1 : timeout;
}


i2c_t *get_i2c_obj(I2C_HandleTypeDef *hi2c)
{
    /* Aim of the function is to get i2c_s pointer using hi2c pointer */
    /* Highly inspired from magical linux kernel's "container_of" */
    /* (which was not directly used since not compatible with IAR toolchain) */
    struct i2c_s *obj_s;
    i2c_t *obj;

    obj_s = (struct i2c_s *)((char *)hi2c - offsetof(struct i2c_s, handle));
    obj = (i2c_t *)((char *)obj_s - offsetof(i2c_t, i2c));

    return (obj);
}

/*
 *  UNITARY APIS.
 *  For very basic operations, direct registers access is needed
 *  There are 2 different IPs version that need to be supported
 */
#ifdef I2C_IP_VERSION_V1
void i2c_start(i2c_t *obj)
{
    int timeout;
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    // Clear Acknowledge failure flag
    __HAL_I2C_CLEAR_FLAG(handle, I2C_FLAG_AF);

    // Wait the STOP condition has been previously correctly sent
    // This timeout can be avoid in some specific cases by simply clearing the STOP bit
    timeout = FLAG_TIMEOUT;
    while ((handle->Instance->CR1 & I2C_CR1_STOP) == I2C_CR1_STOP) {
        if ((timeout--) == 0) {
            return;
        }
    }

    // Generate the START condition
    handle->Instance->CR1 |= I2C_CR1_START;

    // Wait the START condition has been correctly sent
    timeout = FLAG_TIMEOUT;
    while (__HAL_I2C_GET_FLAG(handle, I2C_FLAG_SB) == RESET) {
        if ((timeout--) == 0) {
            return;
        }
    }
}

void i2c_stop(i2c_t *obj)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_TypeDef *i2c = (I2C_TypeDef *)obj_s->i2c;

    // Generate the STOP condition
    i2c->CR1 |= I2C_CR1_STOP;

    /*  In case of mixed usage of the APIs (unitary + SYNC)
     *  re-init HAL state
     */
    if (obj_s->XferOperation != I2C_FIRST_AND_LAST_FRAME) {

#ifdef DEVICE_I2CSLAVE
        const bool is_slave = obj_s->slave ? true : false;
#else
        const bool is_slave = false;
#endif

        i2c_init(obj, obj_s->sda, obj_s->scl, is_slave);
    }
}
#endif //I2C_IP_VERSION_V1
#ifdef I2C_IP_VERSION_V2

void i2c_start(i2c_t *obj)
{
    struct i2c_s *obj_s = &obj->i2c;
    /*  This I2C IP doesn't  */
    obj_s->pending_start = 1;
}

void i2c_stop(i2c_t *obj)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);
    int timeout = FLAG_TIMEOUT;
#if DEVICE_I2CSLAVE
    if (obj_s->slave) {
        /*  re-init slave when stop is requested */
        i2c_init(obj, obj_s->sda, obj_s->scl, true);
        return;
    }
#endif
    // Disable reload mode
    handle->Instance->CR2 &= (uint32_t)~I2C_CR2_RELOAD;

    // Ensure the transmission is started before sending a stop
    if ((handle->Instance->CR2 & (uint32_t)I2C_CR2_RD_WRN) == 0) {
        timeout = FLAG_TIMEOUT;
        while (!__HAL_I2C_GET_FLAG(handle, I2C_FLAG_TXIS)) {
            if ((timeout--) == 0) {
                return;
            }
        }
    }

    // Generate the STOP condition
    handle->Instance->CR2 |= I2C_CR2_STOP;

    timeout = FLAG_TIMEOUT;
    while (!__HAL_I2C_GET_FLAG(handle, I2C_FLAG_STOPF)) {
        if ((timeout--) == 0) {
            return;
        }
    }

    /* Clear STOP Flag */
    __HAL_I2C_CLEAR_FLAG(handle, I2C_FLAG_STOPF);

    /* Erase slave address, this wiil be used as a marker
     * to know when we need to prepare next start */
    handle->Instance->CR2 &=  ~I2C_CR2_SADD;

    /*
     * V2 IP is meant for automatic STOP, not user STOP
     * SW reset the IP state machine before next transaction
     */
    i2c_sw_reset(obj);

    /*  In case of mixed usage of the APIs (unitary + SYNC)
     *  re-init HAL state */
    if (obj_s->XferOperation != I2C_FIRST_AND_LAST_FRAME) {
        i2c_init(obj, obj_s->sda, obj_s->scl, false);
    }
}
#endif //I2C_IP_VERSION_V2

/*
 *  SYNC APIS
 */
int32_t i2c_read(i2c_t *obj, uint16_t address, uint8_t *data, uint32_t length, bool stop)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

#ifdef DEVICE_I2CSLAVE
    if (obj_s->slave == 1) {
        return i2c_slave_read(obj, data, length);
    }
#endif

    // Trick to remove compiler warning "left and right operands are identical" in some cases
    uint32_t op1 = I2C_FIRST_AND_LAST_FRAME;
    uint32_t op2 = I2C_LAST_FRAME;
    if ((obj_s->XferOperation == op1) || (obj_s->XferOperation == op2)) {
        if (stop) {
            obj_s->XferOperation = I2C_FIRST_AND_LAST_FRAME;
        } else {
            obj_s->XferOperation = I2C_FIRST_FRAME;
        }
    } else if ((obj_s->XferOperation == I2C_FIRST_FRAME) ||
               (obj_s->XferOperation == I2C_NEXT_FRAME)) {
        if (stop) {
            obj_s->XferOperation = I2C_LAST_FRAME;
        } else {
            obj_s->XferOperation = I2C_NEXT_FRAME;
        }
    }

    obj_s->event = 0;

    /* Activate default IRQ handlers for sync mode
    * which would be overwritten in async mode
    */
    i2c_ev_err_enable(obj, i2c_get_irq_handler(obj));

    const HAL_StatusTypeDef status = HAL_I2C_Master_Sequential_Receive_IT(
        handle, address, data, length, obj_s->XferOperation);

    if (status != HAL_OK) {
        DEBUG_PRINTF("ERROR in i2c_read:%d\r\n", status);

        return I2C_ERROR_BUS_BUSY;
    }

    // + 1 for addressing stage
    uint32_t timeout = obj_s->timeout != UINT32_MAX ? obj_s->timeout : BYTE_TIMEOUT_US * (length + 1);

    /*  transfer started : wait completion or timeout */
    while (!(obj_s->event & I2C_EVENT_ALL) && (--timeout != 0)) {
        wait_ns(1000);
    }

    i2c_ev_err_disable(obj);

    if ((timeout == 0) || (obj_s->event != I2C_EVENT_TRANSFER_COMPLETE)) {
        DEBUG_PRINTF(" TIMEOUT or error in i2c_read\r\n");

#ifdef DEVICE_I2CSLAVE
        i2c_init(obj, obj_s->sda, obj_s->scl, obj_s->slave);
#else
        i2c_init(obj, obj_s->sda, obj_s->scl, false);
#endif // DEVICE_I2CSLAVE
        return I2C_ERROR_TIMEOUT;
    } else if (obj_s->event == I2C_EVENT_ARBITRATION_LOST) {
        return I2C_ERROR_ARBITRATION_LOST;
    }

    return (length - handle->XferCount);
}

int32_t i2c_write(i2c_t *obj, uint16_t address, const uint8_t *data, uint32_t length, bool stop)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

#ifdef DEVICE_I2CSLAVE
    if (obj_s->slave == 1) {
        return i2c_slave_write(obj, data, length);
    }
#endif

    // Trick to remove compiler warning "left and right operands are identical" in some cases
    uint32_t op1 = I2C_FIRST_AND_LAST_FRAME;
    uint32_t op2 = I2C_LAST_FRAME;
    if ((obj_s->XferOperation == op1) || (obj_s->XferOperation == op2)) {
        if (stop) {
            obj_s->XferOperation = I2C_FIRST_AND_LAST_FRAME;
        } else {
            obj_s->XferOperation = I2C_FIRST_FRAME;
        }
    } else if ((obj_s->XferOperation == I2C_FIRST_FRAME) ||
               (obj_s->XferOperation == I2C_NEXT_FRAME)) {
        if (stop) {
            obj_s->XferOperation = I2C_LAST_FRAME;
        } else {
            obj_s->XferOperation = I2C_NEXT_FRAME;
        }
    }

    obj_s->event = 0;

    i2c_ev_err_enable(obj, i2c_get_irq_handler(obj));

    const HAL_StatusTypeDef status = HAL_I2C_Master_Sequential_Transmit_IT(
        handle, address, (uint8_t *) data, length, obj_s->XferOperation);

    if (status != HAL_OK) {
      DEBUG_PRINTF("ERROR in i2c_write\r\n");

      return I2C_ERROR_BUS_BUSY;
    }

    // + 1 for addressing stage
    uint32_t timeout = obj_s->timeout != UINT32_MAX ? obj_s->timeout : BYTE_TIMEOUT_US * (length + 1);

    /*  transfer started : wait completion or timeout */
    while (!(obj_s->event & I2C_EVENT_ALL) && (--timeout != 0)) {
        wait_ns(1000);
    }

    i2c_ev_err_disable(obj);

    if ((timeout == 0) || (obj_s->event != I2C_EVENT_TRANSFER_COMPLETE)) {
        DEBUG_PRINTF(" TIMEOUT or error in i2c_write\r\n");

#ifdef DEVICE_I2CSLAVE
        i2c_init(obj, obj_s->sda, obj_s->scl, obj_s->slave);
#else
        i2c_init(obj, obj_s->sda, obj_s->scl, false);
#endif // DEVICE_I2CSLAVE

        return I2C_ERROR_TIMEOUT;
    } else if (obj_s->event == I2C_EVENT_ARBITRATION_LOST) {
        return I2C_ERROR_ARBITRATION_LOST;
    }

    return (length - handle->XferCount);
}

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    /* Get object ptr based on handler ptr */
    i2c_t *obj = get_i2c_obj(hi2c);
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    obj_s->tx_complete = 1;

#if DEVICE_I2C_ASYNCH
    /* Handle potential Tx/Rx use case */
    if ((obj->tx_buff.length) && (obj->rx_buff.length)) {
        if (obj_s->stop) {
            obj_s->XferOperation = I2C_LAST_FRAME;
        } else {
            obj_s->XferOperation = I2C_NEXT_FRAME;
        }

        HAL_I2C_Master_Sequential_Receive_IT(hi2c, obj_s->address, (uint8_t *)obj->rx_buff.buffer, obj->rx_buff.length, obj_s->XferOperation);
    } else
#endif
    {
        /* Set event flag */
        obj_s->event = I2C_EVENT_TRANSFER_COMPLETE;

#if DEVICE_I2C_ASYNCH
        /* */
        if (obj->handler == NULL) {
            return;
        }

        i2c_async_event_t event;
        event.sent_bytes     = (obj->tx_buff.length - handle->XferCount);
        event.received_bytes = 0;
        event.error          = false;

        obj->handler(obj, &event, obj->ctx);

        obj->handler = NULL;
        obj->ctx     = NULL;
#endif // DEVICE_I2C_ASYNCH
    }
}

void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    /* Get object ptr based on handler ptr */
    i2c_t *obj = get_i2c_obj(hi2c);
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    /* Set event flag */
    obj_s->event = I2C_EVENT_TRANSFER_COMPLETE;

#if DEVICE_I2C_ASYNCH
    /* */
    if (obj->handler == NULL) {
      return;
    }

    i2c_async_event_t event;
    event.sent_bytes     = obj->tx_buff.length;
    event.received_bytes = (obj->rx_buff.length - handle->XferCount);
    event.error          = false;

    obj->handler(obj, &event, obj->ctx);

    obj->handler = NULL;
    obj->ctx     = NULL;
#endif // DEVICE_I2C_ASYNCH
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    /* Get object ptr based on handler ptr */
    i2c_t *obj = get_i2c_obj(hi2c);
    struct i2c_s *obj_s = &obj->i2c;
#if DEVICE_I2CSLAVE
    I2C_HandleTypeDef *handle = &(obj_s->handle);
    uint16_t address = 0;
    /*  Store address to handle it after reset */
    if (obj_s->slave) {
        address = handle->Init.OwnAddress1;
    }
#endif

    DEBUG_PRINTF("HAL_I2C_ErrorCallback:%d, index=%d\r\n", (int) hi2c->ErrorCode, obj_s->index);

    /* re-init IP to try and get back in a working state */
#ifdef DEVICE_I2CSLAVE
    const bool is_slave = obj_s->slave ? true : false;
#else
    const bool is_slave = false;
#endif
    i2c_init(obj, obj_s->sda, obj_s->scl, is_slave);

#if DEVICE_I2CSLAVE
    /*  restore slave address */
    if (address != 0) {
        obj_s->slave = 1;
        i2c_slave_address(obj, address);
    }
#endif

    /* Keep Set event flag */
    obj_s->event = I2C_EVENT_ERROR;

#if DEVICE_I2C_ASYNCH
    /* */
    if (obj->handler == NULL) {
      return;
    }

    i2c_async_event_t event;
    event.error = true;
    if (hi2c->ErrorCode & HAL_I2C_ERROR_ARLO) {
        event.error_status = I2C_ERROR_ARBITRATION_LOST;
    }

    if (!obj_s->tx_complete) {
        event.sent_bytes     = (obj->tx_buff.length - handle->XferCount);
        event.received_bytes = 0;
    } else {
        event.sent_bytes     = (obj->tx_buff.length);
        event.received_bytes = (obj->rx_buff.length - handle->XferCount);
    }

    obj->handler(obj, &event, obj->ctx);

    obj->handler = NULL;
    obj->ctx     = NULL;
#endif // DEVICE_I2C_ASYNCH
}

const PinMap *i2c_master_sda_pinmap()
{
    return PinMap_I2C_SDA;
}

const PinMap *i2c_master_scl_pinmap()
{
    return PinMap_I2C_SCL;
}

const PinMap *i2c_slave_sda_pinmap()
{
    return PinMap_I2C_SDA;
}

const PinMap *i2c_slave_scl_pinmap()
{
    return PinMap_I2C_SCL;
}

#if DEVICE_I2CSLAVE
/* SLAVE API FUNCTIONS */
void i2c_slave_address(i2c_t *obj, uint16_t address)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    // I2C configuration
    handle->Init.OwnAddress1 = address;
    HAL_I2C_Init(handle);

    i2c_ev_err_enable(obj, i2c_get_irq_handler(obj));

    HAL_I2C_EnableListen_IT(handle);
}

void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t TransferDirection, uint16_t AddrMatchCode)
{
    /* Get object ptr based on handler ptr */
    i2c_t *obj = get_i2c_obj(hi2c);
    struct i2c_s *obj_s = &obj->i2c;

    /*  Transfer direction in HAL is from Master point of view */
    if (TransferDirection == I2C_DIRECTION_RECEIVE) {
        obj_s->pending_slave_tx_master_rx = 1;
    }

    if (TransferDirection == I2C_DIRECTION_TRANSMIT) {
        obj_s->pending_slave_rx_maxter_tx = 1;
    }
}

void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *I2cHandle)
{
    /* Get object ptr based on handler ptr */
    i2c_t *obj = get_i2c_obj(I2cHandle);
    struct i2c_s *obj_s = &obj->i2c;
    obj_s->pending_slave_tx_master_rx = 0;
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *I2cHandle)
{
    /* Get object ptr based on handler ptr */
    i2c_t *obj = get_i2c_obj(I2cHandle);
    struct i2c_s *obj_s = &obj->i2c;
    obj_s->pending_slave_rx_maxter_tx = 0;
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    /* restart listening for master requests */
    HAL_I2C_EnableListen_IT(hi2c);
}

i2c_slave_status_t i2c_slave_status(i2c_t *obj)
{
    struct i2c_s *obj_s = &obj->i2c;

    i2c_slave_status_t retValue = Idle;

    if (obj_s->pending_slave_rx_maxter_tx) {
        retValue = WriteAddressed;
    }

    if (obj_s->pending_slave_tx_master_rx) {
        retValue = ReadAddressed;
    }

    return retValue;
}
#endif // DEVICE_I2CSLAVE

#if DEVICE_I2C_ASYNCH
/* ASYNCH MASTER API FUNCTIONS */
void HAL_I2C_AbortCpltCallback(I2C_HandleTypeDef *hi2c)
{
    /* Get object ptr based on handler ptr */
    i2c_t *obj = get_i2c_obj(hi2c);
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    /* Disable IT. Not always done before calling macro */
    __HAL_I2C_DISABLE_IT(handle, I2C_IT_ALL);
    i2c_ev_err_disable(obj);

    /* Set event flag */
    obj_s->event = I2C_EVENT_ERROR;

    if (obj->handler == NULL) {
      return;
    }

#if DEVICE_I2C_ASYNCH
    i2c_async_event_t event;
    event.error = true;

    if (!obj_s->tx_complete) {
        event.sent_bytes     = (obj->tx_buff.length - handle->XferCount);
        event.received_bytes = 0;
    } else {
        event.sent_bytes     = (obj->tx_buff.length);
        event.received_bytes = (obj->rx_buff.length - handle->XferCount);
    }

    obj->handler(obj, &event, obj->ctx);

    obj->handler = NULL;
    obj->ctx     = NULL;
#endif // DEVICE_I2C_ASYNCH
}

bool i2c_transfer_async(i2c_t *obj, const uint8_t *tx, uint32_t tx_length,
                        uint8_t *rx, uint32_t rx_length, uint16_t address,
                        bool stop, i2c_async_handler_f handler, void *ctx)
{
    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);
    HAL_StatusTypeDef ret = HAL_ERROR;

    /* Update object */
    obj->tx_buff.buffer = (void *)tx;
    obj->tx_buff.length = tx_length;
    obj->tx_buff.pos    = 0;
    obj->tx_buff.width  = 8;

    obj->rx_buff.buffer = (void *)rx;
    obj->rx_buff.length = rx_length;
    obj->rx_buff.pos    = SIZE_MAX;
    obj->rx_buff.width  = 8;

    obj->handler = handler;
    obj->ctx     = ctx;

    obj_s->event = 0;
    obj_s->address = address;
    obj_s->stop = stop;
    obj_s->tx_complete = 0;

    i2c_ev_err_enable(obj, i2c_get_irq_handler(obj));

    /* Set operation step depending if stop sending required or not */
    if ((tx_length && !rx_length) || (!tx_length && rx_length)) {
        // Trick to remove compiler warning "left and right operands are identical" in some cases
        uint32_t op1 = I2C_FIRST_AND_LAST_FRAME;
        uint32_t op2 = I2C_LAST_FRAME;
        if ((obj_s->XferOperation == op1) || (obj_s->XferOperation == op2)) {
            if (stop) {
                obj_s->XferOperation = I2C_FIRST_AND_LAST_FRAME;
            } else {
                obj_s->XferOperation = I2C_FIRST_FRAME;
            }
        } else if ((obj_s->XferOperation == I2C_FIRST_FRAME) ||
                   (obj_s->XferOperation == I2C_NEXT_FRAME)) {
            if (stop) {
                obj_s->XferOperation = I2C_LAST_FRAME;
            } else {
                obj_s->XferOperation = I2C_NEXT_FRAME;
            }
        }

        if (tx_length > 0) {
            ret = HAL_I2C_Master_Sequential_Transmit_IT(handle, address, (uint8_t *) tx, tx_length, obj_s->XferOperation);
        }
        if (rx_length > 0) {
            ret = HAL_I2C_Master_Sequential_Receive_IT(handle, address, (uint8_t *) rx, rx_length, obj_s->XferOperation);
        }
    } else if (tx_length && rx_length) {
        /* Two steps operation, don't modify XferOperation, keep it for next step */
        // Trick to remove compiler warning "left and right operands are identical" in some cases
        uint32_t op1 = I2C_FIRST_AND_LAST_FRAME;
        uint32_t op2 = I2C_LAST_FRAME;
        if ((obj_s->XferOperation == op1) || (obj_s->XferOperation == op2)) {
            ret = HAL_I2C_Master_Sequential_Transmit_IT(handle, address, (uint8_t *) tx, tx_length, I2C_FIRST_FRAME);
        } else if ((obj_s->XferOperation == I2C_FIRST_FRAME) ||
                   (obj_s->XferOperation == I2C_NEXT_FRAME)) {
            ret = HAL_I2C_Master_Sequential_Transmit_IT(handle, address, (uint8_t *) tx, tx_length, I2C_NEXT_FRAME);
        }
    }
    return (ret == HAL_OK);
}

void i2c_abort_async(i2c_t *obj)
{

    struct i2c_s *obj_s = &obj->i2c;
    I2C_HandleTypeDef *handle = &(obj_s->handle);

    /* Abort HAL requires DevAddress, but is not used. Use Dummy */
    uint16_t Dummy_DevAddress = 0x00;

    HAL_I2C_Master_Abort_IT(handle, Dummy_DevAddress);
}

#endif // DEVICE_I2C_ASYNCH

#endif // DEVICE_I2C

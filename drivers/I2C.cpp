/* mbed Microcontroller Library
 * Copyright (c) 2006-2015 ARM Limited
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "drivers/I2C.h"
#include "drivers/DigitalInOut.h"
#include "platform/mbed_wait_api.h"
#include "platform/mbed_assert.h"

#if DEVICE_I2C

#if DEVICE_I2C_ASYNCH
#include "platform/mbed_power_mgmt.h"
#endif

namespace mbed {

I2C *I2C::_owner = NULL;
SingletonPtr<PlatformMutex> I2C::_mutex;

I2C::I2C(PinName sda, PinName scl) :
#if DEVICE_I2C_ASYNCH
    _deep_sleep_locked(false),
    _async_transfer_ongoing(false),
#endif
    _i2c(), _hz(100000)
{
    lock();
    // The init function also set the frequency to 100000
    _sda = sda;
    _scl = scl;
    recover(sda, scl);
    i2c_init(&_i2c, sda, scl, false);
    frequency(_hz);

    // Used to avoid unnecessary frequency updates
    _owner = this;
    unlock();
}

I2C::~I2C()
{
    i2c_free(&_i2c);
}

void I2C::frequency(int hz)
{
    lock();
    MBED_ASSERT(_hz > 0);
    _hz = (uint32_t)hz;

    // We want to update the frequency even if we are already the bus owners
    i2c_frequency(&_i2c, _hz);

    // Updating the frequency of the bus we become the owners of it
    _owner = this;
    unlock();
}

void I2C::timeout(uint32_t timeout)
{
    lock();
    i2c_timeout(&_i2c, timeout);
    unlock();
}


void I2C::aquire()
{
    lock();
    if (_owner != this) {
        i2c_frequency(&_i2c, _hz);
        _owner = this;
    }
    unlock();
}

// write - Master Transmitter Mode
int I2C::write(int address, const char *data, int length, bool repeated)
{
    lock();
    aquire();

    int stop = (repeated) ? 0 : 1;
    int written = i2c_write(&_i2c, address, (const uint8_t *)data, length, stop);

    unlock();
    return length != written;
}

int I2C::write(int data)
{
    lock();
    uint8_t byte = data;
    int ret = i2c_write(&_i2c, 0, &byte, 1, false);
    unlock();
    return ret;
}

// read - Master Receiver Mode
int I2C::read(int address, char *data, int length, bool repeated)
{
    lock();
    aquire();

    int stop = (repeated) ? 0 : 1;
    int read = i2c_read(&_i2c, address, (uint8_t *)data, length, stop);

    unlock();
    return length != read;
}

int I2C::read(int ack)
{
    lock();
    uint8_t ret;
    i2c_read(&_i2c, 0, &ret, 1, (ack == 0));
    unlock();
    return ret;
}

void I2C::start(void)
{
    lock();
    i2c_start(&_i2c);
    unlock();
}

void I2C::stop(void)
{
    lock();
    i2c_stop(&_i2c);
    unlock();
}

void I2C::lock()
{
    _mutex->lock();
}

void I2C::unlock()
{
    _mutex->unlock();
}

int I2C::recover(PinName sda, PinName scl)
{
    DigitalInOut pin_sda(sda, PIN_INPUT, PullNone, 1);
    DigitalInOut pin_scl(scl, PIN_INPUT, PullNone, 1);

    // Return as SCL is low and no access to become master.
    if (pin_scl == 0) {
        return I2C_ERROR_BUS_BUSY;
    }

    // Return successfully as SDA and SCL is high
    if (pin_sda == 1) {
        return 0;
    }

    // Send clock pulses, for device to recover 9
    pin_scl.mode(PullNone);
    pin_scl.output();
    for (int count = 0; count < 10; count++) {
        pin_scl.mode(PullNone);
        pin_scl = 0;
        wait_us(5);
        pin_scl.mode(PullUp);
        pin_scl = 1;
        wait_us(5);
    }

    // Send Stop
    pin_sda.output();
    pin_sda = 0;
    wait_us(5);
    pin_scl = 1;
    wait_us(5);
    pin_sda = 1;
    wait_us(5);

    pin_sda.input();
    pin_scl.input();
    if ((pin_scl == 0) || (pin_sda == 0)) {
        // Return as SCL is low and no access to become master.
        return I2C_ERROR_BUS_BUSY;
    }

    return 0;
}

#if DEVICE_I2C_ASYNCH

int I2C::transfer(int address, const char *tx_buffer, int tx_length, char *rx_buffer, int rx_length, const event_callback_t &callback, int event, bool repeated)
{
    lock();
    if (_async_transfer_ongoing) {
        unlock();
        return -1;
    }
    lock_deep_sleep();
    aquire();

    _callback = callback;
    bool stop = (repeated) ? false : true;
    _async_transfer_ongoing = true;
    i2c_transfer_async(&_i2c, (const uint8_t *)tx_buffer, tx_length, (uint8_t *)rx_buffer, rx_length, address, stop, &I2C::irq_handler_asynch, (void *)this);
    unlock();
    return 0;
}

void I2C::abort_transfer(void)
{
    lock();
    i2c_abort_async(&_i2c);
    _async_transfer_ongoing = false;
    unlock_deep_sleep();
    unlock();
}

void I2C::irq_handler_asynch(i2c_t *obj, i2c_async_event_t *event, void *ctx)
{
    I2C *self = (I2C *)ctx;
    if (self->_callback) {
        self->_callback.call(event->error ? I2C_EVENT_ERROR : I2C_EVENT_TRANSFER_COMPLETE);
    }
    self->_async_transfer_ongoing = false;
    self->unlock_deep_sleep();
}

void I2C::lock_deep_sleep()
{
    if (_deep_sleep_locked == false) {
        sleep_manager_lock_deep_sleep();
        _deep_sleep_locked = true;
    }
}

void I2C::unlock_deep_sleep()
{
    if (_deep_sleep_locked == true) {
        sleep_manager_unlock_deep_sleep();
        _deep_sleep_locked = false;
    }
}

#endif

} // namespace mbed

#endif

/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>

#include "py/nlr.h"
#include "py/runtime.h"
#include "irq.h"
#include "pin.h"
#include "genhdr/pins.h"
#include "bufhelper.h"
#include "dma.h"
#include "i2c.h"
#include MICROPY_HAL_H

#if !defined(STM32F7)
// The STM32F7 has Timing, where the F4 has ClockSpeed and DutyCycle, so we
// need to figure that out before we can enable i2c

/// \moduleref pyb
/// \class I2C - a two-wire serial protocol
///
/// I2C is a two-wire protocol for communicating between devices.  At the physical
/// level it consists of 2 wires: SCL and SDA, the clock and data lines respectively.
///
/// I2C objects are created attached to a specific bus.  They can be initialised
/// when created, or initialised later on:
///
///     from pyb import I2C
///
///     i2c = I2C(1)                         # create on bus 1
///     i2c = I2C(1, I2C.MASTER)             # create and init as a master
///     i2c.init(I2C.MASTER, baudrate=20000) # init as a master
///     i2c.init(I2C.SLAVE, addr=0x42)       # init as a slave with given address
///     i2c.deinit()                         # turn off the peripheral
///
/// Printing the i2c object gives you information about its configuration.
///
/// Basic methods for slave are send and recv:
///
///     i2c.send('abc')      # send 3 bytes
///     i2c.send(0x42)       # send a single byte, given by the number
///     data = i2c.recv(3)   # receive 3 bytes
///
/// To receive inplace, first create a bytearray:
///
///     data = bytearray(3)  # create a buffer
///     i2c.recv(data)       # receive 3 bytes, writing them into data
///
/// You can specify a timeout (in ms):
///
///     i2c.send(b'123', timeout=2000)   # timout after 2 seconds
///
/// A master must specify the recipient's address:
///
///     i2c.init(I2C.MASTER)
///     i2c.send('123', 0x42)        # send 3 bytes to slave with address 0x42
///     i2c.send(b'456', addr=0x42)  # keyword for address
///
/// Master also has other methods:
///
///     i2c.is_ready(0x42)           # check if slave 0x42 is ready
///     i2c.scan()                   # scan for slaves on the bus, returning
///                                  #   a list of valid addresses
///     i2c.mem_read(3, 0x42, 2)     # read 3 bytes from memory of slave 0x42,
///                                  #   starting at address 2 in the slave
///     i2c.mem_write('abc', 0x42, 2, timeout=1000)

// Possible DMA configurations for I2C busses:
// I2C1_TX: DMA1_Stream6.CHANNEL_1 or DMA1_Stream7.CHANNEL_1
// I2C1_RX: DMA1_Stream0.CHANNEL_1 or DMA1_Stream5.CHANNEL_1
// I2C2_TX: DMA1_Stream7.CHANNEL_7
// I2C2_RX: DMA1_Stream2.CHANNEL_7 or DMA1_Stream3.CHANNEL_7
// I2C3_TX: DMA1_Stream4.CHANNEL_3
// I2C3_RX: DMA1_Stream2.CHANNEL_3

#define PYB_I2C_MASTER (0)
#define PYB_I2C_SLAVE  (1)

typedef struct _pyb_i2c_obj_t {
    mp_obj_base_t base;
    I2C_HandleTypeDef *i2c;
    DMA_Stream_TypeDef *tx_dma_stream;
    uint32_t tx_dma_channel;
    DMA_Stream_TypeDef *rx_dma_stream;
    uint32_t rx_dma_channel;
} pyb_i2c_obj_t;

#if defined(MICROPY_HW_I2C1_SCL)
I2C_HandleTypeDef I2CHandle1 = {.Instance = NULL};
#endif
#if defined(MICROPY_HW_I2C2_SCL)
I2C_HandleTypeDef I2CHandle2 = {.Instance = NULL};
#endif
#if defined(MICROPY_HW_I2C3_SCL)
I2C_HandleTypeDef I2CHandle3 = {.Instance = NULL};
#endif

STATIC const pyb_i2c_obj_t pyb_i2c_obj[] = {
    #if defined(MICROPY_HW_I2C1_SCL)
    {{&pyb_i2c_type}, &I2CHandle1, DMA1_Stream7, DMA_CHANNEL_1, DMA1_Stream0, DMA_CHANNEL_1},
    #else
    {{&pyb_i2c_type}, NULL, NULL, 0, NULL, 0},
    #endif
    #if defined(MICROPY_HW_I2C2_SCL)
    {{&pyb_i2c_type}, &I2CHandle2, DMA1_Stream7, DMA_CHANNEL_7, DMA1_Stream2, DMA_CHANNEL_7},
    #else
    {{&pyb_i2c_type}, NULL, NULL, 0, NULL, 0},
    #endif
    #if defined(MICROPY_HW_I2C3_SCL)
    {{&pyb_i2c_type}, &I2CHandle3, DMA1_Stream4, DMA_CHANNEL_3, DMA1_Stream2, DMA_CHANNEL_3},
    #else
    {{&pyb_i2c_type}, NULL, NULL, 0, NULL, 0},
    #endif
};

void i2c_init0(void) {
    // reset the I2C1 handles
    #if defined(MICROPY_HW_I2C1_SCL)
    memset(&I2CHandle1, 0, sizeof(I2C_HandleTypeDef));
    I2CHandle1.Instance = I2C1;
    #endif
    #if defined(MICROPY_HW_I2C2_SCL)
    memset(&I2CHandle2, 0, sizeof(I2C_HandleTypeDef));
    I2CHandle2.Instance = I2C2;
    #endif
    #if defined(MICROPY_HW_I2C3_SCL)
    memset(&I2CHandle3, 0, sizeof(I2C_HandleTypeDef));
    I2CHandle3.Instance = I2C3;
    #endif
}

void i2c_init(I2C_HandleTypeDef *i2c) {
    // init the GPIO lines
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStructure.Speed = GPIO_SPEED_FAST;
    GPIO_InitStructure.Pull = GPIO_NOPULL; // have external pull-up resistors on both lines

    const pyb_i2c_obj_t *self;
    const pin_obj_t *pins[2];

    if (0) {
    #if defined(MICROPY_HW_I2C1_SCL)
    } else if (i2c == &I2CHandle1) {
        self = &pyb_i2c_obj[0];
        pins[0] = &MICROPY_HW_I2C1_SCL;
        pins[1] = &MICROPY_HW_I2C1_SDA;
        GPIO_InitStructure.Alternate = GPIO_AF4_I2C1;
        __I2C1_CLK_ENABLE();
    #endif
    #if defined(MICROPY_HW_I2C2_SCL)
    } else if (i2c == &I2CHandle2) {
        self = &pyb_i2c_obj[1];
        pins[0] = &MICROPY_HW_I2C2_SCL;
        pins[1] = &MICROPY_HW_I2C2_SDA;
        GPIO_InitStructure.Alternate = GPIO_AF4_I2C2;
        __I2C2_CLK_ENABLE();
    #endif
    #if defined(MICROPY_HW_I2C3_SCL)
    } else if (i2c == &I2CHandle3) {
        self = &pyb_i2c_obj[2];
        pins[0] = &MICROPY_HW_I2C3_SCL;
        pins[1] = &MICROPY_HW_I2C3_SDA;
        GPIO_InitStructure.Alternate = GPIO_AF4_I2C3;
        __I2C3_CLK_ENABLE();
    #endif
    } else {
        // I2C does not exist for this board (shouldn't get here, should be checked by caller)
        return;
    }

    // init the GPIO lines
    for (uint i = 0; i < 2; i++) {
        mp_hal_gpio_clock_enable(pins[i]->gpio);
        GPIO_InitStructure.Pin = pins[i]->pin_mask;
        HAL_GPIO_Init(pins[i]->gpio, &GPIO_InitStructure);
    }

    // init the I2C device
    if (HAL_I2C_Init(i2c) != HAL_OK) {
        // init error
        // TODO should raise an exception, but this function is not necessarily going to be
        // called via Python, so may not be properly wrapped in an NLR handler
        printf("OSError: HAL_I2C_Init failed\n");
        return;
    }

    // invalidate the DMA channels so they are initialised on first use
    dma_invalidate_channel(self->tx_dma_stream, self->tx_dma_channel);
    dma_invalidate_channel(self->rx_dma_stream, self->rx_dma_channel);
}

void i2c_deinit(I2C_HandleTypeDef *i2c) {
    HAL_I2C_DeInit(i2c);
    if (0) {
    #if defined(MICROPY_HW_I2C1_SCL)
    } else if (i2c->Instance == I2C1) {
        __I2C1_FORCE_RESET();
        __I2C1_RELEASE_RESET();
        __I2C1_CLK_DISABLE();
    #endif
    #if defined(MICROPY_HW_I2C2_SCL)
    } else if (i2c->Instance == I2C2) {
        __I2C2_FORCE_RESET();
        __I2C2_RELEASE_RESET();
        __I2C2_CLK_DISABLE();
    #endif
    #if defined(MICROPY_HW_I2C3_SCL)
    } else if (i2c->Instance == I2C3) {
        __I2C3_FORCE_RESET();
        __I2C3_RELEASE_RESET();
        __I2C3_CLK_DISABLE();
    #endif
    }
}

STATIC HAL_StatusTypeDef i2c_wait_dma_finished(I2C_HandleTypeDef *i2c, uint32_t timeout) {
    // Note: we can't use WFI to idle in this loop because the DMA completion
    // interrupt may occur before the WFI.  Hence we miss it and have to wait
    // until the next sys-tick (up to 1ms).
    uint32_t start = HAL_GetTick();
    while (HAL_I2C_GetState(i2c) != HAL_I2C_STATE_READY) {
        if (HAL_GetTick() - start >= timeout) {
            return HAL_TIMEOUT;
        }
    }
    return HAL_OK;
}

/******************************************************************************/
/* Micro Python bindings                                                      */

STATIC inline bool in_master_mode(pyb_i2c_obj_t *self) { return self->i2c->Init.OwnAddress1 == PYB_I2C_MASTER_ADDRESS; }

STATIC void pyb_i2c_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    pyb_i2c_obj_t *self = self_in;

    uint i2c_num = 0;
    if (0) { }
    #if defined(MICROPY_HW_I2C1_SCL)
    else if (self->i2c->Instance == I2C1) { i2c_num = 1; }
    #endif
    #if defined(MICROPY_HW_I2C2_SCL)
    else if (self->i2c->Instance == I2C2) { i2c_num = 2; }
    #endif
    #if defined(MICROPY_HW_I2C3_SCL)
    else if (self->i2c->Instance == I2C3) { i2c_num = 3; }
    #endif

    if (self->i2c->State == HAL_I2C_STATE_RESET) {
        mp_printf(print, "I2C(%u)", i2c_num);
    } else {
        if (in_master_mode(self)) {
            mp_printf(print, "I2C(%u, I2C.MASTER, baudrate=%u)", i2c_num, self->i2c->Init.ClockSpeed);
        } else {
            mp_printf(print, "I2C(%u, I2C.SLAVE, addr=0x%02x)", i2c_num, (self->i2c->Instance->OAR1 >> 1) & 0x7f);
        }
    }
}

/// \method init(mode, *, addr=0x12, baudrate=400000, gencall=False)
///
/// Initialise the I2C bus with the given parameters:
///
///   - `mode` must be either `I2C.MASTER` or `I2C.SLAVE`
///   - `addr` is the 7-bit address (only sensible for a slave)
///   - `baudrate` is the SCL clock rate (only sensible for a master)
///   - `gencall` is whether to support general call mode
STATIC mp_obj_t pyb_i2c_init_helper(const pyb_i2c_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_addr,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0x12} },
        { MP_QSTR_baudrate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 400000} },
        { MP_QSTR_gencall,  MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };

    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // set the I2C configuration values
    I2C_InitTypeDef *init = &self->i2c->Init;

    if (args[0].u_int == PYB_I2C_MASTER) {
        // use a special address to indicate we are a master
        init->OwnAddress1 = PYB_I2C_MASTER_ADDRESS;
    } else {
        init->OwnAddress1 = (args[1].u_int << 1) & 0xfe;
    }

    init->AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    init->ClockSpeed      = MIN(args[2].u_int, 400000);
    init->DualAddressMode = I2C_DUALADDRESS_DISABLED;
    init->DutyCycle       = I2C_DUTYCYCLE_16_9;
    init->GeneralCallMode = args[3].u_bool ? I2C_GENERALCALL_ENABLED : I2C_GENERALCALL_DISABLED;
    init->NoStretchMode   = I2C_NOSTRETCH_DISABLED;
    init->OwnAddress2     = 0xfe; // unused

    // init the I2C bus
    i2c_init(self->i2c);

    return mp_const_none;
}

/// \classmethod \constructor(bus, ...)
///
/// Construct an I2C object on the given bus.  `bus` can be 1 or 2.
/// With no additional parameters, the I2C object is created but not
/// initialised (it has the settings from the last initialisation of
/// the bus, if any).  If extra arguments are given, the bus is initialised.
/// See `init` for parameters of initialisation.
///
/// The physical pins of the I2C busses are:
///
///   - `I2C(1)` is on the X position: `(SCL, SDA) = (X9, X10) = (PB6, PB7)`
///   - `I2C(2)` is on the Y position: `(SCL, SDA) = (Y9, Y10) = (PB10, PB11)`
STATIC mp_obj_t pyb_i2c_make_new(mp_obj_t type_in, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    // check arguments
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    // work out i2c bus
    int i2c_id = 0;
    if (MP_OBJ_IS_STR(args[0])) {
        const char *port = mp_obj_str_get_str(args[0]);
        if (0) {
        #ifdef MICROPY_HW_I2C1_NAME
        } else if (strcmp(port, MICROPY_HW_I2C1_NAME) == 0) {
            i2c_id = 1;
        #endif
        #ifdef MICROPY_HW_I2C2_NAME
        } else if (strcmp(port, MICROPY_HW_I2C2_NAME) == 0) {
            i2c_id = 2;
        #endif
        #ifdef MICROPY_HW_I2C3_NAME
        } else if (strcmp(port, MICROPY_HW_I2C3_NAME) == 0) {
            i2c_id = 3;
        #endif
        } else {
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_ValueError,
                "I2C(%s) does not exist", port));
        }
    } else {
        i2c_id = mp_obj_get_int(args[0]);
        if (i2c_id < 1 || i2c_id > MP_ARRAY_SIZE(pyb_i2c_obj)
            || pyb_i2c_obj[i2c_id - 1].i2c == NULL) {
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_ValueError,
                "I2C(%d) does not exist", i2c_id));
        }
    }

    // get I2C object
    const pyb_i2c_obj_t *i2c_obj = &pyb_i2c_obj[i2c_id - 1];

    if (n_args > 1 || n_kw > 0) {
        // start the peripheral
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
        pyb_i2c_init_helper(i2c_obj, n_args - 1, args + 1, &kw_args);
    }

    return (mp_obj_t)i2c_obj;
}

STATIC mp_obj_t pyb_i2c_init(mp_uint_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return pyb_i2c_init_helper(args[0], n_args - 1, args + 1, kw_args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pyb_i2c_init_obj, 1, pyb_i2c_init);

/// \method deinit()
/// Turn off the I2C bus.
STATIC mp_obj_t pyb_i2c_deinit(mp_obj_t self_in) {
    pyb_i2c_obj_t *self = self_in;
    i2c_deinit(self->i2c);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pyb_i2c_deinit_obj, pyb_i2c_deinit);

/// \method is_ready(addr)
/// Check if an I2C device responds to the given address.  Only valid when in master mode.
STATIC mp_obj_t pyb_i2c_is_ready(mp_obj_t self_in, mp_obj_t i2c_addr_o) {
    pyb_i2c_obj_t *self = self_in;

    if (!in_master_mode(self)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "I2C must be a master"));
    }

    mp_uint_t i2c_addr = mp_obj_get_int(i2c_addr_o) << 1;

    for (int i = 0; i < 10; i++) {
        HAL_StatusTypeDef status = HAL_I2C_IsDeviceReady(self->i2c, i2c_addr, 10, 200);
        if (status == HAL_OK) {
            return mp_const_true;
        }
    }

    return mp_const_false;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(pyb_i2c_is_ready_obj, pyb_i2c_is_ready);

/// \method scan()
/// Scan all I2C addresses from 0x01 to 0x7f and return a list of those that respond.
/// Only valid when in master mode.
STATIC mp_obj_t pyb_i2c_scan(mp_obj_t self_in) {
    pyb_i2c_obj_t *self = self_in;

    if (!in_master_mode(self)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "I2C must be a master"));
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);

    for (uint addr = 1; addr <= 127; addr++) {
        for (int i = 0; i < 10; i++) {
            HAL_StatusTypeDef status = HAL_I2C_IsDeviceReady(self->i2c, addr << 1, 10, 200);
            if (status == HAL_OK) {
                mp_obj_list_append(list, mp_obj_new_int(addr));
                break;
            }
        }
    }

    return list;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pyb_i2c_scan_obj, pyb_i2c_scan);

/// \method send(send, addr=0x00, timeout=5000)
/// Send data on the bus:
///
///   - `send` is the data to send (an integer to send, or a buffer object)
///   - `addr` is the address to send to (only required in master mode)
///   - `timeout` is the timeout in milliseconds to wait for the send
///
/// Return value: `None`.
STATIC mp_obj_t pyb_i2c_send(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_send,    MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_addr,    MP_ARG_INT, {.u_int = PYB_I2C_MASTER_ADDRESS} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 5000} },
    };

    // parse args
    pyb_i2c_obj_t *self = pos_args[0];
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // get the buffer to send from
    mp_buffer_info_t bufinfo;
    uint8_t data[1];
    pyb_buf_get_for_send(args[0].u_obj, &bufinfo, data);

    // if IRQs are enabled then we can use DMA
    DMA_HandleTypeDef tx_dma;
    if (query_irq() == IRQ_STATE_ENABLED) {
        dma_init(&tx_dma, self->tx_dma_stream, &dma_init_struct_spi_i2c, self->tx_dma_channel, DMA_MEMORY_TO_PERIPH, self->i2c);
        self->i2c->hdmatx = &tx_dma;
        self->i2c->hdmarx = NULL;
    }

    // send the data
    HAL_StatusTypeDef status;
    if (in_master_mode(self)) {
        if (args[1].u_int == PYB_I2C_MASTER_ADDRESS) {
            if (query_irq() == IRQ_STATE_ENABLED) {
                dma_deinit(&tx_dma);
            }
            nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "addr argument required"));
        }
        mp_uint_t i2c_addr = args[1].u_int << 1;
        if (query_irq() == IRQ_STATE_DISABLED) {
            status = HAL_I2C_Master_Transmit(self->i2c, i2c_addr, bufinfo.buf, bufinfo.len, args[2].u_int);
        } else {
            status = HAL_I2C_Master_Transmit_DMA(self->i2c, i2c_addr, bufinfo.buf, bufinfo.len);
        }
    } else {
        if (query_irq() == IRQ_STATE_DISABLED) {
            status = HAL_I2C_Slave_Transmit(self->i2c, bufinfo.buf, bufinfo.len, args[2].u_int);
        } else {
            status = HAL_I2C_Slave_Transmit_DMA(self->i2c, bufinfo.buf, bufinfo.len);
        }
    }

    // if we used DMA, wait for it to finish
    if (query_irq() == IRQ_STATE_ENABLED) {
        if (status == HAL_OK) {
            status = i2c_wait_dma_finished(self->i2c, args[2].u_int);
        }
        dma_deinit(&tx_dma);
    }

    if (status != HAL_OK) {
        mp_hal_raise(status);
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pyb_i2c_send_obj, 1, pyb_i2c_send);

/// \method recv(recv, addr=0x00, timeout=5000)
///
/// Receive data on the bus:
///
///   - `recv` can be an integer, which is the number of bytes to receive,
///     or a mutable buffer, which will be filled with received bytes
///   - `addr` is the address to receive from (only required in master mode)
///   - `timeout` is the timeout in milliseconds to wait for the receive
///
/// Return value: if `recv` is an integer then a new buffer of the bytes received,
/// otherwise the same buffer that was passed in to `recv`.
STATIC mp_obj_t pyb_i2c_recv(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_recv,    MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_addr,    MP_ARG_INT, {.u_int = PYB_I2C_MASTER_ADDRESS} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 5000} },
    };

    // parse args
    pyb_i2c_obj_t *self = pos_args[0];
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // get the buffer to receive into
    vstr_t vstr;
    mp_obj_t o_ret = pyb_buf_get_for_recv(args[0].u_obj, &vstr);

    // if IRQs are enabled then we can use DMA
    DMA_HandleTypeDef rx_dma;
    if (query_irq() == IRQ_STATE_ENABLED) {
        dma_init(&rx_dma, self->rx_dma_stream, &dma_init_struct_spi_i2c, self->rx_dma_channel, DMA_PERIPH_TO_MEMORY, self->i2c);
        self->i2c->hdmatx = NULL;
        self->i2c->hdmarx = &rx_dma;
    }

    // receive the data
    HAL_StatusTypeDef status;
    if (in_master_mode(self)) {
        if (args[1].u_int == PYB_I2C_MASTER_ADDRESS) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "addr argument required"));
        }
        mp_uint_t i2c_addr = args[1].u_int << 1;
        if (query_irq() == IRQ_STATE_DISABLED) {
            status = HAL_I2C_Master_Receive(self->i2c, i2c_addr, (uint8_t*)vstr.buf, vstr.len, args[2].u_int);
        } else {
            status = HAL_I2C_Master_Receive_DMA(self->i2c, i2c_addr, (uint8_t*)vstr.buf, vstr.len);
        }
    } else {
        if (query_irq() == IRQ_STATE_DISABLED) {
            status = HAL_I2C_Slave_Receive(self->i2c, (uint8_t*)vstr.buf, vstr.len, args[2].u_int);
        } else {
            status = HAL_I2C_Slave_Receive_DMA(self->i2c, (uint8_t*)vstr.buf, vstr.len);
        }
    }

    // if we used DMA, wait for it to finish
    if (query_irq() == IRQ_STATE_ENABLED) {
        if (status == HAL_OK) {
            status = i2c_wait_dma_finished(self->i2c, args[2].u_int);
        }
        dma_deinit(&rx_dma);
    }

    if (status != HAL_OK) {
        mp_hal_raise(status);
    }

    // return the received data
    if (o_ret != MP_OBJ_NULL) {
        return o_ret;
    } else {
        return mp_obj_new_str_from_vstr(&mp_type_bytes, &vstr);
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pyb_i2c_recv_obj, 1, pyb_i2c_recv);

/// \method mem_read(data, addr, memaddr, timeout=5000, addr_size=8)
///
/// Read from the memory of an I2C device:
///
///   - `data` can be an integer or a buffer to read into
///   - `addr` is the I2C device address
///   - `memaddr` is the memory location within the I2C device
///   - `timeout` is the timeout in milliseconds to wait for the read
///   - `addr_size` selects width of memaddr: 8 or 16 bits
///
/// Returns the read data.
/// This is only valid in master mode.
STATIC const mp_arg_t pyb_i2c_mem_read_allowed_args[] = {
    { MP_QSTR_data,    MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_addr,    MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_memaddr, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 5000} },
    { MP_QSTR_addr_size, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 8} },
};

STATIC mp_obj_t pyb_i2c_mem_read(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // parse args
    pyb_i2c_obj_t *self = pos_args[0];
    mp_arg_val_t args[MP_ARRAY_SIZE(pyb_i2c_mem_read_allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(pyb_i2c_mem_read_allowed_args), pyb_i2c_mem_read_allowed_args, args);

    if (!in_master_mode(self)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "I2C must be a master"));
    }

    // get the buffer to read into
    vstr_t vstr;
    mp_obj_t o_ret = pyb_buf_get_for_recv(args[0].u_obj, &vstr);

    // get the addresses
    mp_uint_t i2c_addr = args[1].u_int << 1;
    mp_uint_t mem_addr = args[2].u_int;
    // determine width of mem_addr; default is 8 bits, entering any other value gives 16 bit width
    mp_uint_t mem_addr_size = I2C_MEMADD_SIZE_8BIT;
    if (args[4].u_int != 8) {
        mem_addr_size = I2C_MEMADD_SIZE_16BIT;
    }

    HAL_StatusTypeDef status;
    if (query_irq() == IRQ_STATE_DISABLED) {
        status = HAL_I2C_Mem_Read(self->i2c, i2c_addr, mem_addr, mem_addr_size, (uint8_t*)vstr.buf, vstr.len, args[3].u_int);
    } else {
        DMA_HandleTypeDef rx_dma;
        dma_init(&rx_dma, self->rx_dma_stream, &dma_init_struct_spi_i2c, self->rx_dma_channel, DMA_PERIPH_TO_MEMORY, self->i2c);
        self->i2c->hdmatx = NULL;
        self->i2c->hdmarx = &rx_dma;
        status = HAL_I2C_Mem_Read_DMA(self->i2c, i2c_addr, mem_addr, mem_addr_size, (uint8_t*)vstr.buf, vstr.len);
        if (status == HAL_OK) {
            status = i2c_wait_dma_finished(self->i2c, args[3].u_int);
        }
        dma_deinit(&rx_dma);
    }

    if (status != HAL_OK) {
        mp_hal_raise(status);
    }

    // return the read data
    if (o_ret != MP_OBJ_NULL) {
        return o_ret;
    } else {
        return mp_obj_new_str_from_vstr(&mp_type_bytes, &vstr);
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pyb_i2c_mem_read_obj, 1, pyb_i2c_mem_read);

/// \method mem_write(data, addr, memaddr, timeout=5000, addr_size=8)
///
/// Write to the memory of an I2C device:
///
///   - `data` can be an integer or a buffer to write from
///   - `addr` is the I2C device address
///   - `memaddr` is the memory location within the I2C device
///   - `timeout` is the timeout in milliseconds to wait for the write
///   - `addr_size` selects width of memaddr: 8 or 16 bits
///
/// Returns `None`.
/// This is only valid in master mode.
STATIC mp_obj_t pyb_i2c_mem_write(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // parse args (same as mem_read)
    pyb_i2c_obj_t *self = pos_args[0];
    mp_arg_val_t args[MP_ARRAY_SIZE(pyb_i2c_mem_read_allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(pyb_i2c_mem_read_allowed_args), pyb_i2c_mem_read_allowed_args, args);

    if (!in_master_mode(self)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "I2C must be a master"));
    }

    // get the buffer to write from
    mp_buffer_info_t bufinfo;
    uint8_t data[1];
    pyb_buf_get_for_send(args[0].u_obj, &bufinfo, data);

    // get the addresses
    mp_uint_t i2c_addr = args[1].u_int << 1;
    mp_uint_t mem_addr = args[2].u_int;
    // determine width of mem_addr; default is 8 bits, entering any other value gives 16 bit width
    mp_uint_t mem_addr_size = I2C_MEMADD_SIZE_8BIT;
    if (args[4].u_int != 8) {
        mem_addr_size = I2C_MEMADD_SIZE_16BIT;
    }

    HAL_StatusTypeDef status;
    if (query_irq() == IRQ_STATE_DISABLED) {
        status = HAL_I2C_Mem_Write(self->i2c, i2c_addr, mem_addr, mem_addr_size, bufinfo.buf, bufinfo.len, args[3].u_int);
    } else {
        DMA_HandleTypeDef tx_dma;
        dma_init(&tx_dma, self->tx_dma_stream, &dma_init_struct_spi_i2c, self->tx_dma_channel, DMA_MEMORY_TO_PERIPH, self->i2c);
        self->i2c->hdmatx = &tx_dma;
        self->i2c->hdmarx = NULL;
        status = HAL_I2C_Mem_Write_DMA(self->i2c, i2c_addr, mem_addr, mem_addr_size, bufinfo.buf, bufinfo.len);
        if (status == HAL_OK) {
            status = i2c_wait_dma_finished(self->i2c, args[3].u_int);
        }
        dma_deinit(&tx_dma);
    }

    if (status != HAL_OK) {
        mp_hal_raise(status);
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pyb_i2c_mem_write_obj, 1, pyb_i2c_mem_write);

STATIC const mp_map_elem_t pyb_i2c_locals_dict_table[] = {
    // instance methods
    { MP_OBJ_NEW_QSTR(MP_QSTR_init), (mp_obj_t)&pyb_i2c_init_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_deinit), (mp_obj_t)&pyb_i2c_deinit_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_is_ready), (mp_obj_t)&pyb_i2c_is_ready_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_scan), (mp_obj_t)&pyb_i2c_scan_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_send), (mp_obj_t)&pyb_i2c_send_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_recv), (mp_obj_t)&pyb_i2c_recv_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_mem_read), (mp_obj_t)&pyb_i2c_mem_read_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_mem_write), (mp_obj_t)&pyb_i2c_mem_write_obj },

    // class constants
    /// \constant MASTER - for initialising the bus to master mode
    /// \constant SLAVE - for initialising the bus to slave mode
    { MP_OBJ_NEW_QSTR(MP_QSTR_MASTER),       MP_OBJ_NEW_SMALL_INT(PYB_I2C_MASTER) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_SLAVE),        MP_OBJ_NEW_SMALL_INT(PYB_I2C_SLAVE) },
};

STATIC MP_DEFINE_CONST_DICT(pyb_i2c_locals_dict, pyb_i2c_locals_dict_table);

const mp_obj_type_t pyb_i2c_type = {
    { &mp_type_type },
    .name = MP_QSTR_I2C,
    .print = pyb_i2c_print,
    .make_new = pyb_i2c_make_new,
    .locals_dict = (mp_obj_t)&pyb_i2c_locals_dict,
};

#endif // STM32F7

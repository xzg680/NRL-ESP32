#ifndef DRIVER_S31_I2C_H
#define DRIVER_S31_I2C_H

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_S31_KORVO

#include <driver/i2c_master.h>

bool S31_I2C_GetBus(i2c_master_bus_handle_t *out_bus);

#endif

#endif // DRIVER_S31_I2C_H

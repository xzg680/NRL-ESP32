#ifndef DRIVER_SERIAL_PORT_CONFIG_H
#define DRIVER_SERIAL_PORT_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool uart1_enabled;
    bool uart2_enabled;
    int uart1_rx_pin;
    int uart1_tx_pin;
    int uart2_rx_pin;
    int uart2_tx_pin;
    uint32_t uart2_baud;
    uint8_t uart2_data_bits;
    char uart2_parity;
    uint8_t uart2_stop_bits;
} SerialPortConfig;

void SERIAL_PORT_CONFIG_Init(void);
void SERIAL_PORT_CONFIG_Get(SerialPortConfig *out);
bool SERIAL_PORT_CONFIG_Set(const SerialPortConfig *config, bool persist);
bool SERIAL_PORT_CONFIG_Validate(const SerialPortConfig *config);
bool SERIAL_PORT_CONFIG_IsAllowedPin(int gpio);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_SERIAL_PORT_CONFIG_H

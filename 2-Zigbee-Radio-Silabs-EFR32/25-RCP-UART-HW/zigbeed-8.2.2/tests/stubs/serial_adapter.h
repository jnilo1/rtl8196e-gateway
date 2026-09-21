#ifndef TEST_SERIAL_ADAPTER_H
#define TEST_SERIAL_ADAPTER_H
#include <stdint.h>
#include "sl_zigbee_types.h"
typedef uint8_t SerialBaudRate;
typedef uint8_t SerialParity;
#define SERIAL_PORT_NAME_MAX_LEN 4096
extern char serialPort[SERIAL_PORT_NAME_MAX_LEN];
sl_status_t sli_legacy_serial_init(uint8_t, SerialBaudRate, SerialParity, uint8_t);
sl_status_t sli_legacy_serial_write_byte(uint8_t, uint8_t);
sl_status_t sli_legacy_serial_read_byte(uint8_t, uint8_t *);
uint16_t sli_legacy_serial_write_available(uint8_t);
sl_status_t sli_legacy_serial_write_string(uint8_t, const char *);
void sli_serial_adapter_tick_callback(void);
#endif

#ifndef MONITOR_DRIVER_H
#define MONITOR_DRIVER_H

#include <stdint.h> // We need this for uint8_t


// Function Prototypes (The "Public Interface")
void read_register(const uint8_t device_addr, const uint8_t reg, uint8_t buffer[INA_DATA_CHAR_LEN]);
int write_to_register(const uint8_t device_addr, const uint8_t reg, const uint8_t data[INA_DATA_CHAR_LEN]);

#endif
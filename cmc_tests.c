// HANDLE NEGATIVE VALUES WITH SHIFTING AND CASTING 

// CMC BOARD MIXES 8-bit,12-bit,13-bit and 16-bit DATA TYPES 

// THE EASY PART : 16-bit VALUES 
// READ TWO BYTES MSB , LSB

// uint8_t buffer[2];
// read_register(I2C_ADDR_CMC , CMC_REG_CURRENT_5V , buffer);

// COMBINE THEM SHIFTING
// uint16_t raw_unsigned = (buffer[0] << 8) | buffer[1];

// CASTING TO HANDLE NEGATIVES
// int16_t raw_signed = (int16_t) raw_unsigned;


// THE SECOND VALUES : 8-bit SIGNED 
// READ 1 BYTE 
// uint8_t temp_buffer[1];
// read_register_8bit(I2C_ADDR_CMC , CMC_REG_TEMP_SMPS , temp_buffer);
// 
// HANDLE NEGATIVES CAST 8-bit SIGNED
// int8_t raw_temp = (int8_t) temp_buffer[0];


// THE TRICKY PART : 12-bit & 13-bit
// READ 2 BYTES
// read_register(I2C_ADDR_CMC, CMC_REG_VOLT_3V3, buffer);

// 2. Combine
// uint16_t raw_value = (buffer[0] << 8) | buffer[1];

// 3. Mask (Optional but safe). 
// The manual implies it's a 13-bit value (D12 to D0).
// 0x1FFF keeps the bottom 13 bits.
// raw_value = raw_value & 0x1FFF; 

// 4. Conversion (LSB = 4mV)
// float voltage_mv = (float)raw_value * 4.0;




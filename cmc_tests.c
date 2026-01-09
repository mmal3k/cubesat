// Le déroulement de ce code
// D'abord il importe les fonctions necessaire afin de manipuler la CMC a partir de monitor_driver.h
// ensuite test la connection avec la CMC en i2C en utilisant le registre du Firmware car la CMC n'a pas de manifectureID comme la INA 260
// Si le Résulat dans le registre est different de 0x00 et 0xFF alors la connection est réussi
// Enfin on passe à la lecture des registres de Temperature et de Voltage pour enfin les printer sur notre terminal (celui du picocom XD )

#include <stdio.h>
#include <unistd.h>         // For sleep()
#include "monitor_driver.h" // Includes I2C_ADDR_CMC and register definitions

void monitor_cmc(void)
{
    uint8_t buffer[2];
    printf("--- Starting CMC I2C Connection Test ---\n");
    read_register_8bit(I2C_ADDR_CMC, CMC_REG_FIRMWARE_VER, buffer);
    uint8_t fw_raw = buffer[0];

    // Basic validity check: If we read 0x00 or 0xFF, the bus is likely floating or unconnected
    if (fw_raw == 0x00 || fw_raw == 0xFF)
    {
        printf("[Error] CMC not detected at address 0x%02X. (Read: 0x%02X)\n", I2C_ADDR_CMC, fw_raw);
        return;
    }

    // Decode BCD: Upper nibble = Major version, Lower nibble = Minor version
    uint8_t major = (fw_raw >> 4) & 0x0F;
    uint8_t minor = fw_raw & 0x0F;
    printf("[Success] CMC Detected! Firmware Version: %d.%d\n", major, minor);

    while (1)
    {
        // --- A. READ SMPS TEMPERATURE (Reg 0x2C) ---
        read_register_8bit(I2C_ADDR_CMC, CMC_REG_TEMP_SMPS, buffer);
        int8_t smps_temp = (int8_t)buffer[0];
        printf("SMPS Temp: %d C\n", smps_temp);

        // --- B. READ 3.3V VOLTAGE (Reg 0x30) ---
        read_register_16bit(I2C_ADDR_CMC, CMC_REG_VOLT_3V3, buffer);

        // Combine MSB (buffer[0]) and LSB (buffer[1])
        uint16_t volt_raw = (buffer[0] << 8) | buffer[1];

        // Formula: Value * 4mV
        float volt_real = (float)volt_raw * 0.004f;

        printf("3.3V Rail: %.3f V\n", volt_real);
        printf("----------------------------------\n");

        sleep(2);
    }
}

int main(void)
{
    printf("Test de la connection en I2C entre la Q7 et la CMC\n");
    monitor_cmc();
    return 0;
}
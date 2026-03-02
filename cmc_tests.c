// Le déroulement de ce code
// D'abord il importe les fonctions necessaire afin de manipuler la CMC a partir de monitor_driver.h
// ensuite test la connection avec la CMC en i2C en utilisant le registre du Firmware car la CMC n'a pas de manifectureID comme la INA 260
// Si le Résulat dans le registre est different de 0x00 et 0xFF alors la connection est réussi
// Enfin on passe à la lecture des registres de Temperature et de Voltage pour enfin les printer sur notre terminal (celui du picocom XD )

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include "monitor_driver.h"

// HOW HANDLE NEGATIVE VALUES WITH SHIFTING AND CASTING 
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


void monitor_cmc(void) {
    
    uint8_t buffer[2];
    printf("--- Starting CMC I2C Connection Test ---\n");
    read_register_8bit(I2C_ADDR_CMC, CMC_REG_FIRMWARE_VER, buffer);
    uint8_t fw_raw = buffer[0];

    // BASIC VALIDITY CHECK : IF WE READ 0x00 OR 0xFF , THE BUS IS LIKELY FLOATING OR UNCONNECTED 
    if (fw_raw == 0x00 || fw_raw == 0xFF) {
        printf("[Error] CMC not detected at address 0x%02X. (Read: 0x%02X)\n", I2C_ADDR_CMC, fw_raw);
        return;
    }

    // DECODE BCD : UPPER NIBBLE = MAJOR VERSION , LOWER NIBBLE = MINOR VERSION 
    uint8_t major = (fw_raw >> 4) & 0x0F;
    uint8_t minor = fw_raw & 0x0F;
    printf("[Success] CMC Detected! Firmware Version: %d.%d\n", major, minor);

    while (1) {
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
        
        uint8_t data_buf[2];

        data_buf[0] = 0x00;
        data_buf[1] = 0xf0;

        write_block(I2C_ADDR_CMC,CMC_REG_TX_FREQ_OFFSET,data_buf,2);

        cmc_transmit_data(data_buf , 2);
        
        sleep(5);
        
    }
}

void run_cmc_diagnostics(){
    uint8_t buffer[2];
    uint8_t status_lock = 0;
    uint16_t fwd_power = 0;
    uint16_t tx_offset = 0;

    while(1){
        read_register_8bit(I2C_ADDR_CMC , CMC_REG_FREQ_LOCK , &status_lock);

        read_register_16bit(I2C_ADDR_CMC , CMC_REG_FREQ_LOCK , buffer);
        fwd_power = (buffer[0] << 8) | buffer[1];

        read_register_16bit(I2C_ADDR_CMC , CMC_REG_TX_FREQ_OFFSET , buffer);
        tx_offset = (buffer[0] << 8) | buffer[1];


        printf("[DIAG] Lock : %d | TX Offset : %d | Power : %d\n" , status_lock ,fwd_power , tx_offset);

        sleep(5);
    }
}


void enable_beacon(void) {
    uint8_t data ; 

    data = 0x01;
    write_block(I2C_ADDR_CMC , CMC_REG_I2C_TIMEOUT , &data , 1);


    data = 10;
    write_block(I2C_ADDR_CMC , CMC_REG_REC_TIMEOUT , &data , 1);


    data = 0x01;
    write_block(I2C_ADDR_CMC , CMC_REG_BEACON_CTRL , &data , 1);

    printf("succeeded");

}

void force_transmission_test(){
    uint8_t payload[] = "HELLO WORLD";
    uint8_t buffer[2]; // For reading diagnostics

    printf("--- CONFIGURING RADIO ---\n");

    // 1. Set Frequency Offset (256)
    uint8_t data[2];
    data[0]= 0x01; 
    data[1]= 0x00; 
    write_block(I2C_ADDR_CMC, CMC_REG_TX_FREQ_OFFSET, data, 2);

    // 2. Set Modem Config
    uint8_t mod_config = 0x01; 
    write_block(I2C_ADDR_CMC, CMC_REG_MODEM_CONFIG, &mod_config, 1);

    // 3. Set Power (0x00 = On, 0x03 = Inhibit/Off)
    uint8_t pwr = 0x00; 
    write_block(I2C_ADDR_CMC, CMC_REG_PA_POWER, &pwr, 1);

    printf("--- STARTING LOOP ---\n");

    while(1){
        // A. READ DIAGNOSTICS BEFORE SENDING
        uint8_t lock_reg = 0;
        read_register_8bit(I2C_ADDR_CMC, CMC_REG_FREQ_LOCK, &lock_reg);
        
        // Extract the TX Lock bit (usually Bit 1)
        int is_locked = (lock_reg >> 1) & 0x01; 

        // B. READ CURRENT DRAW (To see if PA is on)
        // (Assuming you have a function or register for this, optional but helpful)
        
        printf("[STATUS] TX Lock: %d | Sending Packet... ", is_locked);
        
        if (is_locked == 0) {
            printf("(WARNING: Radio not locked!)\n");
        }

        // C. TRANSMIT
        cmc_transmit_data(payload, 11);
        printf("Done\n");

        sleep(1);
    }
}
void test_voltage_under_load() {
    uint8_t buf[2];
    float v33, c33;
    uint8_t min_pwr = 0x00; // On force la puissance au minimum (à ajuster selon datasheet)

    printf("\n--- FINAL DIAGNOSTIC TEST (LOW POWER MODE) ---\n");

    while(1) {
        // 1. Lire les capteurs
        // read_register_16bit(I2C_ADDR_CMC, CMC_REG_VOLT_3V3, buf);
        // v33 = ((buf[0] << 8) | buf[1]) * 0.004f;

        // read_register_16bit(I2C_ADDR_CMC, CMC_REG_CURRENT_3V3, buf);
        // c33 = (((buf[0] << 8) | buf[1]) * 0.001f) / 100.0f; 

        // printf("3.3V Rail: %.3f V | Current: %.1f mA\n", v33, c33 * 1000.0f);

        // // 2. FORCER LA PUISSANCE BASSE AVANT L'ENVOI
        // // On écrit la puissance minimale pour essayer d'éviter le crash
        // write_block(I2C_ADDR_CMC, CMC_REG_PA_POWER, &min_pwr, 1);

        // printf("Tentative d'envoi (Puissance)...\n");
        // uint8_t dummy[] = "TEST";
        // cmc_transmit_data(dummy, 4);
        
        read_register_16bit(I2C_ADDR_CMC, CMC_REG_VOLT_3V3, buf);
        v33 = ((buf[0] << 8) | buf[1]) * 0.004f;
        printf("3.3V Rail: %.3f V \n", v33);
        printf("------------------\n");


        // 3. Pause longue pour voir si on survit au reset
        sleep(2); 
    }
}


int main(void) {
    test_voltage_under_load();
    
    return 0;
}



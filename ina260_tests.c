/**
 * @Author: Maxime Feingesicht
 * @Date:   2023-03-24T16:40:00+01:00
 * @Email:  maxime.feingesicht@lisa.ipsl.fr
 * @Project: IR-COASTER
 * @Filename: ina228_tests.c
 * @Last modified by:   ir-coaster-soft
 * @Last modified time: 2023-04-03T18:00:00+01:00
 * @Copyright: 2021 CNRS
 */

#include <stdbool.h>
#include "time.h"

#include "monitor_driver.h"

////////////////////////////////////////////////////////////////////////
//////////////////////////ina260 registers//////////////////////////////
////////////////////////////////////////////////////////////////////////


// I2C addresses for every single sensor connected to my system 

// For our case we will need the q7 ( i need to change the @ of the q7 later from 0x47 to the q7 @ !! )
#define I2C_ADDR_Q7             0x41

typedef enum {
    ///Configures Averaging, conversion times and operation mode (see datasheet page 22)
    I2C_REG_CONFIG          =0x00,
    /// Contains the current value (direct mA/A reading on INA260)
    I2C_REG_CURRENT         =0x01,
    /// Value of the bus voltage
    I2C_REG_BUS_VOLTAGE     =0x02,
    /// Value of the power
    I2C_REG_TEMPCO          =0x03,
    /// Mask/Enable register (for alerts)
    I2C_REG_MASK_ENABLE     = 0x06,
    /// Alert Limit register
    I2C_REG_ALERT_LIMIT     = 0x07,
    /// Value of the manufacturer ID, should be 0x5449
    I2C_REG_MANID           = 0xFE,
    /// Value of the device ID, should return 0x2270
    I2C_REG_DEVID           = 0xFF
} ina_260_register_t;


////////////////////////////////////////////////////////////////////////
//////////////////////////main functions////////////////////////////////
////////////////////////////////////////////////////////////////////////

// INA260 produces 16-bit data 
// We have two options to handle negative numbers 

// Option A : simply cast 16-bit value  
// When you read the register:
// uint16_t raw_value = (buffer[0] << 8) | buffer[1];
// int16_t final_value = (int16_t)raw_value; // This automatically handles the negative sign!

// Option B : use this function 
int16_t complem_two_16_bits(uint16_t raw_value){
    int16_t complem_value = raw_value;

    //printf("conv : %x\n", raw_value);
    //printf("first bit : %x\n", (raw_value & 0x0008000));

    if((raw_value & 0x0008000) == 0x0008000){
    complem_value = raw_value | 0xFFFF0000;
    }
    return complem_value;
}

//~ Function : monitor_q7 
//~ ----------------------------

void monitor_q7(void) {
    uint8_t buffer[2];
    int16_t raw_value;
    float final_value; 

    // 1.Verify the connection (Optional but good practice)
    // we need to call the read register function here !!!
    // missing code !!!
    read_register(I2C_ADDR_Q7 , I2C_REG_MANID , buffer);
    uint16_t man_id= (buffer[0] << 8) | buffer[1];

    if(man_id != 0x5449){
        printf("Error : Could not find INA260 at adress 0x%x" ,I2C_ADDR_Q7);
        return ;
    }

    printf("INA260 Connected \n");
    while (1)
    {
        // READ BUS VOLTAGE Reg 0x02
        // !!! Missing code read register 
        read_register(I2C_ADDR_Q7 , I2C_REG_BUS_VOLTAGE , buffer);
        raw_value = (buffer[0] << 8) | buffer[1];
        // INA260 Voltage LSB is fixed at 1.25 mV
        final_value = (float) raw_value * 1.25;
        printf("Voltage %.2f mV\n" , final_value);


        // READ CURRENT 0x01
        // !!! Missing code read register 
        raw_value = (buffer[0] << 8) | buffer[1];
        // INA260 Voltage LSB is fixed at 1.25 mA
        final_value = (float) raw_value * 1.25;
        printf("Current %.2f mA\n" , final_value);
        

        // READ POWER 0x03
        // !!! Missing code read register 
        raw_value = (buffer[0] << 8) | buffer[1];
        // INA260 Voltage LSB is fixed at 10 mW
        final_value = (float) raw_value * 10.0; 
        printf("Power %.2f mW\n" , final_value);

        // wait 1 second before next reading 
        sleep(1);
    }
    

}

int main (void){        
    printf("test");
    monitor_q7();
    return 0;
}

#include <stdio.h>          // For printf
#include <fcntl.h>          // For open() and O_RDWR
#include <sys/ioctl.h>      // For ioctl()
#include <linux/i2c-dev.h>  // For I2C_SLAVE constant
#include <errno.h>          // For error number reporting
#include <string.h>         // For strerror() (turns error numbers into text)
#include <unistd.h>         // For close()
#include <stdint.h>         // For uint8_t

// Check your Q7 manual: It is usually "/dev/i2c-1" or "/dev/i2c-0"
#define I2C_BUS_FILE          "/dev/i2c-1"
#define INA_DATA_CHAR_LEN     2
#define INA_REG_CHAR_LEN      1

// FUNCTION : open_i2c 
// -------------------
// OPENS THE I2C BUS AND ASSOCIATES THE FILE DESCRIPTOR WITH THE GIVEN ADDRESS
// INPUT : 
//          - uint8_t addr : the I2C address of the sensor 
//          - int *file_i2c_p : A pointer to store the FILE DESCRIPTOR
// 
// OUTPUT : 
// void (the result is stores in the *file_i2c_p)

static void open_i2c(uint8_t addr, int *file_i2c_p){

    // 1. OPEN THE I2C BUS FILE 
    if ((*file_i2c_p = open(I2C_BUS_FILE, O_RDWR)) < 0) {
        //ERROR HANDLING: you can check errno to see what went wrong
        printf("[error] Failed to open the i2c bus : %s\n", strerror(errno));
        return;
    }

    // 2. CONFIGURE THE DRIVER (IOCTL)
    // I2C_SLAVE = Tell the driver we want to talk to a specific slave address
    if (ioctl(*file_i2c_p, I2C_SLAVE, addr) < 0) {
        printf("Failed to acquire bus access and/or talk to slave 0x$s\n", addr , strerror(errno)); 
        //ERROR HANDLING; you can check errno to see what went wrong
        close(*file_i2c_p);
        *file_i2c_p = -1;
        return;
    }
}

//~ Function : close_i2c
//~ ----------------------------
//~ Close the i2c bus
//~
//~ input : bool verbose is true if verbose mode, falise otherwise
//~
//~ output : void
static void close_i2c(int *file_i2c_p){
  if (close(*file_i2c_p) < 0) {
    //ERROR HANDLING: you can check errno to see what went wrong
    printf("[error] Failed to close the i2c bus\n");
    return;
  }
}

static void read_i2c(uint8_t *buffer, int length, int file_i2c){
  //----- READ BYTES -----
  if (read(file_i2c, buffer, length) != length) {
    //ERROR HANDLING: i2c transaction failed
    printf("Failed to read from the i2c bus, returned %s\n", strerror(errno));
  }
}

static void write_i2c(uint8_t *data, int length, int file_i2c){
  //----- WRITE BYTES -----
  //if (write(file_i2c, data, length) != length)
  int l;
  l = write(file_i2c, data, length);
  if (l != length) {
    //ERROR HANDLING: i2c transaction failed
    fprintf(stderr, "Failed to write to the i2c bus, returned %s\n", strerror(errno));
  }
}

static int write_i2c_with_error_code(uint8_t *data, int length, int file_i2c){
  //----- WRITE BYTES -----
  int l;
  l = write(file_i2c, data, length);
  if (l != length) {
    //ERROR HANDLING: i2c transaction failed
    fprintf(stderr, "Failed to write to the i2c bus, returned %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

////////////////////////////////////////////////////////////////////////
//////////////////////////public functions//////////////////////////////
////////////////////////////////////////////////////////////////////////

//~ Function : read_register
//~ ----------------------------
//~ Reads the requested register of the i2c device
//~
//~ input : uint8_t device_addr: adress of the device to read from,
//~   uint8_t reg: register to read from, uint8_t buffer: returned data
//~
//~ output : void
void read_register (
    const uint8_t device_addr ,
    const uint8_t reg ,
    uint8_t buffer[INA_DATA_CHAR_LEN]
  ){
    int file_i2c;
    uint8_t writing_data = reg;

    open_i2c(device_addr, &file_i2c);

    //write to the i2c register pointer set (see datasheet)
    write_i2c(&writing_data, INA_REG_CHAR_LEN, file_i2c);

    //read from the register
    read_i2c(buffer, INA_DATA_CHAR_LEN, file_i2c);

    //Close i2c
    close_i2c(&file_i2c);
    #ifdef DEBUG_CM
      //printf("Returned %s\n", buffer);
    #endif
}

//~ Function : write_to_register
//~ ----------------------------
//~ Writes 16 bits to a specific register
//~
//~ returns 1 for success , 0 for failure 
int write_to_register(
  const uint8_t device_addr, 
  const uint8_t reg,
  const uint8_t data[INA_DATA_CHAR_LEN]
){
  // WE NEED 3 BYTES IN TOTAL (1 REG ADDRESS , 2 BYTES FOR THE DATA)
  uint8_t writing_data[INA_DATA_CHAR_LEN + 1];
  int file_i2c;

  open_i2c(device_addr, &file_i2c);

  if(file_i2c < 0) {
    return 0;
  }

  // Prepare the package : [Address , MSB , LSB]
  writing_data[0] = reg;
  writing_data[1] = data[0];
  writing_data[2] = data[1];

  int ret = write_i2c_with_error_code(writing_data, INA_DATA_CHAR_LEN + 1, file_i2c);

  close_i2c(&file_i2c);

  return ret;
}

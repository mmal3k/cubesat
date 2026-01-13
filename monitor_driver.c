#include <stdio.h>          // For printf
#include <fcntl.h>          // For open() and O_RDWR
#include <sys/ioctl.h>      // For ioctl()
#include <linux/i2c-dev.h>  // For I2C_SLAVE constant
#include <errno.h>          // For error number reporting
#include <string.h>         // For strerror() (turns error numbers into text)
#include <unistd.h>         // For close()
#include <stdint.h>         // For uint8_t
#include <stdlib.h>         // For malloc

#include "monitor_driver.h"
#define I2C_DEVICE_FILE     "/dev/i2c-1"

/**
 * Opens the I2C bus and connects to a specific slave address.
 * @param device_addr: The hex address of the sensor (0x25 for CMC)
 * @param file_descriptor: A pointer to store the "ID" of the open file
 */
void  open_i2c(uint8_t device_addr , int *file_descriptor){
  if (file_descriptor == NULL){
    fprintf(stderr,"File descriptor is Null\n");
    abort();
  }
  char filename[20];
  sprintf(filename , "%s" , I2C_DEVICE_FILE) ;
  
  // OPEN THE FILE (READ/WRITE MODE)
  *file_descriptor = open(filename , O_RDWR);

  if (*file_descriptor < 0) {
    printf("[Error] Failed to open the I2C bus%s\n" , filename);
    exit(1);
  }

  // CONFIGURE THE FILE 
  // the <linux/i2c-dev.h> provide the I2C_SLAVE
  
  if(ioctl(*file_descriptor , I2C_SLAVE , device_addr) < 0) {
    printf("[Error] Failed to acquire bus access and/or talk to slave.\n");
    exit(1);
  }
}

void close_i2c(int *file_descriptor){
  if(*file_descriptor >= 0) {
    close(*file_descriptor) ;
    *file_descriptor = -1; // RESET IT SO WE KNOW ITS CLOSED
  }
}


// HELPER : WRITES N BYTES TO THE BUS
void write_i2c_with_error_code(uint8_t *data , int length , int file_descriptor) {
  if(write(file_descriptor , data , length) != length){
    printf("[Error] Failed to write to the I2C Bus .(Errno : %s)\n" , strerror(errno));
  }
}

// HELPER : READS N BYTES TO THE BUS
void read_i2c(uint8_t *buffer , int length , int file_descriptor) {
  if(read(file_descriptor , buffer , length) != length){
    printf("[Error] Failed to read to the I2C Bus .(Errno : %s)\n" , strerror(errno));
  }
}

/**
 * Reads a 16-bit register (2 bytes).
 * Steps: 
 * 1. Write the Register Address (1 byte)
 * 2. Read the Data (2 bytes)
 */
void read_register_16bit(uint8_t device_addr , uint8_t reg_addr , uint8_t *data_buffer) {
  int file = -1 ;

  // SETUP 
  open_i2c(device_addr , &file);

  // TELL THE CHIP WHICH REGISTER WE WANT
  write_i2c_with_error_code(&reg_addr , 1 , file);

  // READ THE ANSWER (2 BYTES)
  read_i2c(data_buffer , 2 , file);

  // CLEANUP
  close_i2c(&file);
}

void read_register_8bit(uint8_t device_addr , uint8_t reg_addr , uint8_t *data_buffer) {
  int file = -1;

  // SETUP 
  open_i2c(device_addr , &file);
  
  // TELL THE CHIP WHICH REGISTER WE WANT
  write_i2c_with_error_code(&reg_addr , 1 , file);

  // READ THE ANSWER (2 BYTES)
  read_i2c(data_buffer , 1 , file);

  // // CLEANUP
  close_i2c(&file);
}

void write_block(uint8_t device_addr , uint8_t reg_addr , uint8_t *data , int length) {
  int *file;
  
  // BUFFER THAT HOLDS [REG_ADDR] + [DATA] 
  // I2C SENDS REGISTER ADDR FIRST !
  uint8_t *buffer = (uint8_t) malloc(length + 1); 

  if(buffer == NULL) {
    printf("[Fatal] Memory allocation failed\n");
    exit(1);
  }

  // PREPARE THE PACKAGE
  buffer[0] = reg_addr ;
  memcpy(&buffer[1], data , length); 

  // SEND THE DATA
  open_i2c(device_addr , file) ;
  write_i2c_with_error_code(buffer, length + 1, file);
  close_i2c(&file);

  // RELEASE THE MEMORY 
  free(buffer);
}


/**
 * Implements the CMC "Simple Protocol" (Manual Page 15).
 * Wraps your data with Header, Length, and Checksum.
 */
void cmc_transmit_data(uint8_t *user_payload, uint8_t payload_len) {
    // 1. Calculate the TOTAL size of the Simple Protocol frame
    // Frame = [1A] [CF] [Len] [Payload...] [Checksum]
    // Total = 2 + 1 + payload_len + 1 = payload_len + 4
    int total_frame_len = payload_len + 4;
    
    // Allocate memory for the envelope
    uint8_t *frame = (uint8_t*)malloc(total_frame_len);
    
    // --- BUILD THE HEADER ---
    frame[0] = 0x1A;            // Preamble Byte 1 (Fixed)
    frame[1] = 0xCF;            // Preamble Byte 2 (Fixed)
    
    // --- BUILD THE LENGTH ---
    // Rule: "0 means 1 byte". So we subtract 1.
    frame[2] = payload_len - 1; 

    // --- COPY THE PAYLOAD ---
    // Copy your custom protocol data into the frame
    memcpy(&frame[3], user_payload, payload_len);
    
    // --- BUILD THE CHECKSUM ---
    // Rule: Sum of payload bytes (ignoring overflow)
    uint8_t checksum = 0;
    for (int i = 0; i < payload_len; i++) {
        checksum += user_payload[i];
    }
    frame[total_frame_len - 1] = checksum; // Put at the very end

    // --- SEND TO I2C DRIVER ---
    write_block(I2C_ADDR_CMC, CMC_REG_TX_DATA, frame, total_frame_len);

    // Clean up
    free(frame);
}
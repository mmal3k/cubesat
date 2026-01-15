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

#define MAX_PAYLOAD_SIZE    (MAX_PACKET_SIZE - HEADER_SIZE)


static uint16_t sequence_id = 0 ;

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
  int file;
  
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

  // send the data
  open_i2c(device_addr , &file) ;
  write_i2c_with_error_code(buffer, length + 1, file);
  close_i2c(&file);

  // release the memory 
  free(buffer);
}

static uint16_t calculate_crc16(uint8_t *data , int len){
  uint16_t crc = 0;
  for(int i=0 ; i < len ; i++){
    crc += data[i];

  }
  return crc ;
}


void cmc_modem_init(){
  int timeout = 100;
  uint8_t buffer = 0;
  printf("[Init] Configuring CMC Modem...\n");

  // --- STEP 1: Configure Modem ---
  // "Write to Modem Config (0x00) and set Uplink/Downlink to 1200bps/9600bps"
 
  uint8_t config_val = 0x01; 
  write_block(I2C_ADDR_CMC, CMC_REG_MODEM_CONFIG, &config_val, 1);

  while(timeout > 0){
    // --- STEP 2: Read Ready Signal Register---
    // CHECK IF TR(THE FIRST BYTE) = 1
    read_register_8bit(I2C_ADDR_CMC, CMC_REG_READY_SIG, &buffer);

    if((buffer & 0x01) == 0x01){
      printf("[Init] Modem Ready! (TR=1)\n");
      return; // Success
    }

    printf("[Init] Waiting for TR... (Status: 0x%02X)\n", buffer);
    timeout--;
    delay_ms(50);
  }
  printf("[Init] Error: Modem Timed Out\n");
}
/**
 * implements the cmc "simple protocol" (manual page 15).
 * wraps your data with header, length, and checksum.
 */
void cmc_transmit_data(uint8_t *user_payload, uint8_t payload_len) {
    // 1. Calculate the TOTAL size of the Simple Protocol frame
    // Frame = [1A] [CF] [Len] [Payload...] [Checksum]
    // Total = 2 + 1 + payload_len + 1 = payload_len + 4

    cmc_modem_init();


    prepare_transmit();
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


void send_data (PacketType type , uint16_t payload_length , uint8_t *data){
  uint8_t packet[MAX_PACKET_SIZE];

  uint16_t frag_total =  (payload_length + MAX_PAYLOAD_SIZE - 1)/ MAX_PAYLOAD_SIZE;

  packet[IDX_TYPE]    = (uint8_t) type ; 

  packet[IDX_FRAG_TOT_LSB] =(frag_total>> 8) & 0xFF;
  packet[IDX_FRAG_TOT_MSB] = (frag_total) & 0xFF;

  uint32_t offset = 0 ;

  for(uint16_t frag_id = 0 ; frag_id <= frag_total ; frag_id++) {
    // a. determine size for this specific packet
    // Normally 245, but the last one might be smaller (e.g., 5 bytes)
    uint16_t current_chunk_size = MAX_PAYLOAD_SIZE;
    
    if ((payload_length - offset) < MAX_PAYLOAD_SIZE) {
        current_chunk_size = (uint16_t)(payload_length- offset);
    }

    packet[IDX_SEQ_LSB] = (sequence_id >> 8) & 0xFF;
    packet[IDX_SEQ_MSB] = sequence_id & 0xFF ;

    packet[IDX_FRAG_ID_LSB] =(frag_id>> 8) & 0xFF;
    packet[IDX_FRAG_ID_MSB] = (frag_id) & 0xFF;
        
    // Payload Length (Size of THIS chunk, NOT total file)
    packet[IDX_LEN_MSB] = (current_chunk_size >> 8) & 0xFF;
    packet[IDX_LEN_LSB] = (current_chunk_size) & 0xFF;

    // C. COPY DATA PAYLOAD
    // We copy from data[offset] to packet[11]
    if (current_chunk_size > 0 && data != NULL) {
        memcpy(&packet[HEADER_SIZE], &data[offset], current_chunk_size);
    } 
    
    // D. ZERO OUT CRC (Crucial Step)
    packet[IDX_CRC_MSB] = 0x00;
    packet[IDX_CRC_LSB] = 0x00;
    
    uint16_t total_packet_len = HEADER_SIZE + current_chunk_size;
    uint16_t my_crc = calculate_crc16(packet, total_packet_len);

    packet[IDX_CRC_MSB] = (my_crc >> 8) & 0xFF;
    packet[IDX_CRC_LSB] = (my_crc) & 0xFF;

    // G. TRANSMIT
    // Call your external hardware driver here
    cmc_transmit_data(packet, total_packet_len);

    // H. UPDATE STATE
    offset += current_chunk_size;
    sequence_id++; // Increment global counter for next packet 
  }
}

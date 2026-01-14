#ifndef MONITOR_DRIVER_H
#define MONITOR_DRIVER_H

#include <stdint.h> // We need this for uint8_t

#define I2C_ADDR_CMC 0x27

typedef enum {
    // --- Configuration Registers ---
    CMC_REG_MODEM_CONFIG    = 0x00, // 8-bit: Uplink/Downlink modulation [cite: 4844]
    CMC_REG_AX25_TXDELAY    = 0x01, // 8-bit: TxDelay (1-255) [cite: 4847]
    CMC_REG_SYNC_BYTES      = 0x02, // 8-bit: Sync byte value (0x55 default) [cite: 4861]
    CMC_REG_TX_DATA         = 0x03, // 8-bit: Write data here to transmit [cite: 4865]
    
    // --- Beacon Registers ---
    CMC_REG_BEACON_CTRL     = 0x04, // 8-bit: Enable/Clear beacon [cite: 4868]
    CMC_REG_BEACON_DATA     = 0x05, // 8-bit: Custom beacon data (max 128 bytes) [cite: 4872]
    
    // --- RF & Power Configuration ---
    CMC_REG_PA_POWER        = 0x06, // 8-bit: Set Power (0.5W, 1W, 2W) [cite: 4876]
    CMC_REG_RX_FREQ_OFFSET  = 0x07, // 16-bit (Start at 0x07): Rx Frequency [cite: 4882]
    CMC_REG_TX_FREQ_OFFSET  = 0x09, // 16-bit (Start at 0x09): Tx Frequency [cite: 4902]
    
    // --- Timeouts & Debug ---
    CMC_REG_I2C_TIMEOUT     = 0x0B, // 8-bit: Initial Timeout (minutes) [cite: 4912]
    CMC_REG_REC_TIMEOUT     = 0x0C, // 8-bit: Recurring Timeout (seconds) [cite: 4916]
    CMC_REG_DEBUG           = 0x0D, // 8-bit: Control LEDs [cite: 4927]
    CMC_REG_RESET           = 0x0E, // 8-bit: Soft Reset (Write 0 to nRST) [cite: 4937]
    CMC_REG_TRANSPARENT     = 0x10, // 8-bit: Transparent Mode Config [cite: 4941]
    CMC_REG_ALMOST_EMPTY    = 0x11, // 16-bit (Start at 0x11): Tx Buffer Threshold [cite: 4949]

    // --- Status & Info ---
    CMC_REG_FIRMWARE_VER    = 0x19, // 8-bit: BCD encoded version [cite: 4953]
    CMC_REG_READY_SIG       = 0x1A, // 8-bit: TR (Tx Ready) and RR (Rx Ready) flags [cite: 4958]
    CMC_REG_RX_BUF_COUNT    = 0x1B, // 16-bit (Start at 0x1B): Bytes available to read [cite: 4976]
    CMC_REG_RX_DATA         = 0x1D, // 8-bit: Read received data here [cite: 4984]
    CMC_REG_TX_FREE_SLOTS   = 0x1E, // 16-bit (Start at 0x1E): Free Tx buffer space [cite: 4991]

    // --- Counters (Diagnostics) ---
    CMC_REG_RX_CRC_FAIL     = 0x21, // 16-bit (Start at 0x21): CRC Fail Counter [cite: 4998]
    CMC_REG_RX_PKT_COUNT    = 0x23, // 16-bit (Start at 0x23): Successful Packet Counter [cite: 5004]
    CMC_REG_RX_FAIL_FULL    = 0x25, // 8-bit: Dropped due to full buffer [cite: 5015]
    CMC_REG_TX_OVERRUN      = 0x26, // 16-bit (Start at 0x26): Tx Overrun Counter [cite: 5024]
    
    // --- Hardware Status ---
    CMC_REG_FREQ_LOCK       = 0x28, // 8-bit: Tx/Rx Lock Status [cite: 5030]
    CMC_REG_DTMF            = 0x29, // 8-bit: Last received DTMF tone [cite: 5041]
    
    // --- Analog Telemetry (All 16-bit / 2 Bytes) ---
    CMC_REG_RSSI            = 0x2A, // 16-bit: Received Signal Strength [cite: 5049]
    CMC_REG_TEMP_SMPS       = 0x2C, // 8-bit: SMPS Temperature (Note: Manual says 8-bit) [cite: 5067]
    CMC_REG_TEMP_PA         = 0x2D, // 8-bit: PA Temperature (Note: Manual says 8-bit) [cite: 5075]
    CMC_REG_CURRENT_3V3     = 0x2E, // 16-bit: 3.3V Current [cite: 5081]
    CMC_REG_VOLT_3V3        = 0x30, // 16-bit: 3.3V Voltage [cite: 5096]
    CMC_REG_CURRENT_5V      = 0x32, // 16-bit: 5V Current [cite: 5102]
    CMC_REG_VOLT_5V         = 0x34, // 16-bit: 5V Voltage [cite: 5108]
    CMC_REG_PA_FWD_PWR      = 0x36, // 16-bit: Forward Power [cite: 5113]
    CMC_REG_PA_REV_PWR      = 0x38  // 16-bit: Reverse Power [cite: 5131]

} cmc_register_t;

// HEADER CONFIGURATION
#define HEADER_SIZE         11
#define MAX_PACKET_SIZE     256 

//--- BYTE OFFSETS ---
#define IDX_TYPE         0   // Packet Type (PID)
#define IDX_SEQ_MSB      1   // Sequence ID High Byte
#define IDX_SEQ_LSB      2   // Sequence ID Low Byte
#define IDX_FRAG_ID_MSB  3   // Fragment ID High
#define IDX_FRAG_ID_LSB  4   // Fragment ID Low
#define IDX_FRAG_TOT_MSB 5   // Total Fragments High
#define IDX_FRAG_TOT_LSB 6   // Total Fragments Low
#define IDX_LEN_MSB      7   // Payload Length High
#define IDX_LEN_LSB      8   // Payload Length Low
#define IDX_CRC_MSB      9   // CRC High (Placed BEFORE Data)
#define IDX_CRC_LSB      10  // CRC Low  (Placed BEFORE Data)
#define IDX_DATA_START   11  // Data starts here

/**
 * Reads a single byte (8-bit) from a specific register.
 * Usage: Reading CMC Temperature (Reg 0x0F)
 */
void read_register_8bit(uint8_t device_addr, uint8_t reg_addr, uint8_t *data_buffer);

/**
 * Reads two bytes (16-bit) from a specific register.
 * Usage: Reading Voltage/Current (if you add INA260 later)
 */
void read_register_16bit(uint8_t device_addr, uint8_t reg_addr, uint8_t *data_buffer);

/**
 * Writes a raw block of data to a specific register.
 * Usage: Internal helper for sending packets.
 * Note: Does NOT add the CMC Protocol headers (1A CF...).
 */
void write_block(uint8_t device_addr, uint8_t reg_addr, uint8_t *data, int length);

/**
 * Implements the CMC "Simple Protocol" Wrapper.
 * 1. Adds Preamble (0x1A 0xCF)
 * 2. Adds Length Byte
 * 3. Adds Your Payload
 * 4. Adds Checksum
 * 5. Sends everything using write_block()
 */
void cmc_transmit_data(uint8_t *user_payload, uint8_t payload_len);

#endif 
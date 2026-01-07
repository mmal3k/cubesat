# Makefile for INA260 Sensor Test
# The Q7 SDK environment automatically defines the variables: CC, CFLAGS, LDFLAGS

# 1. Name of the output file (the executable)
TARGET = sensor_test_app

# 2. List of source files
SRCS = ina260_tests.c monitor_driver.c

# 3. Create object files (.o) from source files (.c)
OBJS = $(SRCS:.c=.o)

# --- Rules ---

# Default rule: Build the target
all: $(TARGET)

# Link the object files to create the final executable
# $(CC) is the ARM cross-compiler provided by the SDK script
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Compile source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Cleanup command (run 'make clean' to delete built files)
clean:
	rm -f $(OBJS) $(TARGET)
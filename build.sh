#!/bin/bash

# =============================================================================
# Q7 BUILD SCRIPT
# Follows Q7 User Manual Section 11.2 and 11.6
# =============================================================================

# 1. DEFINE SDK PATH
# This path comes from Manual Section 11.2 [cite: 429]
SDK_ENV_SCRIPT="/opt/xiphos/sdk/ark/environment-setup-cortexa9hf-neon-xiphos-linux-gnueabi"

# 2. CHECK IF SDK EXISTS
if [ ! -f "$SDK_ENV_SCRIPT" ]; then
    echo "[Error] SDK script not found at $SDK_ENV_SCRIPT"
    echo "Please check Q7 Manual Section 11.2 for installation."
    exit 1
fi

# 3. SOURCE THE ENVIRONMENT
# This is critical. It fills variables like $CC with the correct ARM compiler.
echo "Loading Q7 SDK Environment..."
source "$SDK_ENV_SCRIPT"

# 4. RUN MAKE
# This calls your 'Makefile'. Because we sourced the SDK above, 
# 'make' will use the correct cross-compiler automatically.
echo "Running Make..."
make

# 5. CHECK SUCCESS
if [ $? -eq 0 ]; then
    echo "----------------------------------------"
    echo "[SUCCESS] Build complete."
    echo "Executable created: sensor_test_app"
    echo "----------------------------------------"
else
    echo "[FAILURE] Build failed."
    exit 1
fi
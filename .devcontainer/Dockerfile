FROM espressif/idf:v5.3.2

# Install Node.js 24 + usbip utils for USB device access
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl \
        usbutils \
        linux-tools-generic \
    && curl -fsSL https://deb.nodesource.com/setup_24.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# dialout group gives access to /dev/ttyACM* and /dev/ttyUSB*
RUN usermod -aG dialout root

WORKDIR /workspace

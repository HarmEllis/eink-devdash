FROM espressif/idf:v5.3.2

# Node.js 24 (API server dev)
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl \
        python3-pip \
    && curl -fsSL https://deb.nodesource.com/setup_24.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

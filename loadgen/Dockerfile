FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# build-essential - provides gcc, make, g++, and other build tools
# libcurl4-openssl-dev -  libcurl to fire HTTP requests
# Cleanup (rm -rf /var/lib/apt/lists/*) reduces image size
RUN apt-get update && apt-get install -y \
    build-essential libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/loadgen
COPY . /opt/loadgen
RUN make

# Run loadgen with unbuffered stdout, allow CLI flags to pass properly
ENTRYPOINT ["sh", "-c", "stdbuf -o0 ./loadgen \"$@\"", "--"]

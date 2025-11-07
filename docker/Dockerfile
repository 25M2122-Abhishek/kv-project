# Single-container that runs PostgreSQL and the server.
FROM ubuntu:22.04

RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|http://mirrors.edge.kernel.org/ubuntu/|g' /etc/apt/sources.list

ENV DEBIAN_FRONTEND=noninteractive

# Install tools, libpq dev, jansson, civetweb dev (if available), build essentials
RUN apt-get update && apt-get install -y \
    build-essential \
    libpq-dev \
    libjansson-dev \
    wget \
    ca-certificates \
    supervisor \
    postgresql postgresql-contrib \
    git \
    pkg-config \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Install civetweb from source
RUN git clone https://github.com/civetweb/civetweb.git /tmp/civetweb && \
    cd /tmp/civetweb && \
    mkdir -p _build && cd _build && \
    cmake .. -DCIVETWEB_BUILD_EXAMPLES=OFF -DCIVETWEB_BUILD_TESTS=OFF && \
    make -j$(nproc) && make install && \
    cd / && rm -rf /tmp/civetweb

# Copy server source and build
WORKDIR /opt/kv_server
COPY server /opt/kv_server/server
RUN cd /opt/kv_server/server && make

# Copy SQL init script and entrypoint
COPY docker/entrypoint.sh /entrypoint.sh
COPY server/sql/init.sql /init.sql
RUN chmod +x /entrypoint.sh

EXPOSE 8080 5432

# CMD ["/entrypoint.sh"]
ENTRYPOINT ["/entrypoint.sh"]

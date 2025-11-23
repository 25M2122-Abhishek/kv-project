FROM debian:latest

ENV DEBIAN_FRONTEND=noninteractive

# Install monitoring tools
RUN apt update && apt install -y \
    sysstat \
    procps \
    htop \
    iotop \
    iproute2 \
    util-linux \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY kv_monitor/monitor.sh /app/monitor.sh
RUN chmod +x /app/monitor.sh

# ENTRYPOINT ensures monitor.sh always runs
ENTRYPOINT ["/app/monitor.sh"]
# CMD defines default arguments, can be overridden
CMD ["4-7", "0-3", "8", "1", "1", "1"]

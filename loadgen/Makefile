CC = gcc
CFLAGS = -Wall -O2 -I./include -pthread
LIBS = -lcurl

SRCS = src/main.c src/worker.c src/metrics.c src/key_registry.c
OBJS = $(SRCS:.c=.o)
TARGET = loadgen

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

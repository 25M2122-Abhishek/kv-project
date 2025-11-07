CC = gcc
# CFLAGS = -Wall -Wextra -O2 -pthread -I./include
CFLAGS = -Wall -Wextra -O2 -pthread -I./include -I/usr/include/postgresql
LIBS = -lcivetweb -lpq -ljansson

SRCS = src/main.c src/http_server.c src/cache.c src/db.c
OBJS = $(SRCS:.c=.o)
TARGET = kv_server

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

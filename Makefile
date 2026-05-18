CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

TARGET = p2p_peer
SRC = p2p_peer.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
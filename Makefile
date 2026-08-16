CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O3 -std=gnu11
LDFLAGS ?= 
LDLIBS ?= -libverbs

TARGET = test
SRCS = main_test.c pg.c
OBJS = $(SRCS:.c=.o)
HEADERS = pg.h pg_internal.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

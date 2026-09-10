CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O3 -std=gnu11 -msse4.2 -fno-strict-aliasing -fwrapv
LDFLAGS ?= 
LDLIBS ?= -libverbs

MODE ?= auto
ifeq ($(MODE),eager)
    CFLAGS += -DPG_MODE_EAGER
else ifeq ($(MODE),rendezvous)
    CFLAGS += -DPG_MODE_RENDEZVOUS
else
    CFLAGS += -DPG_MODE_AUTO
endif

RDV_VARIANT ?= classic
ifeq ($(RDV_VARIANT),pipeline)
    CFLAGS += -DPG_RDV_PIPELINE
else
    CFLAGS += -DPG_RDV_CLASSIC
endif

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

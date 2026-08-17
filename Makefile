CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O3 -std=gnu11 -msse4.2
LDFLAGS ?= 
LDLIBS ?= -libverbs

PROFILE ?= bringup
ifeq ($(PROFILE),perf)
    CFLAGS += -DPROFILE_PERF
endif

WORKBUFFER ?= safe
ifeq ($(WORKBUFFER),inplace)
    CFLAGS += -DPG_WORKBUFFER_INPLACE
endif

MODE ?= rendezvous
ifeq ($(MODE),eager)
    CFLAGS += -DPG_MODE_EAGER
else ifeq ($(MODE),auto)
    CFLAGS += -DPG_MODE_AUTO
else
    CFLAGS += -DPG_MODE_RENDEZVOUS
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

check: $(TARGET)
	python3 test_v1_local.py

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean check

CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O3 -std=gnu11 -msse4.2 -fno-strict-aliasing -fwrapv
LDFLAGS ?=
LDLIBS ?= -libverbs

MODE ?= auto
WORKBUFFER ?= inplace
TEST_HOOKS ?= 0

ifeq ($(MODE),eager)
    override CPPFLAGS += -DPG_MODE_EAGER
else ifeq ($(MODE),rendezvous)
    override CPPFLAGS += -DPG_MODE_RENDEZVOUS
else ifeq ($(MODE),auto)
    override CPPFLAGS += -DPG_MODE_AUTO
else
    $(error MODE must be auto, eager, or rendezvous)
endif

ifeq ($(WORKBUFFER),inplace)
    override CPPFLAGS += -DPG_WORKBUFFER_INPLACE
else ifneq ($(WORKBUFFER),safe)
    $(error WORKBUFFER must be inplace or safe)
endif

ifeq ($(TEST_HOOKS),1)
    override CPPFLAGS += -DPG_TEST_HOOKS
else ifneq ($(TEST_HOOKS),0)
    $(error TEST_HOOKS must be 0 or 1)
endif

all: test

# Always rebuild: changing MODE or WORKBUFFER must change the executable.
test: test.c pg.c pg.h pg_internal.h FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ test.c pg.c $(LDLIBS)

help:
	@echo 'Build: make MODE=auto|eager|rendezvous WORKBUFFER=inplace|safe TEST_HOOKS=0|1'
	@echo 'Defaults: MODE=auto WORKBUFFER=inplace TEST_HOOKS=0'
	@echo 'Run on each host: ./test -myindex 01 -list host1 host2 [host3 host4]'
	@echo 'Use the same build and host list on every rank; indices start at 01.'
	@echo 'Library tuning environment variables:'
	@echo '  PG_PIPELINE_CHUNK PG_RDMA_WINDOW PG_RDMA_SIGNAL_INTERVAL PG_BATCH_SIZE'
	@echo '  PG_EAGER_THRESHOLD PG_EAGER_WINDOW'
	@echo 'With TEST_HOOKS=1: PG_TEST_DELAY_RANK PG_TEST_DELAY_AFTER_RS_US'

clean:
	rm -f test *.o

FORCE:

.PHONY: all help clean FORCE

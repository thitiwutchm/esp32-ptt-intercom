# Unit tests for components/ptt_core, built with the host compiler, plus a
# check that tools/ptt_peer.py matches the C protocol and codec.
#   make -C test/host
CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
CORE := ../../components/ptt_core
SRCS := test_main.c $(wildcard $(CORE)/*.c)

all: run

test_ptt: $(SRCS) $(wildcard $(CORE)/include/*.h)
	$(CC) $(CFLAGS) -I$(CORE)/include $(SRCS) -lm -o $@

libptt_core.so: $(wildcard $(CORE)/*.c) $(wildcard $(CORE)/include/*.h)
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror -shared -fPIC -I$(CORE)/include $(wildcard $(CORE)/*.c) -o $@

run: test_ptt libptt_core.so
	./test_ptt
	python3 test_interop.py

clean:
	rm -f test_ptt libptt_core.so

.PHONY: all run clean

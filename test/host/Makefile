# Unit tests for components/ptt_core, built with the host compiler, plus a
# check that tools/ptt_peer.py matches the C protocol and codec.
#   make -C test/host
CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
CORE := ../../components/ptt_core
VOIP := ../../components/voip_core
SETUP := ../../components/setup_core
PRESENCE := ../../components/presence_core
SRCS := test_main.c $(wildcard $(CORE)/*.c)

all: run

test_ptt: $(SRCS) $(wildcard $(CORE)/include/*.h)
	$(CC) $(CFLAGS) -I$(CORE)/include $(SRCS) -lm -o $@

test_voip: test_voip.c $(wildcard $(VOIP)/*.c) $(wildcard $(VOIP)/include/*.h)
	$(CC) $(CFLAGS) -I$(VOIP)/include test_voip.c $(wildcard $(VOIP)/*.c) -lm -o $@

test_setup: test_setup.c $(wildcard $(SETUP)/*.c) $(wildcard $(SETUP)/include/*.h)
	$(CC) $(CFLAGS) -I$(SETUP)/include test_setup.c $(wildcard $(SETUP)/*.c) -o $@

# Only the hardware-independent files: flash ring log and the IRK/RPA resolver.
test_presence: test_presence.c $(PRESENCE)/flog.c $(PRESENCE)/irk_resolver.c $(wildcard $(PRESENCE)/include/*.h)
	$(CC) $(CFLAGS) -I$(PRESENCE)/include test_presence.c $(PRESENCE)/flog.c $(PRESENCE)/irk_resolver.c -o $@

libptt_core.so: $(wildcard $(CORE)/*.c) $(wildcard $(CORE)/include/*.h)
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror -shared -fPIC -I$(CORE)/include $(wildcard $(CORE)/*.c) -o $@

run: test_ptt test_voip test_setup test_presence libptt_core.so
	./test_ptt
	./test_voip
	./test_setup
	./test_presence
	python3 test_interop.py

clean:
	rm -f test_ptt test_voip test_setup test_presence libptt_core.so

.PHONY: all run clean

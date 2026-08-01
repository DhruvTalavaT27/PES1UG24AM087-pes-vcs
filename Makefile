CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lcrypto

# ─── Main binary ─────────────────────────────────────────────────────────────

SRCS = object.c tree.c index.c commit.c status.c strata.c
OBJS = $(SRCS:.c=.o)
DEPS = strata.h index.h tree.h commit.h status.h

strata: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

# ─── Test binaries ───────────────────────────────────────────────────────────

test_objects: test_objects.o object.o
	$(CC) -o $@ $^ $(LDFLAGS)

test_tree: test_tree.o object.o tree.o index.o
	$(CC) -o $@ $^ $(LDFLAGS)

# ─── Convenience targets ─────────────────────────────────────────────────────

.PHONY: all clean test test-unit test-integration

all: strata test_objects test_tree

clean:
	rm -f strata test_objects test_tree $(OBJS) test_objects.o test_tree.o
	rm -rf .strata

test: test-unit test-integration

test-unit: test_objects test_tree
	@echo "=== Object store tests ==="
	./test_objects
	@echo ""
	@echo "=== Tree tests ==="
	./test_tree

test-integration: strata
	@echo "=== End-to-end tests ==="
	bash test_sequence.sh

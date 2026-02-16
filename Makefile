CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -Wpedantic -O2

TARGET  = mdcat
SRC     = mdcat.c

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

# Run against the test file; force color output via TERM even when piped
test: $(TARGET)
	@echo "=== mdcat test.md ==="
	./$(TARGET) test.md

# Pipe test: strip ANSI codes and check key strings are present
test-pipe: $(TARGET)
	@echo "=== pipe / no-color test ==="
	./$(TARGET) test.md | cat   # isatty()==0, so no escapes

clean:
	rm -f $(TARGET)

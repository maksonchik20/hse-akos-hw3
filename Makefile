CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS += -pthread -lrt
BUILD ?= build
ROLES = server flower gardener
SHM = $(addprefix $(BUILD)/independent-shm/,$(ROLES))
MQ = $(addprefix $(BUILD)/independent-mq/,$(ROLES))
FORK = $(BUILD)/fork-named $(BUILD)/fork-unnamed
COMMON = $(addprefix $(BUILD)/,model.o ipc.o config.o io.o)
HEADERS = src/model.h src/ipc.h src/config.h src/io.h

.PHONY: all test clean
all: $(FORK) $(SHM) $(MQ)

$(BUILD)/fork-named: MODE = FORK_NAMED
$(BUILD)/fork-unnamed: MODE = FORK_UNNAMED
$(SHM): MODE = INDEPENDENT_SHM
$(MQ): MODE = INDEPENDENT_MQ
$(FORK) $(filter %/server,$(SHM) $(MQ)): ROLE = COORDINATOR
$(filter %/flower,$(SHM) $(MQ)): ROLE = FLOWER
$(filter %/gardener,$(SHM) $(MQ)): ROLE = GARDENER

$(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(FORK) $(SHM) $(MQ): src/main.c $(HEADERS) $(COMMON)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMODE=$(MODE) -DROLE=$(ROLE) $< $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

test: all
	python3 tests/run.py $(BUILD)

clean:
	rm -rf $(BUILD)

# wrapper for CMake

.PHONY: build configure clean fetch-deps format format-check

BUILD_DIR := build/
# The built binary, and the convenience symlink to it left at the project root
# so it can be run as ./ts-ref rather than ./build/ts-ref. A link rather than a
# copy, so it always resolves to the most recent build; nothing is installed
# outside the project.
BINARY := ts-ref
SOURCES := $(shell find src include tests -name '*.cc' -o -name '*.h')
# Downloaded by fetch-deps, and so safe for clean to delete.
FETCHED_DEPS := third_party/nlohmann

build: configure
	cmake --build $(BUILD_DIR)
	@ln -sf $(BUILD_DIR)$(BINARY) $(BINARY)

configure: fetch-deps
	mkdir -p build
	cmake -S . -B $(BUILD_DIR)

format:
	clang-format -i $(SOURCES)

format-check:
	clang-format --dry-run --Werror $(SOURCES)

# Removes only what the build produced or downloaded. third_party/tree_sitter
# is vendored source, not a fetched dependency -- deleting it silently breaks
# external scanner compilation -- so it is named explicitly here rather than
# swept up by a wildcard. abseil and RE2 are fetched by CMake into
# $(BUILD_DIR)/_deps and go with it.
clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf $(FETCHED_DEPS)
	@rm -f $(BINARY)

fetch-deps:
	mkdir -p third_party/nlohmann
	curl -sSL https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp -o \
		third_party/nlohmann/json.hpp
	# verify hash
	echo 'aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63 third_party/nlohmann/json.hpp' | sha256sum -c

# wrapper for CMake

.PHONY: build configure clean fetch-deps format format-check

BUILD_DIR := build/
SOURCES := $(shell find src include tests -name '*.cc' -o -name '*.h')

build: configure
	cmake --build $(BUILD_DIR)

configure: fetch-deps
	mkdir -p build
	cmake -S . -B $(BUILD_DIR)

format:
	clang-format -i $(SOURCES)

format-check:
	clang-format --dry-run --Werror $(SOURCES)

clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf third_party/*/

fetch-deps:
	mkdir -p third_party/nlohmann
	curl -sSL https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp -o \
		third_party/nlohmann/json.hpp
	# verify hash
	echo 'aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63 third_party/nlohmann/json.hpp' | sha256sum -c

.PHONY: all build clean toolchain test

all: build

build:
	./markdown_viewer/build.sh

test:
	./test/run.sh

clean:
	rm -f markdown_viewer/*.app markdown_viewer/*.elf
	rm -f markdown_viewer/*.bin markdown_viewer/*.map
	rm -f markdown_viewer/markdown_assets.h
	rm -rf markdown_viewer/__pycache__

# Optional: install the pinned ARM toolchain into ./.toolchain (gitignored).
toolchain:
	./tools/fetch-toolchain.sh

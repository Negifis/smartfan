VERSION := 1.0.0
TARGET  := -arch x86_64 -mmacosx-version-min=13.0
CFLAGS  := -O2 -Wall -Wextra $(TARGET)
DIST    := dist/SmartFan-$(VERSION)-intel

all: build/smartfan build/smartfan-dump build/SmartFan.app

build/smartfan: src/smartfan.c src/smc.c src/smc.h
	@mkdir -p build
	clang $(CFLAGS) -o $@ src/smartfan.c src/smc.c -framework IOKit

build/smartfan-dump: src/dump.c src/smc.c src/smc.h
	@mkdir -p build
	clang $(CFLAGS) -o $@ src/dump.c src/smc.c -framework IOKit

build/SmartFan.app: src/menubar.swift resources/Info.plist
	@mkdir -p $@/Contents/MacOS
	swiftc -O -target x86_64-apple-macos13.0 -o $@/Contents/MacOS/SmartFan src/menubar.swift
	cp resources/Info.plist $@/Contents/Info.plist
	codesign --force -s - $@

# Zip with prebuilt binaries for a GitHub release.
release: all
	rm -rf $(DIST) $(DIST).zip
	mkdir -p $(DIST)
	cp -R build/smartfan build/smartfan-dump build/SmartFan.app config install.sh README.md LICENSE $(DIST)/
	cd dist && ditto -c -k --keepParent SmartFan-$(VERSION)-intel SmartFan-$(VERSION)-intel.zip

clean:
	rm -rf build dist

.PHONY: all release clean

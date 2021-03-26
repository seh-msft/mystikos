TOP=$(abspath ../../../..)
include $(TOP)/defs.mak

include $(TOP)/rules.mak

all: src/lib.rs
	cargo build --release
	cp target/release/libclock.so ../

headers: src/lib.rs
	cbindgen -c cbindgen.toml src/lib.rs -o clock.h
	cp clock.h $(TOP)/include/myst/clock.h


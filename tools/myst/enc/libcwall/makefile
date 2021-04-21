
all: src/lib.rs wrapper.h build.rs
	cargo build --release

test:
	cp ../libpoints/libpoints.so ./target/*/
	cp ../librectangles/librectangles.so ./target/*/
	cargo test

clean:
	cargo clean
	rm *.so bindings.rs

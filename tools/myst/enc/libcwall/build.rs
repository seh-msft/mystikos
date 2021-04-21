extern crate bindgen;

use std::env;
use std::path::PathBuf;

fn main() {
    let root: String = env::var("CARGO_MANIFEST_DIR").unwrap().to_owned() + &"/".to_owned();
    
    /*
    println!("cargo:rustc-link-search=native={}", root.clone() + "../libpoints");
    println!("cargo:rustc-link-lib=dylib={}", "points");
    
    println!("cargo:rustc-link-search=native={}", root.clone() + "../librectangles");
    println!("cargo:rustc-link-lib=dylib={}", "rectangles");
    */

    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=src/bindings.rs");

    let bindings = bindgen::Builder::default()
        // The input header we would like to generate
        // bindings for.
        .header("wrapper.h")
        .clang_arg("-I../../../../include")
        .clang_arg("-I../../../../third_party/openenclave/openenclave/include")
        .clang_arg("-I../../../../third_party/openenclave/openenclave/build/output/include")
        // Tell cargo to invalidate the built crate whenever any of the
        // included header files changed.
        .parse_callbacks(Box::new(bindgen::CargoCallbacks))
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(root.clone());
    bindings
        .write_to_file(out_path.join("src/bindings.rs"))
        .expect("Couldn't write bindings!");
}

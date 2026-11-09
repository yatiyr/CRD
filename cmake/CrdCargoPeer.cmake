# crd_require_cargo(<tag>): a configure error unless CRD_CARGO names the pinned Cargo.
function(crd_require_cargo tag)
    if(NOT EXISTS "${CRD_CARGO}")
        message(FATAL_ERROR "[${tag}] CRD_CARGO names no Cargo: install the pinned Rust toolchain with "
                            "scripts/install-rust.py and configure with the CRD_CARGO it prints")
    endif()
endfunction()

# crd_add_cargo_peer(<target> <manifest> <directory> <artifact> <tag>): a Cargo package linted by Clippy and built with
# the pinned Rust toolchain (CRD_CARGO) by `cargo build --locked`, so every crate is the one Cargo.lock names, verified
# by its SHA-256. CARGO_HOME (the crate downloads) and the target directory (_deps/<directory>) stay inside this tree,
# and the package's own warnings, the compiler's and Clippy's, fail the build. Cargo decides what to rebuild, so the
# target always runs it. Sets <target>_OUTPUT to the built <artifact> in the caller's scope.
function(crd_add_cargo_peer target manifest directory artifact tag)
    set(target_directory "${CMAKE_BINARY_DIR}/_deps/${directory}")
    set(output "${target_directory}/release/${artifact}")
    get_filename_component(rust_bin "${CRD_CARGO}" DIRECTORY)
    set(cargo_env "${CMAKE_COMMAND}" -E env "CARGO_HOME=${CMAKE_BINARY_DIR}/_deps/cargo-home" "CARGO=${CRD_CARGO}"
                  "RUSTC=${rust_bin}/rustc" "RUSTFLAGS=-D warnings")
    set(cargo_args --locked --release --manifest-path "${manifest}" --target-dir "${target_directory}")
    add_custom_target(${target} ALL
        COMMAND ${cargo_env} "${rust_bin}/cargo-clippy" clippy ${cargo_args} -- -D warnings
        COMMAND ${cargo_env} "${CRD_CARGO}" build ${cargo_args}
        BYPRODUCTS "${output}"
        COMMENT "[${tag}] cargo clippy and cargo build --locked: ${target}"
        USES_TERMINAL
        VERBATIM)
    set(${target}_OUTPUT "${output}" PARENT_SCOPE)
    message(STATUS "[${tag}] ${target} (Cargo.lock), Rust from ${rust_bin}")
endfunction()

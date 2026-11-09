# crd_require_go(<tag>): a configure error unless CRD_GO names the pinned Go.
function(crd_require_go tag)
    if(NOT EXISTS "${CRD_GO}")
        message(FATAL_ERROR "[${tag}] CRD_GO names no Go: install the pinned Go toolchain with "
                            "scripts/install-go.py and configure with the CRD_GO it prints")
    endif()
endfunction()

# crd_add_go_peer(<target> <module> <directory> <artifact> <tag>): a Go module vetted and built with the pinned Go
# (CRD_GO), so every module is the one its go.sum names, verified by its h1 hash: GOFLAGS=-mod=readonly refuses to touch
# go.mod or go.sum, GOSUMDB=off leaves go.sum the only authority, and GOTOOLCHAIN=local refuses any other toolchain. The
# module and build caches stay inside this tree. Go decides what to rebuild, so the target always runs it. Sets
# <target>_OUTPUT to the built <artifact> in the caller's scope.
function(crd_add_go_peer target module directory artifact tag)
    set(output "${CMAKE_BINARY_DIR}/_deps/${directory}/${artifact}")
    set(go_env "${CMAKE_COMMAND}" -E env "GOMODCACHE=${CMAKE_BINARY_DIR}/_deps/go-modules"
                  "GOCACHE=${CMAKE_BINARY_DIR}/_deps/go-cache" "GOTOOLCHAIN=local" "GOFLAGS=-mod=readonly -modcacherw"
                  "GOPROXY=https://proxy.golang.org" "GOSUMDB=off" "GOWORK=off" "GOTELEMETRY=off" "CGO_ENABLED=0")
    add_custom_target(${target} ALL
        COMMAND ${go_env} "${CRD_GO}" vet ./...
        COMMAND ${go_env} "${CRD_GO}" build -trimpath -o "${output}" .
        WORKING_DIRECTORY "${module}"
        BYPRODUCTS "${output}"
        COMMENT "[${tag}] go vet and go build: ${target}"
        USES_TERMINAL
        VERBATIM)
    set(${target}_OUTPUT "${output}" PARENT_SCOPE)
    message(STATUS "[${tag}] ${target} (go.sum), Go ${CRD_GO}")
endfunction()

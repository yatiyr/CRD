add_executable(crd-runtime
    src/main.cpp
)

target_link_libraries(crd-runtime PRIVATE
    crd-core
    crd-log
    crd-memory
)
crd_enable_default_pch(crd-runtime)

add_executable(smoke_log
    examples/smoke_log.cpp
)

target_link_libraries(smoke_log PRIVATE
    crd-core
    crd-log
)
crd_enable_default_pch(smoke_log)

add_executable(smoke_memory
    examples/smoke_memory.cpp
)

target_link_libraries(smoke_memory PRIVATE
    crd-core
    crd-log
    crd-memory
)
crd_enable_default_pch(smoke_memory)

add_executable(smoke_containers
    examples/smoke_containers.cpp
)

target_link_libraries(smoke_containers PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
)
crd_enable_default_pch(smoke_containers)

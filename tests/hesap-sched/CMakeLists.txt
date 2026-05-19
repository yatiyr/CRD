add_executable(crd-hesap-sched-tests
    test_task_graph.cpp
)

target_link_libraries(crd-hesap-sched-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-jobs
    crd-hesap
    crd-hesap-sched
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-sched-tests)

catch_discover_tests(crd-hesap-sched-tests)

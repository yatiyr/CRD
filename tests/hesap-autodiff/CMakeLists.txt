# crd-hesap-autodiff tests — Phase 3.1.6 v15. This target also SERVES AS THE LINK-ISOLATION SMOKE (ADR-0097 §1):
# it links ONLY crd-hesap-autodiff (+ the base substrate) — a clean link proves a scalar-Dual/Jet consumer drags
# neither the tensor JVPs nor the solver bridges nor the reverse tape.
add_executable(crd-hesap-autodiff-tests
    test_dual_forward.cpp
    test_jet.cpp
    test_jet_simd.cpp
    test_jvp_rules.cpp
    test_hyperdual.cpp
    test_drivers.cpp
    test_sparsity.cpp
    test_matrix_jvp.cpp
    test_taylor.cpp
    test_complex_dual.cpp
    test_ad_cli.cpp
    test_reverse.cpp
    test_reverse_rules.cpp
    test_nn_reverse.cpp
    test_bicoloring.cpp
    test_sparse_reverse.cpp
    test_matrix_reverse.cpp
    test_suite_reverse.cpp
    test_hvp.cpp
    test_vhvp.cpp
    test_ode_adjoint.cpp
    test_implicit_diff.cpp
    test_graph_ad.cpp
    test_determinism_moat.cpp
    test_topopt.cpp
    test_neural_ode.cpp
    test_kan.cpp
)

target_link_libraries(crd-hesap-autodiff-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-autodiff
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-autodiff-tests)

catch_discover_tests(crd-hesap-autodiff-tests)

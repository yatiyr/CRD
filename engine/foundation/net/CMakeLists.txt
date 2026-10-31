# crd-net -- the network foundation (NET.1 decision D1: the one owner of every path that can reach another machine).
#
# Ships the version and extension negotiation every Cerid wire protocol carries (NET-STD): offers, answers, GREASE
# in every build, ignore-unknown, downgrade detection and the transcript bytes the handshake binds, with the
# identifiers generated from the standards register (register_ids.hpp, scripts/gen-register-ids.py). Transport,
# sessions, channels and admission land here with 4.2a and the NETCRASH rows. Contract:
# docs/design/network-foundation.md#negotiation.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-net STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-net
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(crd-net
    PUBLIC
        crd-core
        crd-containers
)

target_compile_features(crd-net PUBLIC cxx_std_20)
crd_enable_default_pch(crd-net)

# Cerid::net of the relocatable package.
crd_package_library(crd-net NAME net INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")

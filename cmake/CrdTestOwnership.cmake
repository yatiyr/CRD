# Direct executable tests retain their commands/properties and publish CMake's actual artifact identity.
include_guard(GLOBAL)

function(crd_test_target TEST_NAME TARGET_NAME)
    if(NOT TEST "${TEST_NAME}" OR NOT TARGET "${TARGET_NAME}")
        message(FATAL_ERROR "crd_test_target requires an existing test and target")
    endif()
    get_target_property(_type "${TARGET_NAME}" TYPE)
    if(NOT _type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "Direct executable test owner must be an executable: ${TARGET_NAME}")
    endif()
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY LABELS
        "cerid.test.target=${TARGET_NAME}" "cerid.test.executable=$<TARGET_FILE:${TARGET_NAME}>")
endfunction()

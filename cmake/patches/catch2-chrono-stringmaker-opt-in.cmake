# catch2-chrono-stringmaker-opt-in — applied by crd_patched_copy() to Catch2/src/catch2/catch_tostring.hpp (3.7.1).
#
# catch_tostring.hpp, which every assertion macro reaches, includes <chrono> unconditionally for its
# std::chrono::duration and time_point printers. In C++20 <chrono> brings the calendar, the time-zone database and
# <format>: the 2026-10-18 compile-time profiles (docs/bench/2026-10-18-build-speed-compile-time.md) measured it as
# the most expensive header of the build, 575 s in 1,155 units on MSVC and 633 s in 1,126 units with libstdc++ (plus
# 222 s of std::vformat_to instantiations), almost all through this header. Catch2 v2 made these printers opt-in under
# CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER; this restores that switch (CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS turns it on
# too). Assertions pass and fail exactly as before; only a failure message changes: without the switch a duration
# prints through C++20's operator<< when the test includes <chrono>, and a type with no operator<< prints {?}.
# Catch2's own sources that use <chrono> (catch_config.hpp, the benchmark clocks) include it themselves.
crd_patch_replace(
    [[// Separate std::chrono::duration specialization
#include <ctime>
#include <ratio>
#include <chrono>
]]
    [[// Separate std::chrono::duration specialization
// crd-patch catch2-chrono-stringmaker-opt-in: opt-in, as in Catch2 v2 (cmake/patches/catch2-chrono-stringmaker-opt-in.cmake)
#if defined(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS) && !defined(CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER)
#  define CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER
#endif
#if defined(CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER)
#include <ctime>
#include <ratio>
#include <chrono>
]])
crd_patch_replace(
    [[            return std::string(timeStamp, timeStampSize - 1);
        }
    };
}

#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
]]
    [[            return std::string(timeStamp, timeStampSize - 1);
        }
    };
}
#endif // CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER (crd-patch catch2-chrono-stringmaker-opt-in)

#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
]])

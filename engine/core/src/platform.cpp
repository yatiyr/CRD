#include <crd/core/platform.hpp>

namespace crd
{
	const char* platform_name()
	{
#if CRD_OS_WINDOWS
		return "WINDOWS";
#elif CRD_OS_LINUX
		return "LINUX";
#elif CRD_OS_MAC
		return "MAC";
#else
        return "UNKNOWN PLATFORM!";
#endif
	}

	const char* compiler_name()
	{
#if CRD_COMPILER_MSVC
        return "MSVC";
#elif CRD_COMPILER_GCC
        return "GCC";
#elif CRD_COMPILER_CLANG
        return "CLANG";
#else
        return "UNKNOWN COMPILER!";
#endif
	}

	const char* arch_name()
	{
#if CRD_ARCH_X64
        return "X64";
#elif CRD_ARCH_ARM64
        return "ARM64";
#else
        return "UNKNOWN ARCHITECTURE!";
#endif
	}
}
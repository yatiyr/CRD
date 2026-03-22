#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>

#include <cstdio>
#include <cstdlib>

#if CRD_OS_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include<Windows.h>
#endif


namespace crd::detail
{
	int report_assert_failure(const char* expression, const char* file,
		int line, const char* message)
	{
        char buffer[1024];
                
		if (message != nullptr)
		{
            const char* formatted_string_with_msg =
                "Assertion failed! \n\t expr: %s \n\t file: %s \n\t line: %d \n\t message: %s\n";
            std::snprintf(buffer, sizeof(buffer), formatted_string_with_msg, expression, file, line, message);
		}
		else
		{
            const char* formatted_string_without_msg =
				"Assertion failed! \n\t expr: %s \n\t file: %s \n\t line: %d\n";
            std::snprintf(buffer, sizeof(buffer), formatted_string_without_msg, expression, file, line);
		}

		std::fputs(buffer, stderr);

#if CRD_OS_WINDOWS
        OutputDebugStringA(buffer);
        const int result = MessageBoxA(nullptr, buffer, "CRD Assert",
			MB_ABORTRETRYIGNORE | MB_ICONERROR);
		if (result == IDABORT)
		{
            std::abort();
		}
		

		return (result == IDRETRY) ? 2 : 0;
#else
        return 2;
#endif
	}
}
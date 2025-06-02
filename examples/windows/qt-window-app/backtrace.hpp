#pragma once

#include <string>

namespace backtrace 
{
bool initialize_crashpad(const std::string& url,
                         const std::wstring& handler_path);
void crash_memset();
}

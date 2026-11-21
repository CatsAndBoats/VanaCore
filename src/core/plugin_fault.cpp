#include "plugin_fault.hpp"

#include <vanacore/log_file.hpp>

#include <cstdio>

#include "crash.hpp"

namespace vanacore::plugin_fault {

int record(const EXCEPTION_POINTERS* info, Fault& fault)
{
    const EXCEPTION_RECORD& r = *info->ExceptionRecord;
    fault.code = r.ExceptionCode;
    fault.address = r.ExceptionAddress;
    if (r.NumberParameters >= 2) {
        fault.access = r.ExceptionInformation[0];
        fault.target = r.ExceptionInformation[1];
    }
    crash::cpp_exception_message(r, fault.message, sizeof(fault.message));
    if (r.ExceptionCode != crash::kCppException)
        crash::callers(*info->ContextRecord, &fault.caller, 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

std::string describe(const Fault& fault)
{
    char text[200];
    if (fault.code == EXCEPTION_ACCESS_VIOLATION)
        sprintf_s(text, "access violation %s 0x%08lX", fault.access == 1 ? "writing" : fault.access == 8 ? "executing" : "reading",
                  static_cast<unsigned long>(fault.target));
    else if (fault.code == 0xE06D7363)
        sprintf_s(text, "a C++ exception");
    else
        sprintf_s(text, "exception 0x%08lX", fault.code);
    std::string out = std::string(text) + " at ";
    for (const wchar_t c : describe_address(fault.address))
        out += static_cast<char>(c);
    if (fault.caller != 0) {
        out += " from ";
        for (const wchar_t c : describe_address(reinterpret_cast<const void*>(fault.caller)))
            out += static_cast<char>(c);
    }
    return out;
}

} // namespace vanacore::plugin_fault

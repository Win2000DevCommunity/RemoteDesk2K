#ifdef _MSC_VER
// Use undecorated name for the stub, .def will export the correct symbol
__declspec(naked) void __stdcall __security_check_cookie(void) { __asm { ret 4 } }

// Stub for buffer overflow range check failure (required by newer MSVC)
void __cdecl __report_rangecheckfailure(void) { /* do nothing */ }
#endif

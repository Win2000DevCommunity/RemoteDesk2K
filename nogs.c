/*
 * Security cookie stub for older CRT
 * Provides __security_check_cookie when not available
 */

#ifdef _MSC_VER
#pragma warning(disable: 4273)
#endif

/* Security cookie variable */
unsigned long __security_cookie = 0xBB40E64E;

/* Security check function - does nothing in this stub */
void __fastcall __security_check_cookie(unsigned long cookie)
{
    (void)cookie;
    /* No-op for compatibility with older systems */
}

/* Report failure - just return */
void __cdecl __report_gsfailure(void)
{
    /* In a real implementation, this would terminate the process */
}

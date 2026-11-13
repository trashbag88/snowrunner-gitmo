; Gitmo - version.dll proxy stubs.
; Every export jumps straight to the same function in the real
; C:\Windows\System32\version.dll, so arguments pass through untouched.
; g_real[] is filled in by DllMain (gitmo.cpp) in the order below.

EXTERN g_real:QWORD

STUB MACRO name, idx
name&_stub PROC
    jmp QWORD PTR [g_real + idx*8]
name&_stub ENDP
ENDM

.code
STUB GetFileVersionInfoA,        0
STUB GetFileVersionInfoByHandle, 1
STUB GetFileVersionInfoExA,      2
STUB GetFileVersionInfoExW,      3
STUB GetFileVersionInfoSizeA,    4
STUB GetFileVersionInfoSizeExA,  5
STUB GetFileVersionInfoSizeExW,  6
STUB GetFileVersionInfoSizeW,    7
STUB GetFileVersionInfoW,        8
STUB VerFindFileA,               9
STUB VerFindFileW,               10
STUB VerInstallFileA,            11
STUB VerInstallFileW,            12
STUB VerLanguageNameA,           13
STUB VerLanguageNameW,           14
STUB VerQueryValueA,             15
STUB VerQueryValueW,             16
END

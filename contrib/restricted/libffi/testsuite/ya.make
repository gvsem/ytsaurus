# Generated by devtools/yamaker.

VERSION(3.3)

RECURSE(
    libffi.bhaible
    libffi.call
    libffi.closures
)

IF (NOT OS_IOS)
    RECURSE(
        libffi.go
    )
ENDIF()

IF (NOT OS_WINDOWS AND NOT ARCH_PPC64LE)
    RECURSE(
        libffi.complex
    )
ENDIF()

# Generated by devtools/yamaker.

LIBRARY()

VERSION(16.0.0)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/lib/DebugInfo/DWARF
    contrib/libs/llvm16/lib/MC
    contrib/libs/llvm16/lib/Object
    contrib/libs/llvm16/lib/Support
    contrib/libs/llvm16/lib/Target
)

ADDINCL(
    contrib/libs/llvm16/lib/DWP
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    DWP.cpp
    DWPError.cpp
)

END()

# Generated by devtools/yamaker.

PROGRAM()

VERSION(16.0.0)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/lib/Demangle
    contrib/libs/llvm16/lib/Support
)

ADDINCL(
    contrib/libs/llvm16/tools/llvm-undname
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    llvm-undname.cpp
)

END()

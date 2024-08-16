# Generated by devtools/yamaker.

LIBRARY()

VERSION(16.0.0)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/include
    contrib/libs/llvm16/lib/Analysis
    contrib/libs/llvm16/lib/IR
    contrib/libs/llvm16/lib/MC
    contrib/libs/llvm16/lib/Support
    contrib/libs/llvm16/lib/TargetParser
    contrib/libs/llvm16/lib/Transforms/Scalar
    contrib/libs/llvm16/lib/Transforms/Utils
)

ADDINCL(
    contrib/libs/llvm16/lib/Frontend/OpenMP
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    OMP.cpp
    OMPContext.cpp
    OMPIRBuilder.cpp
)

END()

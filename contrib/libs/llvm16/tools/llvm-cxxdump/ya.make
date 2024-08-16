# Generated by devtools/yamaker.

PROGRAM()

VERSION(16.0.0)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/lib/AsmParser
    contrib/libs/llvm16/lib/BinaryFormat
    contrib/libs/llvm16/lib/Bitcode/Reader
    contrib/libs/llvm16/lib/Bitstream/Reader
    contrib/libs/llvm16/lib/Demangle
    contrib/libs/llvm16/lib/IR
    contrib/libs/llvm16/lib/IRReader
    contrib/libs/llvm16/lib/MC
    contrib/libs/llvm16/lib/MC/MCParser
    contrib/libs/llvm16/lib/Object
    contrib/libs/llvm16/lib/Remarks
    contrib/libs/llvm16/lib/Support
    contrib/libs/llvm16/lib/Target/AArch64/TargetInfo
    contrib/libs/llvm16/lib/Target/ARM/TargetInfo
    contrib/libs/llvm16/lib/Target/BPF/TargetInfo
    contrib/libs/llvm16/lib/Target/LoongArch/TargetInfo
    contrib/libs/llvm16/lib/Target/NVPTX/TargetInfo
    contrib/libs/llvm16/lib/Target/PowerPC/TargetInfo
    contrib/libs/llvm16/lib/Target/WebAssembly/TargetInfo
    contrib/libs/llvm16/lib/Target/X86/TargetInfo
    contrib/libs/llvm16/lib/TargetParser
    contrib/libs/llvm16/lib/TextAPI
)

ADDINCL(
    contrib/libs/llvm16/tools/llvm-cxxdump
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    Error.cpp
    llvm-cxxdump.cpp
)

END()

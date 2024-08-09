# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(1.17.0)

LICENSE(MIT)

PEERDIR(
    contrib/python/pycparser
    contrib/python/setuptools
    contrib/restricted/libffi
)

ADDINCL(
    contrib/restricted/libffi/include
)

NO_COMPILER_WARNINGS()

NO_LINT()

SRCS(
    c/_cffi_backend.c
)

PY_REGISTER(
    _cffi_backend
)

PY_SRCS(
    TOP_LEVEL
    cffi/__init__.py
    cffi/_imp_emulation.py
    cffi/_shimmed_dist_utils.py
    cffi/api.py
    cffi/backend_ctypes.py
    cffi/cffi_opcode.py
    cffi/commontypes.py
    cffi/cparser.py
    cffi/error.py
    cffi/ffiplatform.py
    cffi/lock.py
    cffi/model.py
    cffi/pkgconfig.py
    cffi/recompiler.py
    cffi/setuptools_ext.py
    cffi/vengine_cpy.py
    cffi/vengine_gen.py
    cffi/verifier.py
)

RESOURCE_FILES(
    PREFIX contrib/python/cffi/py3/
    .dist-info/METADATA
    .dist-info/entry_points.txt
    .dist-info/top_level.txt
    cffi/_cffi_errors.h
    cffi/_cffi_include.h
    cffi/_embedding.h
    cffi/parse_c_type.h
)

SUPPRESSIONS(
    lsan.supp
)

END()

RECURSE(
    gen
)

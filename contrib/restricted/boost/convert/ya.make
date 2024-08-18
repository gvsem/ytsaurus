# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.86.0)

ORIGINAL_SOURCE(https://github.com/boostorg/convert/archive/boost-1.86.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/config
    contrib/restricted/boost/core
    contrib/restricted/boost/function_types
    contrib/restricted/boost/lexical_cast
    contrib/restricted/boost/math
    contrib/restricted/boost/mpl
    contrib/restricted/boost/optional
    contrib/restricted/boost/parameter
    contrib/restricted/boost/range
    contrib/restricted/boost/spirit
    contrib/restricted/boost/type_traits
)

ADDINCL(
    GLOBAL contrib/restricted/boost/convert/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

END()

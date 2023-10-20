# Generated by devtools/yamaker (pypi).

PY2_LIBRARY()

VERSION(4.6)

LICENSE(ZPL-2.1)

PEERDIR(
    contrib/python/setuptools
)

NO_LINT()

PY_SRCS(
    TOP_LEVEL
    zope/event/__init__.py
    zope/event/classhandler.py
)

RESOURCE_FILES(
    PREFIX contrib/python/zope.event/py2/
    .dist-info/METADATA
    .dist-info/top_level.txt
)

END()

RECURSE_FOR_TESTS(
    tests
)
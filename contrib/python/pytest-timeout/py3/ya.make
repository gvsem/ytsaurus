# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(2.3.1)

LICENSE(MIT)

PEERDIR(
    contrib/python/pytest
)

NO_LINT()

PY_SRCS(
    TOP_LEVEL
    pytest_timeout.py
)

RESOURCE_FILES(
    PREFIX contrib/python/pytest-timeout/py3/
    .dist-info/METADATA
    .dist-info/entry_points.txt
    .dist-info/top_level.txt
)

END()

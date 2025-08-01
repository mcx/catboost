SUBSCRIBER(g:matrixnet)

PY3_PROGRAM(catboost-python)

PEERDIR(
    contrib/python/six
    contrib/python/graphviz
    contrib/python/numpy
    contrib/python/pandas
    contrib/python/scipy
    library/python/pymain
)

PY_SRCS(
    TOP_LEVEL
    __init__.py
)

PY_MAIN(library.python.pymain:run)

NO_CHECK_IMPORTS()

END()

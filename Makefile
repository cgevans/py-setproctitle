# Oh, makefile, help me with the python3 craze :D
#
# Copyright (c) 2010-2011 Daniele Varrazzo <daniele.varrazzo@gmail.com>

MKDIR = mkdir -p
RM = rm -f
PYTHON ?= python
PY2TO3 ?= 2to3

# PYVER value is 2 or 3
PYVER := $(shell $(PYTHON) -c "import sys; print(sys.version_info[0])")

BUILD_DIR = build/lib.$(PYVER)

.PHONY: build test py3 clean

ifeq (2,$(PYVER))

build:
	$(PYTHON) setup.py build --build-lib $(BUILD_DIR)

test: build
	PYTHONPATH=`pwd`/$(BUILD_DIR):$$PYTHONPATH \
		$(PYTHON) `which nosetests` -v -s -w tests

else

build: py3
	$(PYTHON) py3/setup.py build --build-lib $(BUILD_DIR)

test: build
	PYTHONPATH=$(BUILD_DIR):$$PYTHONPATH \
		$(PYTHON) py3/tests/setproctitle_test.py -v

py3: MANIFEST
	$(MKDIR) py3
	$(MKDIR) py3/src
	$(MKDIR) py3/tests
	for f in `cat MANIFEST`; do cp -v $$f py3/$$f; done
	# setup.py should be executable with python3 as distribute
	# currenlty doesn't seem to try to convert it
	$(PY2TO3) -w --no-diffs py3/tests

endif

sdist: MANIFEST
	$(PYTHON) setup.py sdist --formats=gztar,zip

MANIFEST:
	# Must run twice because the manifest contains the manifest itself.
	$(PYTHON) setup.py sdist --manifest-only
	$(PYTHON) setup.py sdist --manifest-only

clean:
	$(RM) -r MANIFEST py3 build dist



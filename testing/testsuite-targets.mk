# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# include automation-build.mk to get the path to the binary
TARGET_DEPTH = $(DEPTH)
include $(topsrcdir)/build/binary-location.mk

ifndef TEST_PACKAGE_NAME
TEST_PACKAGE_NAME := $(ANDROID_PACKAGE_NAME)
endif

# Linking xul-gtest.dll takes too long, so we disable GTest on
# Windows PGO builds (bug 1028035).
ifneq (1_WINNT,$(MOZ_PGO)_$(OS_ARCH))
BUILD_GTEST=1
endif

ifdef MOZ_B2G
BUILD_GTEST=
endif

ifneq (browser,$(MOZ_BUILD_APP))
BUILD_GTEST=
endif

ifndef COMPILE_ENVIRONMENT
BUILD_GTEST=
endif

ifndef NO_FAIL_ON_TEST_ERRORS
define check_test_error_internal
  @errors=`grep 'TEST-UNEXPECTED-' $@.log` ;\
  if test "$$errors" ; then \
	  echo '$@ failed:'; \
	  echo "$$errors"; \
          $(if $(1),echo $(1);) \
	  exit 1; \
  fi
endef
CHECK_TEST_ERROR = $(call check_test_error_internal)
CHECK_TEST_ERROR_RERUN = $(call check_test_error_internal,'To rerun your failures please run "make $@-rerun-failures"')
endif

ifeq ($(OS_ARCH),WINNT) #{
# GPU-rendered shadow layers are unsupported here
OOP_CONTENT = --setpref=layers.async-pan-zoom.enabled=true --setpref=browser.tabs.remote.autostart=true --setpref=layers.acceleration.disabled=true
GPU_RENDERING =
else
OOP_CONTENT = --setpref=layers.async-pan-zoom.enabled=true --setpref=browser.tabs.remote.autostart=true
GPU_RENDERING = --setpref=layers.acceleration.force-enabled=true
endif #}

jstestbrowser:
	$(MAKE) -C $(DEPTH)/config
	$(MAKE) stage-jstests
	$(CHECK_TEST_ERROR)

GARBAGE += $(addsuffix .log,$(MOCHITESTS) jstestbrowser)

REMOTE_CPPUNITTESTS = \
	$(PYTHON) -u $(topsrcdir)/testing/remotecppunittests.py \
	  --xre-path=$(DEPTH)/dist/bin \
	  --localLib=$(DEPTH)/dist/fennec \
	  --dm_trans=$(DM_TRANS) \
	  --deviceIP=${TEST_DEVICE} \
	  $(TEST_PATH) $(EXTRA_TEST_ARGS)

# Usage: |make [TEST_PATH=...] [EXTRA_TEST_ARGS=...] cppunittests-remote|.
cppunittests-remote: DM_TRANS?=adb
cppunittests-remote:
	@if [ '${TEST_DEVICE}' != '' -o '$(DM_TRANS)' = 'adb' ]; \
          then $(call REMOTE_CPPUNITTESTS); \
        else \
          echo 'please prepare your host with environment variables for TEST_DEVICE'; \
        fi

jetpack-tests:
	cd $(topsrcdir)/addon-sdk/source && $(PYTHON) bin/cfx -b $(abspath $(browser_path)) --parseable testpkgs

pgo-profile-run:
	$(PYTHON) $(topsrcdir)/build/pgo/profileserver.py $(EXTRA_TEST_ARGS)

# Package up the tests and test harnesses
include $(topsrcdir)/toolkit/mozapps/installer/package-name.mk

PKG_STAGE = $(DIST)/test-stage

stage-all: \
  stage-config \
  stage-mach \
  stage-extensions \
  stage-mochitest \
  stage-jstests \
  stage-jetpack \
  $(NULL)
ifdef MOZ_WEBRTC
stage-all: stage-steeplechase
endif

ifdef COMPILE_ENVIRONMENT
stage-all: stage-cppunittests
endif

TEST_PKGS := \
  common \
  cppunittest \
  mochitest \
  xpcshell \
  $(NULL)

ifdef BUILD_GTEST
stage-all: stage-gtest
TEST_PKGS += gtest
endif

PKG_ARG = --$(1) '$(PKG_BASENAME).$(1).tests.zip'

package-tests-prepare-dest:
	@rm -f '$(DIST)/$(PKG_PATH)$(TEST_PACKAGE)'
	$(NSINSTALL) -D $(DIST)/$(PKG_PATH)

define package_archive
package-tests-$(1): stage-all package-tests-prepare-dest
	$$(call py_action,test_archive, \
		$(1) \
		'$$(abspath $$(DIST))/$$(PKG_PATH)/$$(PKG_BASENAME).$(1).tests.zip')
package-tests: package-tests-$(1)
endef

$(foreach name,$(TEST_PKGS),$(eval $(call package_archive,$(name))))

ifeq ($(MOZ_BUILD_APP),mobile/android)
stage-all: stage-android
stage-all: stage-instrumentation-tests
endif

ifeq ($(MOZ_WIDGET_TOOLKIT),gonk)
stage-all: stage-b2g
endif

# Prepare _tests before any of the other staging/packaging steps.
# make-stage-dir is a prerequisite to all the stage-* targets in testsuite-targets.mk.
make-stage-dir: install-test-files
	rm -rf $(PKG_STAGE)
	$(NSINSTALL) -D $(PKG_STAGE)
	$(NSINSTALL) -D $(PKG_STAGE)/bin
	$(NSINSTALL) -D $(PKG_STAGE)/bin/components
	$(NSINSTALL) -D $(PKG_STAGE)/certs
	$(NSINSTALL) -D $(PKG_STAGE)/config
	$(NSINSTALL) -D $(PKG_STAGE)/jetpack
	$(NSINSTALL) -D $(PKG_STAGE)/modules
	$(NSINSTALL) -D $(PKG_STAGE)/tools/mach

stage-b2g: make-stage-dir
	$(NSINSTALL) $(topsrcdir)/b2g/test/b2g-unittest-requirements.txt $(PKG_STAGE)/b2g

stage-config: make-stage-dir
	$(NSINSTALL) -D $(PKG_STAGE)/config
	@(cd $(topsrcdir)/testing/config && tar $(TAR_CREATE_FLAGS) - *) | (cd $(PKG_STAGE)/config && tar -xf -)

stage-mach: make-stage-dir
	@(cd $(topsrcdir)/python/mach && tar $(TAR_CREATE_FLAGS) - *) | (cd $(PKG_STAGE)/tools/mach && tar -xf -)
	cp $(topsrcdir)/testing/tools/mach_test_package_bootstrap.py $(PKG_STAGE)/tools/mach_bootstrap.py
	cp $(topsrcdir)/mach $(PKG_STAGE)

stage-mochitest: make-stage-dir
ifeq ($(MOZ_BUILD_APP),mobile/android)
	$(MAKE) -C $(DEPTH)/testing/mochitest stage-package
endif

stage-jstests: make-stage-dir

stage-gtest: make-stage-dir
# FIXME: (bug 1200311) We should be generating the gtest xul as part of the build.
	$(MAKE) -C $(DEPTH)/testing/gtest gtest
	$(NSINSTALL) -D $(PKG_STAGE)/gtest/gtest_bin
	cp -RL $(DIST)/bin/gtest $(PKG_STAGE)/gtest/gtest_bin
	cp -RL $(DEPTH)/_tests/gtest $(PKG_STAGE)
	cp $(topsrcdir)/testing/gtest/rungtests.py $(PKG_STAGE)/gtest
	cp $(DIST)/bin/dependentlibs.list.gtest $(PKG_STAGE)/gtest
	cp $(DEPTH)/mozinfo.json $(PKG_STAGE)/gtest

stage-android: make-stage-dir
	$(NSINSTALL) $(topsrcdir)/mobile/android/fonts $(DEPTH)/_tests/testing/mochitest

stage-jetpack: make-stage-dir
	$(MAKE) -C $(DEPTH)/addon-sdk stage-tests-package

CPP_UNIT_TEST_BINS=$(wildcard $(DIST)/cppunittests/*)

ifdef OBJCOPY
ifneq ($(OBJCOPY), :) # see build/autoconf/toolchain.m4:102 for why this is necessary
ifndef PKG_SKIP_STRIP
STRIP_CPP_TESTS := 1
endif
endif
endif

stage-cppunittests: make-stage-dir
	$(NSINSTALL) -D $(PKG_STAGE)/cppunittest
ifdef STRIP_CPP_TESTS
	$(foreach bin,$(CPP_UNIT_TEST_BINS),$(OBJCOPY) $(or $(STRIP_FLAGS),--strip-unneeded) $(bin) $(bin:$(DIST)/cppunittests/%=$(PKG_STAGE)/cppunittest/%);)
else
	cp -RL $(CPP_UNIT_TEST_BINS) $(PKG_STAGE)/cppunittest
endif

stage-steeplechase: make-stage-dir
	$(NSINSTALL) -D $(PKG_STAGE)/steeplechase/
	cp -RL $(DEPTH)/_tests/steeplechase $(PKG_STAGE)/steeplechase/tests
	cp -RL $(DIST)/xpi-stage/specialpowers $(PKG_STAGE)/steeplechase
	cp -RL $(topsrcdir)/testing/profiles/prefs_general.js $(PKG_STAGE)/steeplechase

stage-instrumentation-tests: make-stage-dir
	$(MAKE) -C $(DEPTH)/testing/instrumentation stage-package

TEST_EXTENSIONS := \
    specialpowers@mozilla.org.xpi \
	$(NULL)

stage-extensions: make-stage-dir
	$(NSINSTALL) -D $(PKG_STAGE)/extensions/
	@$(foreach ext,$(TEST_EXTENSIONS), cp -RL $(DIST)/xpi-stage/$(ext) $(PKG_STAGE)/extensions;)


check::
	@$(topsrcdir)/mach --log-no-times python-test


.PHONY: \
  xpcshell-tests \
  jstestbrowser \
  package-tests \
  package-tests-prepare-dest \
  package-tests-common \
  make-stage-dir \
  stage-all \
  stage-b2g \
  stage-config \
  stage-mochitest \
  stage-jstests \
  stage-android \
  stage-jetpack \
  stage-steeplechase \
  stage-instrumentation-tests \
  check \
  $(NULL)

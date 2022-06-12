# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import absolute_import, unicode_literals

import getpass
import json
import logging
import os
import platform
import subprocess
import sys
import time
import which

from collections import (
    namedtuple,
    OrderedDict,
)

try:
    import psutil
except Exception:
    psutil = None

import mozpack.path as mozpath

from ..base import MozbuildObject

from ..testing import install_test_files

from ..compilation.warnings import (
    WarningsCollector,
    WarningsDatabase,
)

from textwrap import TextWrapper

INSTALL_TESTS_CLOBBER = ''.join([TextWrapper().fill(line) + '\n' for line in
'''
The build system was unable to install tests because the CLOBBER file has \
been updated. This means if you edited any test files, your changes may not \
be picked up until a full/clobber build is performed.

The easiest and fastest way to perform a clobber build is to run:

 $ mach clobber
 $ mach build

If you did not modify any test files, it is safe to ignore this message \
and proceed with running tests. To do this run:

 $ touch {clobber_file}
'''.splitlines()])



BuildOutputResult = namedtuple('BuildOutputResult',
    ('warning', 'state_changed', 'for_display'))


class TierStatus(object):
    """Represents the state and progress of tier traversal.

    The build system is organized into linear phases called tiers. Each tier
    executes in the order it was defined, 1 at a time.
    """

    def __init__(self):
        self.tiers = OrderedDict()
        self.tier_status = OrderedDict()

    def set_tiers(self, tiers):
        """Record the set of known tiers."""
        for tier in tiers:
            self.tiers[tier] = dict(
                begin_time=None,
                finish_time=None,
                duration=None,
            )
            self.tier_status[tier] = None

    def begin_tier(self, tier):
        """Record that execution of a tier has begun."""
        self.tier_status[tier] = 'active'
        t = self.tiers[tier]
        # We should ideally use a monotonic clock here. Unfortunately, we won't
        # have one until Python 3.
        t['begin_time'] = time.time()

    def finish_tier(self, tier):
        """Record that execution of a tier has finished."""
        self.tier_status[tier] = 'finished'
        t = self.tiers[tier]
        t['finish_time'] = time.time()


class BuildMonitor(MozbuildObject):
    """Monitors the output of the build."""

    def init(self, warnings_path):
        """Create a new monitor.

        warnings_path is a path of a warnings database to use.
        """
        self._warnings_path = warnings_path

        self.tiers = TierStatus()

        self.warnings_database = WarningsDatabase()
        if os.path.exists(warnings_path):
            try:
                self.warnings_database.load_from_file(warnings_path)
            except ValueError:
                os.remove(warnings_path)

        self._warnings_collector = WarningsCollector(
            database=self.warnings_database, objdir=self.topobjdir)

        self.build_objects = []

    def start(self):
        """Record the start of the build."""
        self.start_time = time.time()

    def on_line(self, line):
        """Consume a line of output from the build system.

        This will parse the line for state and determine whether more action is
        needed.

        Returns a BuildOutputResult instance.

        In this named tuple, warning will be an object describing a new parsed
        warning. Otherwise it will be None.

        state_changed indicates whether the build system changed state with
        this line. If the build system changed state, the caller may want to
        query this instance for the current state in order to update UI, etc.

        for_display is a boolean indicating whether the line is relevant to the
        user. This is typically used to filter whether the line should be
        presented to the user.
        """
        if line.startswith('BUILDSTATUS'):
            args = line.split()[1:]

            action = args.pop(0)
            update_needed = True

            if action == 'TIERS':
                self.tiers.set_tiers(args)
                update_needed = False
            elif action == 'TIER_START':
                tier = args[0]
                self.tiers.begin_tier(tier)
            elif action == 'TIER_FINISH':
                tier, = args
                self.tiers.finish_tier(tier)
            elif action == 'OBJECT_FILE':
                self.build_objects.append(args[0])
                update_needed = False
            else:
                raise Exception('Unknown build status: %s' % action)

            return BuildOutputResult(None, update_needed, False)

        warning = None

        try:
            warning = self._warnings_collector.process_line(line)
        except:
            pass

        return BuildOutputResult(warning, False, True)

    def finish(self, record_usage=True):
        """Record the end of the build."""
        self.end_time = time.time()
        self.elapsed = self.end_time - self.start_time

        self.warnings_database.prune()
        self.warnings_database.save_to_file(self._warnings_path)


class BuildDriver(MozbuildObject):
    """Provides a high-level API for build actions."""

    def install_tests(self, test_objs):
        """Install test files."""

        if self.is_clobber_needed():
            print(INSTALL_TESTS_CLOBBER.format(
                  clobber_file=os.path.join(self.topobjdir, 'CLOBBER')))
            sys.exit(1)

        if not test_objs:
            # If we don't actually have a list of tests to install we install
            # test and support files wholesale.
            self._run_make(target='install-test-files', pass_thru=True,
                           print_directory=False)
        else:
            install_test_files(mozpath.normpath(self.topsrcdir), self.topobjdir,
                               '_tests', test_objs)

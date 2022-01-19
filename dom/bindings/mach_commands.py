# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, # You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import absolute_import, unicode_literals

import os
import sys

from mach.decorators import (
    CommandArgument,
    CommandProvider,
    Command,
)

from mozbuild.base import MachCommandBase

@CommandProvider
class WebIDLProvider(MachCommandBase):
    @Command('webidl-example', category='misc',
             description='Generate example files for a WebIDL interface.')
    @CommandArgument('interface', nargs='+',
                     help='Interface(s) whose examples to generate.')
    def webidl_example(self, interface):
        from mozwebidlcodegen import BuildSystemWebIDL

        manager = self._spawn(BuildSystemWebIDL).manager
        for i in interface:
            manager.generate_example_files(i)

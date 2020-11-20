# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import absolute_import, print_function, unicode_literals

import os

from mozbuild.base import (
    MachCommandBase,
)


from mach.decorators import (
    CommandProvider,
    Command,
)


here = os.path.abspath(os.path.dirname(__file__))


def setup_argument_parser():
    from mozlint import cli
    return cli.MozlintParser()


@CommandProvider
class MachCommands(MachCommandBase):

    @Command(
        'lint', category='devenv',
        description='Run linters.',
        parser=setup_argument_parser)
    def lint(self, *runargs, **lintargs):
        """Run linters."""
        from mozlint import cli
        lintargs['exclude'] = ['obj*']
        cli.SEARCH_PATHS.append(here)
        self._activate_virtualenv()
        return cli.run(*runargs, **lintargs)

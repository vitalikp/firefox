# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# This action is used to produce test archives.
#
# Ideally, the data in this file should be defined in moz.build files.
# It is defined inline because this was easiest to make test archive
# generation faster.

from __future__ import absolute_import, print_function, unicode_literals

import argparse
import itertools
import os
import sys
import time

from manifestparser import TestManifest

from mozbuild.util import ensureParentDir
from mozpack.files import FileFinder
from mozpack.mozjar import JarWriter
import mozpack.path as mozpath

import buildconfig

STAGE = mozpath.join(buildconfig.topobjdir, 'dist', 'test-stage')

TEST_HARNESS_BINS = [
    'BadCertServer',
    'GenerateOCSPResponse',
    'OCSPStaplingServer',
    'SmokeDMD',
    'certutil',
    'crashinject',
    'fileid',
    'minidumpwriter',
    'pk12util',
    'screenshot',
    'screentopng',
    'ssltunnel',
    'xpcshell',
]

# The fileid utility depends on mozglue. See bug 1069556.
TEST_HARNESS_DLLS = [
    'crashinjectdll',
    'mozglue'
]

TEST_PLUGIN_DLLS = [
    'npctrltest',
    'npsecondtest',
    'npswftest',
    'nptest',
    'nptestjava',
    'npthirdtest',
]

TEST_PLUGIN_DIRS = [
    'JavaTest.plugin/**',
    'SecondTest.plugin/**',
    'Test.plugin/**',
    'ThirdTest.plugin/**',
    'npctrltest.plugin/**',
    'npswftest.plugin/**',
]


ARCHIVE_FILES = {
    'common': [
        {
            'source': STAGE,
            'base': '',
            'pattern': '**',
            'ignore': [
                'cppunittest/**',
                'gtest/**',
                'mochitest/**',
                'xpcshell/**',
            ],
        },
        {
            'source': buildconfig.topobjdir,
            'base': '_tests',
            'pattern': 'modules/**',
        },
        {
            'source': buildconfig.topobjdir,
            'base': '_tests',
            'pattern': 'mozbase/**',
        },
        {
            'source': buildconfig.topsrcdir,
            'base': 'testing',
            'pattern': 'firefox-ui/**',
        },
        {
            'source': buildconfig.topsrcdir,
            'base': 'dom/media/test/external',
            'pattern': '**',
            'dest': 'external-media-tests',
        },
        {
            'source': buildconfig.topsrcdir,
            'base': 'testing',
            'pattern': 'tps/**',
        },
        {
            'source': buildconfig.topsrcdir,
            'base': 'services/sync/',
            'pattern': 'tps/**',
        },
        {
            'source': buildconfig.topobjdir,
            'base': '',
            'pattern': 'mozinfo.json',
        },
        {
            'source': buildconfig.topobjdir,
            'base': 'dist/bin',
            'patterns': [
                '%s%s' % (f, buildconfig.substs['BIN_SUFFIX'])
                for f in TEST_HARNESS_BINS
            ] + [
                '%s%s%s' % (buildconfig.substs['DLL_PREFIX'], f, buildconfig.substs['DLL_SUFFIX'])
                for f in TEST_HARNESS_DLLS
            ],
            'dest': 'bin',
        },
        {
            'source': buildconfig.topobjdir,
            'base': 'dist/plugins',
            'patterns': [
                '%s%s%s' % (buildconfig.substs['DLL_PREFIX'], f, buildconfig.substs['DLL_SUFFIX'])
                for f in TEST_PLUGIN_DLLS
            ],
            'dest': 'bin/plugins',
        },
        {
            'source': buildconfig.topobjdir,
            'base': 'dist/plugins',
            'patterns': TEST_PLUGIN_DIRS,
            'dest': 'bin/plugins',
        },
        {
            'source': buildconfig.topobjdir,
            'base': 'dist/bin',
            'patterns': [
                'dmd.py',
                'fix_linux_stack.py',
                'fix_macosx_stack.py',
                'fix_stack_using_bpsyms.py',
            ],
            'dest': 'bin',
        },
        {
            'source': buildconfig.topobjdir,
            'base': 'dist/bin/components',
            'patterns': [
                'httpd.js',
                'httpd.manifest',
                'test_necko.xpt',
            ],
            'dest': 'bin/components',
        },
        {
            'source': buildconfig.topsrcdir,
            'base': 'build/pgo/certs',
            'pattern': '**',
            'dest': 'certs',
        }
    ],
    'cppunittest': [
        {
            'source': STAGE,
            'base': '',
            'pattern': 'cppunittest/**',
        },
        {
            'source': buildconfig.topsrcdir,
            'base': 'testing',
            'pattern': 'runcppunittests.py',
            'dest': 'cppunittest',
        },
        {
            'source': buildconfig.topsrcdir,
            'base': 'testing',
            'pattern': 'remotecppunittests.py',
            'dest': 'cppunittest',
        },
        {
            'source': buildconfig.topsrcdir,
            'base': 'testing',
            'pattern': 'cppunittest.ini',
            'dest': 'cppunittest',
        },
        {
            'source': buildconfig.topobjdir,
            'base': '',
            'pattern': 'mozinfo.json',
            'dest': 'cppunittest',
        },
    ],
    'gtest': [
        {
            'source': STAGE,
            'base': '',
            'pattern': 'gtest/**',
        },
    ],
    'mochitest': [
        {
            'source': buildconfig.topobjdir,
            'base': '_tests/testing',
            'pattern': 'mochitest/**',
        },
        {
            'source': STAGE,
            'base': '',
            'pattern': 'mochitest/**',
        },
        {
            'source': buildconfig.topobjdir,
            'base': '',
            'pattern': 'mozinfo.json',
            'dest': 'mochitest'
        }
    ],
}


# "common" is our catch all archive and it ignores things from other archives.
# Verify nothing sneaks into ARCHIVE_FILES without a corresponding exclusion
# rule in the "common" archive.
for k, v in ARCHIVE_FILES.items():
    if k in ('common'):
        continue

    ignores = set(itertools.chain(*(e.get('ignore', [])
                                  for e in ARCHIVE_FILES['common'])))

    if not any(p.startswith('%s/' % k) for p in ignores):
        raise Exception('"common" ignore list probably should contain %s' % k)


def find_files(archive):
    for entry in ARCHIVE_FILES[archive]:
        source = entry['source']
        dest = entry.get('dest')
        base = entry.get('base', '')

        pattern = entry.get('pattern')
        patterns = entry.get('patterns', [])
        if pattern:
            patterns.append(pattern)

        manifest = entry.get('manifest')
        manifests = entry.get('manifests', [])
        if manifest:
            manifests.append(manifest)
        if manifests:
            dirs = find_manifest_dirs(buildconfig.topsrcdir, manifests)
            patterns.extend({'{}/**'.format(d) for d in dirs})

        ignore = list(entry.get('ignore', []))
        ignore.extend([
            '**/.flake8',
            '**/.mkdir.done',
            '**/*.pyc',
        ])

        common_kwargs = {
            'find_executables': False,
            'find_dotfiles': True,
            'ignore': ignore,
        }

        finder = FileFinder(os.path.join(source, base), **common_kwargs)

        for pattern in patterns:
            for p, f in finder.find(pattern):
                if dest:
                    p = mozpath.join(dest, p)
                yield p, f


def find_manifest_dirs(topsrcdir, manifests):
    """Routine to retrieve directories specified in a manifest, relative to topsrcdir.

    It does not recurse into manifests, as we currently have no need for that.
    """
    dirs = set()

    for p in manifests:
        p = os.path.join(topsrcdir, p)

        if p.endswith('.ini'):
            test_manifest = TestManifest()
            test_manifest.read(p)
            dirs |= set([os.path.dirname(m) for m in test_manifest.manifests()])

        else:
            raise Exception('"{}" is not a supported manifest format.'.format(
                os.path.splitext(p)[1]))

    dirs = {mozpath.normpath(d[len(topsrcdir):]).lstrip('/') for d in dirs}

    # Filter out children captured by parent directories because duplicates
    # will confuse things later on.
    def parents(p):
        while True:
            p = mozpath.dirname(p)
            if not p:
                break
            yield p

    seen = set()
    for d in sorted(dirs, key=len):
        if not any(p in seen for p in parents(d)):
            seen.add(d)

    return sorted(seen)


def main(argv):
    parser = argparse.ArgumentParser(
        description='Produce test archives')
    parser.add_argument('archive', help='Which archive to generate')
    parser.add_argument('outputfile', help='File to write output to')

    args = parser.parse_args(argv)

    if not args.outputfile.endswith('.zip'):
        raise Exception('expected zip output file')

    file_count = 0
    t_start = time.time()
    ensureParentDir(args.outputfile)
    with open(args.outputfile, 'wb') as fh:
        # Experimentation revealed that level 5 is significantly faster and has
        # marginally larger sizes than higher values and is the sweet spot
        # for optimal compression. Read the detailed commit message that
        # introduced this for raw numbers.
        with JarWriter(fileobj=fh, optimize=False, compress_level=5) as writer:
            res = find_files(args.archive)
            for p, f in res:
                writer.add(p.encode('utf-8'), f.read(), mode=f.mode, skip_duplicates=True)
                file_count += 1

    duration = time.time() - t_start
    zip_size = os.path.getsize(args.outputfile)
    basename = os.path.basename(args.outputfile)
    print('Wrote %d files in %d bytes to %s in %.2fs' % (
          file_count, zip_size, basename, duration))


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

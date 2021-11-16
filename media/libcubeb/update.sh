# Usage: sh update.sh <upstream_src_directory>
set -e

cp $1/AUTHORS .
cp $1/LICENSE .
cp $1/README.md .
cp $1/include/cubeb/cubeb.h include
cp $1/src/android/audiotrack_definitions.h src/android
cp $1/src/android/sles_definitions.h src/android
cp $1/src/cubeb-internal.h src
cp $1/src/cubeb-speex-resampler.h src
cp $1/src/cubeb.c src
cp $1/src/cubeb_alsa.c src
cp $1/src/cubeb_log.h src
cp $1/src/cubeb_audiotrack.c src
cp $1/src/cubeb_audiounit.cpp src
cp $1/src/cubeb_osx_run_loop.h src
cp $1/src/cubeb_jack.cpp src
cp $1/src/cubeb_opensl.c src
cp $1/src/cubeb_panner.cpp src
cp $1/src/cubeb_panner.h src
cp $1/src/cubeb_resampler.cpp src
cp $1/src/cubeb_resampler.h src
cp $1/src/cubeb_resampler_internal.h src
cp $1/src/cubeb_ring_array.h src
cp $1/src/cubeb_sndio.c src
cp $1/src/cubeb_utils.h src
cp $1/src/cubeb_utils_unix.h src
cp $1/src/cubeb_utils_win.h src
cp $1/src/cubeb_wasapi.cpp src
cp $1/src/cubeb_winmm.c src

if [ -d $1/.git ]; then
  rev=$(cd $1 && git rev-parse --verify HEAD)
  dirty=$(cd $1 && git diff-index --name-only HEAD)
fi

if [ -n "$rev" ]; then
  version=$rev
  if [ -n "$dirty" ]; then
    version=$version-dirty
    echo "WARNING: updating from a dirty git repository."
  fi
  sed -i.bak -e "/The git commit ID used was/ s/[0-9a-f]\{40\}\(-dirty\)\{0,1\}\./$version./" README_MOZILLA
  rm README_MOZILLA.bak
else
  echo "Remember to update README_MOZILLA with the version details."
fi

echo "Applying a patch on top of $version"
patch -p1 < ./unresampled-frames.patch

echo "Applying a patch on top of $version"
patch -p1 < ./bug1302231_emergency_bailout.patch

echo "Applying a patch on top of $version"
patch -p1 < ./osx-linearize-operations.patch

echo "Applying a patch on top of $version"
patch -p1 < ./prevent-double-free.patch

echo "Applying a patch on top of $version"
patch -p1 < ./uplift-wasapi-part-to-beta.patch

echo "Applying a patch on top of $version"
patch -p3 < ./fix-crashes.patch

echo "Applying a patch on top of $version"
patch -p3 < ./uplift-part-of-f07ee6d-esr52.patch

echo "Applying a patch on top of $version"
patch -p3 < ./uplift-system-listener-patch.patch

echo "Applying a patch on top of $version"
patch -p1 < ./uplift-patch-7a4c711.patch

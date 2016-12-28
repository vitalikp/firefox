/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef WEBRTC_SOUND_LINUXSOUNDSYSTEM_H_
#define WEBRTC_SOUND_LINUXSOUNDSYSTEM_H_

#include "webrtc/base/arraysize.h"
#include "webrtc/sound/automaticallychosensoundsystem.h"

namespace rtc {

extern const SoundSystemCreator kLinuxSoundSystemCreators[
    1
    ];

typedef AutomaticallyChosenSoundSystem<
    kLinuxSoundSystemCreators,
    arraysize(kLinuxSoundSystemCreators)> LinuxSoundSystem;

}  // namespace rtc

#endif  // WEBRTC_SOUND_LINUXSOUNDSYSTEM_H_

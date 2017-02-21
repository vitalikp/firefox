/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FFmpegDecoderModule.h"
#include "FFmpegAudioDecoder.h"
#include "FFmpegVideoDecoder.h"
#include "MediaPrefs.h"

namespace mozilla {

bool FFmpegDecoderModule::sInitialized = false;

/* static */
void
FFmpegDecoderModule::Init()
{
  if (sInitialized) {
    return;
  }

  avcodec_register_all();
  sInitialized = true;
}

already_AddRefed<MediaDataDecoder>
FFmpegDecoderModule::CreateVideoDecoder(const CreateDecoderParams& aParams)
{
  // Temporary - forces use of VPXDecoder when alpha is present.
  // Bug 1263836 will handle alpha scenario once implemented. It will shift
  // the check for alpha to PDMFactory but not itself remove the need for a
  // check.
  if (aParams.VideoConfig().HasAlpha()) {
    return nullptr;
  }
  if (aParams.mOptions.contains(
        CreateDecoderParams::Option::LowLatency) &&
      !MediaPrefs::PDMFFVPXLowLatencyEnabled()) {
    return nullptr;
  }
  RefPtr<MediaDataDecoder> decoder = new FFmpegVideoDecoder(
    aParams.mTaskQueue,
    aParams.VideoConfig(),
    aParams.mImageContainer,
    aParams.mOptions.contains(CreateDecoderParams::Option::LowLatency));
  return decoder.forget();
}

already_AddRefed<MediaDataDecoder>
FFmpegDecoderModule::CreateAudioDecoder(const CreateDecoderParams& aParams)
{
  RefPtr<MediaDataDecoder> decoder =
    new FFmpegAudioDecoder(aParams.mTaskQueue,
                           aParams.AudioConfig());
  return decoder.forget();
}

bool FFmpegDecoderModule::SupportsMimeType(const nsACString& aMimeType,
                      DecoderDoctorDiagnostics* aDiagnostics) const
{
  AVCodecID videoCodec = FFmpegVideoDecoder::GetCodecId(aMimeType);
  AVCodecID audioCodec = FFmpegAudioDecoder::GetCodecId(aMimeType);
  if (audioCodec == AV_CODEC_ID_NONE && videoCodec == AV_CODEC_ID_NONE) {
    return false;
  }
  AVCodecID codec = audioCodec != AV_CODEC_ID_NONE ? audioCodec : videoCodec;
  return !!FFmpegDataDecoder::FindAVCodec(codec);
}
} // namespace mozilla

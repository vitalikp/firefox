/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/SyncRunnable.h"
#include "mozilla/TaskQueue.h"

#include <string.h>
#ifdef __GNUC__
#include <unistd.h>
#endif

#include "FFmpegLog.h"
#include "FFmpegDataDecoder.h"
#include "prsystem.h"

namespace mozilla
{

StaticMutex FFmpegDataDecoder::sMonitor;

FFmpegDataDecoder::FFmpegDataDecoder(TaskQueue* aTaskQueue,
                                     AVCodecID aCodecID)
  : mCodecContext(nullptr)
  , mFrame(NULL)
  , mExtraData(nullptr)
  , mCodecID(aCodecID)
  , mTaskQueue(aTaskQueue)
{
  MOZ_ASSERT(aLib);
  MOZ_COUNT_CTOR(FFmpegDataDecoder);
}

FFmpegDataDecoder::~FFmpegDataDecoder()
{
  MOZ_COUNT_DTOR(FFmpegDataDecoder);
}

nsresult
FFmpegDataDecoder::InitDecoder()
{
  FFMPEG_LOG("Initialising FFmpeg decoder.");

  AVCodec* codec = FindAVCodec(mCodecID);
  if (!codec) {
    NS_WARNING("Couldn't find ffmpeg decoder");
    return NS_ERROR_FAILURE;
  }

  StaticMutexAutoLock mon(sMonitor);

  if (!(mCodecContext = avcodec_alloc_context3(codec))) {
    NS_WARNING("Couldn't init ffmpeg context");
    return NS_ERROR_FAILURE;
  }

  mCodecContext->opaque = this;

  InitCodecContext();

  if (mExtraData) {
    mCodecContext->extradata_size = mExtraData->Length();
    // FFmpeg may use SIMD instructions to access the data which reads the
    // data in 32 bytes block. Must ensure we have enough data to read.
    mExtraData->AppendElements(FF_INPUT_BUFFER_PADDING_SIZE);
    mCodecContext->extradata = mExtraData->Elements();
  } else {
    mCodecContext->extradata_size = 0;
  }

  if (codec->capabilities & CODEC_CAP_DR1) {
    mCodecContext->flags |= CODEC_FLAG_EMU_EDGE;
  }

  if (avcodec_open2(mCodecContext, codec, nullptr) < 0) {
    NS_WARNING("Couldn't initialise ffmpeg decoder");
    avcodec_close(mCodecContext);
    av_freep(&mCodecContext);
    return NS_ERROR_FAILURE;
  }

  FFMPEG_LOG("FFmpeg init successful.");
  return NS_OK;
}

RefPtr<ShutdownPromise>
FFmpegDataDecoder::Shutdown()
{
  if (mTaskQueue) {
    RefPtr<FFmpegDataDecoder> self = this;
    return InvokeAsync(mTaskQueue, __func__, [self, this]() {
      ProcessShutdown();
      return ShutdownPromise::CreateAndResolve(true, __func__);
    });
  }
  ProcessShutdown();
  return ShutdownPromise::CreateAndResolve(true, __func__);
}

RefPtr<MediaDataDecoder::DecodePromise>
FFmpegDataDecoder::Decode(MediaRawData* aSample)
{
  return InvokeAsync<MediaRawData*>(mTaskQueue, this, __func__,
                                    &FFmpegDataDecoder::ProcessDecode, aSample);
}

RefPtr<MediaDataDecoder::FlushPromise>
FFmpegDataDecoder::Flush()
{
  return InvokeAsync(mTaskQueue, this, __func__,
                     &FFmpegDataDecoder::ProcessFlush);
}

RefPtr<MediaDataDecoder::DecodePromise>
FFmpegDataDecoder::Drain()
{
  return InvokeAsync(mTaskQueue, this, __func__,
                     &FFmpegDataDecoder::ProcessDrain);
}

RefPtr<MediaDataDecoder::FlushPromise>
FFmpegDataDecoder::ProcessFlush()
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());
  if (mCodecContext) {
    avcodec_flush_buffers(mCodecContext);
  }
  return FlushPromise::CreateAndResolve(true, __func__);
}

void
FFmpegDataDecoder::ProcessShutdown()
{
  StaticMutexAutoLock mon(sMonitor);

  if (mCodecContext) {
    avcodec_close(mCodecContext);
    av_freep(&mCodecContext);
#if LIBAVCODEC_VERSION_MAJOR >= 55
    av_frame_free(&mFrame);
#else
    av_freep(&mFrame);
#endif
  }
}

AVFrame*
FFmpegDataDecoder::PrepareFrame()
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());

  if (mFrame) {
    av_frame_unref(mFrame);
  } else {
    mFrame = av_frame_alloc();
  }

  return mFrame;
}

/* static */ AVCodec*
FFmpegDataDecoder::FindAVCodec(AVCodecID aCodec)
{
  return avcodec_find_decoder(aCodec);
}

} // namespace mozilla

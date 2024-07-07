/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DrawTargetSkia.h"
#include "SourceSurfaceSkia.h"
#include "ScaledFontBase.h"
#include "ScaledFontCairo.h"
#include "skia/include/core/SkBitmapDevice.h"
#include "FilterNodeSoftware.h"
#include "HelpersSkia.h"

#include "mozilla/ArrayUtils.h"

#include "skia/include/core/SkSurface.h"
#include "skia/include/core/SkTypeface.h"
#include "skia/include/effects/SkGradientShader.h"
#include "skia/include/core/SkColorFilter.h"
#include "skia/include/effects/SkBlurImageFilter.h"
#include "skia/include/effects/SkLayerRasterizer.h"
#include "skia/src/core/SkSpecialImage.h"
#include "Blur.h"
#include "Logging.h"
#include "Tools.h"
#include "DataSurfaceHelpers.h"
#include "PathHelpers.h"
#include <algorithm>

#ifdef USE_SKIA_GPU
#include "GLDefs.h"
#include "skia/include/gpu/SkGr.h"
#include "skia/include/gpu/GrContext.h"
#include "skia/include/gpu/GrDrawContext.h"
#include "skia/include/gpu/gl/GrGLInterface.h"
#include "skia/src/image/SkImage_Gpu.h"
#endif

#ifdef XP_WIN
#include "ScaledFontDWrite.h"
#endif

namespace mozilla {
namespace gfx {

class GradientStopsSkia : public GradientStops
{
public:
  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(GradientStopsSkia)
  GradientStopsSkia(const std::vector<GradientStop>& aStops, uint32_t aNumStops, ExtendMode aExtendMode)
    : mCount(aNumStops)
    , mExtendMode(aExtendMode)
  {
    if (mCount == 0) {
      return;
    }

    // Skia gradients always require a stop at 0.0 and 1.0, insert these if
    // we don't have them.
    uint32_t shift = 0;
    if (aStops[0].offset != 0) {
      mCount++;
      shift = 1;
    }
    if (aStops[aNumStops-1].offset != 1) {
      mCount++;
    }
    mColors.resize(mCount);
    mPositions.resize(mCount);
    if (aStops[0].offset != 0) {
      mColors[0] = ColorToSkColor(aStops[0].color, 1.0);
      mPositions[0] = 0;
    }
    for (uint32_t i = 0; i < aNumStops; i++) {
      mColors[i + shift] = ColorToSkColor(aStops[i].color, 1.0);
      mPositions[i + shift] = SkFloatToScalar(aStops[i].offset);
    }
    if (aStops[aNumStops-1].offset != 1) {
      mColors[mCount-1] = ColorToSkColor(aStops[aNumStops-1].color, 1.0);
      mPositions[mCount-1] = SK_Scalar1;
    }
  }

  BackendType GetBackendType() const { return BackendType::SKIA; }

  std::vector<SkColor> mColors;
  std::vector<SkScalar> mPositions;
  int mCount;
  ExtendMode mExtendMode;
};

/**
 * When constructing a temporary SkImage via GetSkImageForSurface, we may also
 * have to construct a temporary DataSourceSurface, which must live as long as
 * the SkImage. We attach this temporary surface to the image's pixelref, so
 * that it can be released once the pixelref is freed.
 */
static void
ReleaseTemporarySurface(const void* aPixels, void* aContext)
{
  DataSourceSurface* surf = static_cast<DataSourceSurface*>(aContext);
  if (surf) {
    surf->Release();
  }
}

#ifdef IS_BIG_ENDIAN
static const int kARGBAlphaOffset = 0;
#else
static const int kARGBAlphaOffset = 3;
#endif

static void
WriteRGBXFormat(uint8_t* aData, const IntSize &aSize,
                const int32_t aStride, SurfaceFormat aFormat)
{
  if (aFormat != SurfaceFormat::B8G8R8X8 || aSize.IsEmpty()) {
    return;
  }

  int height = aSize.height;
  int width = aSize.width * 4;

  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; column += 4) {
      aData[column + kARGBAlphaOffset] = 0xFF;
    }
    aData += aStride;
  }

  return;
}

#ifdef DEBUG
static bool
VerifyRGBXFormat(uint8_t* aData, const IntSize &aSize, const int32_t aStride, SurfaceFormat aFormat)
{
  if (aFormat != SurfaceFormat::B8G8R8X8 || aSize.IsEmpty()) {
    return true;
  }
  // We should've initialized the data to be opaque already
  // On debug builds, verify that this is actually true.
  int height = aSize.height;
  int width = aSize.width * 4;

  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; column += 4) {
      if (aData[column + kARGBAlphaOffset] != 0xFF) {
        gfxCriticalError() << "RGBX pixel at (" << column << "," << row << ") in "
                           << width << "x" << height << " surface is not opaque: "
                           << int(aData[column]) << ","
                           << int(aData[column+1]) << ","
                           << int(aData[column+2]) << ","
                           << int(aData[column+3]);
      }
    }
    aData += aStride;
  }

  return true;
}

// Since checking every pixel is expensive, this only checks the four corners and center
// of a surface that their alpha value is 0xFF.
static bool
VerifyRGBXCorners(uint8_t* aData, const IntSize &aSize, const int32_t aStride, SurfaceFormat aFormat)
{
  if (aFormat != SurfaceFormat::B8G8R8X8 || aSize.IsEmpty()) {
    return true;
  }

  int height = aSize.height;
  int width = aSize.width;
  const int pixelSize = 4;
  const int strideDiff = aStride - (width * pixelSize);
  MOZ_ASSERT(width * pixelSize <= aStride);

  const int topLeft = 0;
  const int topRight = width * pixelSize - pixelSize;
  const int bottomRight = aStride * height - strideDiff - pixelSize;
  const int bottomLeft = aStride * height - aStride;

  // Lastly the center pixel
  int middleRowHeight = height / 2;
  int middleRowWidth = (width / 2) * pixelSize;
  const int middle = aStride * middleRowHeight + middleRowWidth;

  const int offsets[] = { topLeft, topRight, bottomRight, bottomLeft, middle };
  for (int offset : offsets) {
    if (aData[offset + kARGBAlphaOffset] != 0xFF) {
        int row = offset / aStride;
        int column = (offset % aStride) / pixelSize;
        gfxCriticalError() << "RGBX corner pixel at (" << column << "," << row << ") in "
                           << width << "x" << height << " surface is not opaque: "
                           << int(aData[offset]) << ","
                           << int(aData[offset+1]) << ","
                           << int(aData[offset+2]) << ","
                           << int(aData[offset+3]);
    }
  }

  return true;
}
#endif

static sk_sp<SkImage>
GetSkImageForSurface(SourceSurface* aSurface)
{
  if (!aSurface) {
    gfxDebug() << "Creating null Skia image from null SourceSurface";
    return nullptr;
  }

  if (aSurface->GetType() == SurfaceType::SKIA) {
    return static_cast<SourceSurfaceSkia*>(aSurface)->GetImage();
  }

  DataSourceSurface* surf = aSurface->GetDataSurface().take();
  if (!surf) {
    gfxWarning() << "Failed getting DataSourceSurface for Skia image";
    return nullptr;
  }

  SkPixmap pixmap(MakeSkiaImageInfo(surf->GetSize(), surf->GetFormat()),
                  surf->GetData(), surf->Stride());
  sk_sp<SkImage> image = SkImage::MakeFromRaster(pixmap, ReleaseTemporarySurface, surf);
  if (!image) {
    ReleaseTemporarySurface(nullptr, surf);
    gfxDebug() << "Failed making Skia raster image for temporary surface";
  }

  // Skia doesn't support RGBX surfaces so ensure that the alpha value is opaque white.
  MOZ_ASSERT(VerifyRGBXCorners(surf->GetData(), surf->GetSize(),
                               surf->Stride(), surf->GetFormat()));
  return image;
}

DrawTargetSkia::DrawTargetSkia()
  : mSnapshot(nullptr)
{
}

DrawTargetSkia::~DrawTargetSkia()
{
}

already_AddRefed<SourceSurface>
DrawTargetSkia::Snapshot()
{
  RefPtr<SourceSurfaceSkia> snapshot = mSnapshot;
  if (mSurface && !snapshot) {
    snapshot = new SourceSurfaceSkia();
    sk_sp<SkImage> image;
    // If the surface is raster, making a snapshot may trigger a pixel copy.
    // Instead, try to directly make a raster image referencing the surface pixels.
    SkPixmap pixmap;
    if (mSurface->peekPixels(&pixmap)) {
      image = SkImage::MakeFromRaster(pixmap, nullptr, nullptr);
    } else {
      image = mSurface->makeImageSnapshot(SkBudgeted::kNo);
    }
    if (!snapshot->InitFromImage(image, mFormat, this)) {
      return nullptr;
    }
    mSnapshot = snapshot;
  }

  return snapshot.forget();
}

bool
DrawTargetSkia::LockBits(uint8_t** aData, IntSize* aSize,
                         int32_t* aStride, SurfaceFormat* aFormat,
                         IntPoint* aOrigin)
{
  // Ensure the layer is at the origin if required.
  SkIPoint origin = mCanvas->getTopDevice()->getOrigin();
  if (!aOrigin && !origin.isZero()) {
    return false;
  }

  /* Test if the canvas' device has accessible pixels first, as actually
   * accessing the pixels may trigger side-effects, even if it fails.
   */
  if (!mCanvas->peekPixels(nullptr)) {
    return false;
  }

  SkImageInfo info;
  size_t rowBytes;
  void* pixels = mCanvas->accessTopLayerPixels(&info, &rowBytes);
  if (!pixels) {
    return false;
  }

  MarkChanged();

  *aData = reinterpret_cast<uint8_t*>(pixels);
  *aSize = IntSize(info.width(), info.height());
  *aStride = int32_t(rowBytes);
  *aFormat = SkiaColorTypeToGfxFormat(info.colorType(), info.alphaType());
  if (aOrigin) {
    *aOrigin = IntPoint(origin.x(), origin.y());
  }
  return true;
}

void
DrawTargetSkia::ReleaseBits(uint8_t* aData)
{
}

static void
ReleaseImage(const void* aPixels, void* aContext)
{
  SkImage* image = static_cast<SkImage*>(aContext);
  SkSafeUnref(image);
}

static sk_sp<SkImage>
ExtractSubset(sk_sp<SkImage> aImage, const IntRect& aRect)
{
  SkIRect subsetRect = IntRectToSkIRect(aRect);
  if (aImage->bounds() == subsetRect) {
    return aImage;
  }
  // makeSubset is slow, so prefer to use SkPixmap::extractSubset where possible.
  SkPixmap pixmap, subsetPixmap;
  if (aImage->peekPixels(&pixmap) &&
      pixmap.extractSubset(&subsetPixmap, subsetRect)) {
    // Release the original image reference so only the subset image keeps it alive.
    return SkImage::MakeFromRaster(subsetPixmap, ReleaseImage, aImage.release());
  }
  return aImage->makeSubset(subsetRect);
}

static inline bool
SkImageIsMask(const sk_sp<SkImage>& aImage)
{
  SkPixmap pixmap;
  if (aImage->peekPixels(&pixmap)) {
    return pixmap.colorType() == kAlpha_8_SkColorType;
#ifdef USE_SKIA_GPU
  }
  if (GrTexture* tex = aImage->getTexture()) {
    return GrPixelConfigIsAlphaOnly(tex->config());
#endif
  }
  return false;
}

static bool
ExtractAlphaBitmap(const sk_sp<SkImage>& aImage, SkBitmap* aResultBitmap)
{
  SkImageInfo info = SkImageInfo::MakeA8(aImage->width(), aImage->height());
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info, SkAlign4(info.minRowBytes())) ||
      !aImage->readPixels(bitmap.info(), bitmap.getPixels(), bitmap.rowBytes(), 0, 0)) {
    gfxWarning() << "Failed reading alpha pixels for Skia bitmap";
    return false;
  }

  *aResultBitmap = bitmap;
  return true;
}

static sk_sp<SkImage>
ExtractAlphaForSurface(SourceSurface* aSurface)
{
  sk_sp<SkImage> image = GetSkImageForSurface(aSurface);
  if (!image) {
    return nullptr;
  }
  if (SkImageIsMask(image)) {
    return image;
  }

  SkBitmap bitmap;
  if (!ExtractAlphaBitmap(image, &bitmap)) {
    return nullptr;
  }

  // Mark the bitmap immutable so that it will be shared rather than copied.
  bitmap.setImmutable();
  return SkImage::MakeFromBitmap(bitmap);
}

static void
SetPaintPattern(SkPaint& aPaint, const Pattern& aPattern, Float aAlpha = 1.0, Point aOffset = Point(0, 0))
{
  switch (aPattern.GetType()) {
    case PatternType::COLOR: {
      Color color = static_cast<const ColorPattern&>(aPattern).mColor;
      aPaint.setColor(ColorToSkColor(color, aAlpha));
      break;
    }
    case PatternType::LINEAR_GRADIENT: {
      const LinearGradientPattern& pat = static_cast<const LinearGradientPattern&>(aPattern);
      GradientStopsSkia *stops = static_cast<GradientStopsSkia*>(pat.mStops.get());
      if (!stops || stops->mCount < 2 ||
          !pat.mBegin.IsFinite() || !pat.mEnd.IsFinite()) {
        aPaint.setColor(SK_ColorTRANSPARENT);
      } else {
        SkShader::TileMode mode = ExtendModeToTileMode(stops->mExtendMode, Axis::BOTH);
        SkPoint points[2];
        points[0] = SkPoint::Make(SkFloatToScalar(pat.mBegin.x), SkFloatToScalar(pat.mBegin.y));
        points[1] = SkPoint::Make(SkFloatToScalar(pat.mEnd.x), SkFloatToScalar(pat.mEnd.y));

        SkMatrix mat;
        GfxMatrixToSkiaMatrix(pat.mMatrix, mat);
        mat.postTranslate(SkFloatToScalar(aOffset.x), SkFloatToScalar(aOffset.y));
        sk_sp<SkShader> shader = SkGradientShader::MakeLinear(points,
                                                              &stops->mColors.front(),
                                                              &stops->mPositions.front(),
                                                              stops->mCount,
                                                              mode, 0, &mat);
        aPaint.setShader(shader);
      }
      break;
    }
    case PatternType::RADIAL_GRADIENT: {
      const RadialGradientPattern& pat = static_cast<const RadialGradientPattern&>(aPattern);
      GradientStopsSkia *stops = static_cast<GradientStopsSkia*>(pat.mStops.get());
      if (!stops || stops->mCount < 2 ||
          !pat.mCenter1.IsFinite() || !IsFinite(pat.mRadius1) ||
          !pat.mCenter2.IsFinite() || !IsFinite(pat.mRadius2)) {
        aPaint.setColor(SK_ColorTRANSPARENT);
      } else {
        SkShader::TileMode mode = ExtendModeToTileMode(stops->mExtendMode, Axis::BOTH);
        SkPoint points[2];
        points[0] = SkPoint::Make(SkFloatToScalar(pat.mCenter1.x), SkFloatToScalar(pat.mCenter1.y));
        points[1] = SkPoint::Make(SkFloatToScalar(pat.mCenter2.x), SkFloatToScalar(pat.mCenter2.y));

        SkMatrix mat;
        GfxMatrixToSkiaMatrix(pat.mMatrix, mat);
        mat.postTranslate(SkFloatToScalar(aOffset.x), SkFloatToScalar(aOffset.y));
        sk_sp<SkShader> shader = SkGradientShader::MakeTwoPointConical(points[0],
                                                                       SkFloatToScalar(pat.mRadius1),
                                                                       points[1],
                                                                       SkFloatToScalar(pat.mRadius2),
                                                                       &stops->mColors.front(),
                                                                       &stops->mPositions.front(),
                                                                       stops->mCount,
                                                                       mode, 0, &mat);
        aPaint.setShader(shader);
      }
      break;
    }
    case PatternType::SURFACE: {
      const SurfacePattern& pat = static_cast<const SurfacePattern&>(aPattern);
      sk_sp<SkImage> image = GetSkImageForSurface(pat.mSurface);
      if (!image) {
        aPaint.setColor(SK_ColorTRANSPARENT);
        break;
      }

      SkMatrix mat;
      GfxMatrixToSkiaMatrix(pat.mMatrix, mat);
      mat.postTranslate(SkFloatToScalar(aOffset.x), SkFloatToScalar(aOffset.y));

      if (!pat.mSamplingRect.IsEmpty()) {
        image = ExtractSubset(image, pat.mSamplingRect);
        mat.preTranslate(pat.mSamplingRect.x, pat.mSamplingRect.y);
      }

      SkShader::TileMode xTileMode = ExtendModeToTileMode(pat.mExtendMode, Axis::X_AXIS);
      SkShader::TileMode yTileMode = ExtendModeToTileMode(pat.mExtendMode, Axis::Y_AXIS);

      aPaint.setShader(image->makeShader(xTileMode, yTileMode, &mat));

      if (pat.mSamplingFilter == SamplingFilter::POINT) {
        aPaint.setFilterQuality(kNone_SkFilterQuality);
      }
      break;
    }
  }
}

static inline Rect
GetClipBounds(SkCanvas *aCanvas)
{
  // Use a manually transformed getClipDeviceBounds instead of
  // getClipBounds because getClipBounds inflates the the bounds
  // by a pixel in each direction to compensate for antialiasing.
  SkIRect deviceBounds;
  if (!aCanvas->getClipDeviceBounds(&deviceBounds)) {
    return Rect();
  }
  SkMatrix inverseCTM;
  if (!aCanvas->getTotalMatrix().invert(&inverseCTM)) {
    return Rect();
  }
  SkRect localBounds;
  inverseCTM.mapRect(&localBounds, SkRect::Make(deviceBounds));
  return SkRectToRect(localBounds);
}

struct AutoPaintSetup {
  AutoPaintSetup(SkCanvas *aCanvas, const DrawOptions& aOptions, const Pattern& aPattern, const Rect* aMaskBounds = nullptr, Point aOffset = Point(0, 0))
    : mNeedsRestore(false), mAlpha(1.0)
  {
    Init(aCanvas, aOptions, aMaskBounds, false);
    SetPaintPattern(mPaint, aPattern, mAlpha, aOffset);
  }

  AutoPaintSetup(SkCanvas *aCanvas, const DrawOptions& aOptions, const Rect* aMaskBounds = nullptr, bool aForceGroup = false)
    : mNeedsRestore(false), mAlpha(1.0)
  {
    Init(aCanvas, aOptions, aMaskBounds, aForceGroup);
  }

  ~AutoPaintSetup()
  {
    if (mNeedsRestore) {
      mCanvas->restore();
    }
  }

  void Init(SkCanvas *aCanvas, const DrawOptions& aOptions, const Rect* aMaskBounds, bool aForceGroup)
  {
    mPaint.setBlendMode(GfxOpToSkiaOp(aOptions.mCompositionOp));
    mCanvas = aCanvas;

    //TODO: Can we set greyscale somehow?
    if (aOptions.mAntialiasMode != AntialiasMode::NONE) {
      mPaint.setAntiAlias(true);
    } else {
      mPaint.setAntiAlias(false);
    }

    bool needsGroup = aForceGroup ||
                      (!IsOperatorBoundByMask(aOptions.mCompositionOp) &&
                       (!aMaskBounds || !aMaskBounds->Contains(GetClipBounds(aCanvas))));

    // TODO: We could skip the temporary for operator_source and just
    // clear the clip rect. The other operators would be harder
    // but could be worth it to skip pushing a group.
    if (needsGroup) {
      mPaint.setBlendMode(SkBlendMode::kSrcOver);
      SkPaint temp;
      temp.setBlendMode(GfxOpToSkiaOp(aOptions.mCompositionOp));
      temp.setAlpha(ColorFloatToByte(aOptions.mAlpha));
      //TODO: Get a rect here
      mCanvas->saveLayerPreserveLCDTextRequests(nullptr, &temp);
      mNeedsRestore = true;
    } else {
      mPaint.setAlpha(ColorFloatToByte(aOptions.mAlpha));
      mAlpha = aOptions.mAlpha;
    }
    mPaint.setFilterQuality(kLow_SkFilterQuality);
  }

  // TODO: Maybe add an operator overload to access this easier?
  SkPaint mPaint;
  bool mNeedsRestore;
  SkCanvas* mCanvas;
  Float mAlpha;
};

void
DrawTargetSkia::Flush()
{
  mCanvas->flush();
}

void
DrawTargetSkia::DrawSurface(SourceSurface *aSurface,
                            const Rect &aDest,
                            const Rect &aSource,
                            const DrawSurfaceOptions &aSurfOptions,
                            const DrawOptions &aOptions)
{
  if (aSource.IsEmpty()) {
    return;
  }

  MarkChanged();

  sk_sp<SkImage> image = GetSkImageForSurface(aSurface);
  if (!image) {
    return;
  }

  SkRect destRect = RectToSkRect(aDest);
  SkRect sourceRect = RectToSkRect(aSource);
  bool forceGroup = SkImageIsMask(image) &&
                    aOptions.mCompositionOp != CompositionOp::OP_OVER;

  AutoPaintSetup paint(mCanvas.get(), aOptions, &aDest, forceGroup);
  if (aSurfOptions.mSamplingFilter == SamplingFilter::POINT) {
    paint.mPaint.setFilterQuality(kNone_SkFilterQuality);
  }

  mCanvas->drawImageRect(image, sourceRect, destRect, &paint.mPaint);
}

DrawTargetType
DrawTargetSkia::GetType() const
{
#ifdef USE_SKIA_GPU
  if (mGrContext) {
    return DrawTargetType::HARDWARE_RASTER;
  }
#endif
  return DrawTargetType::SOFTWARE_RASTER;
}

void
DrawTargetSkia::DrawFilter(FilterNode *aNode,
                           const Rect &aSourceRect,
                           const Point &aDestPoint,
                           const DrawOptions &aOptions)
{
  FilterNodeSoftware* filter = static_cast<FilterNodeSoftware*>(aNode);
  filter->Draw(this, aSourceRect, aDestPoint, aOptions);
}

void
DrawTargetSkia::DrawSurfaceWithShadow(SourceSurface *aSurface,
                                      const Point &aDest,
                                      const Color &aColor,
                                      const Point &aOffset,
                                      Float aSigma,
                                      CompositionOp aOperator)
{
  if (aSurface->GetSize().IsEmpty()) {
    return;
  }

  MarkChanged();

  sk_sp<SkImage> image = GetSkImageForSurface(aSurface);
  if (!image) {
    return;
  }

  mCanvas->save();
  mCanvas->resetMatrix();

  SkPaint paint;
  paint.setBlendMode(GfxOpToSkiaOp(aOperator));

  // bug 1201272
  // We can't use the SkDropShadowImageFilter here because it applies the xfer
  // mode first to render the bitmap to a temporary layer, and then implicitly
  // uses src-over to composite the resulting shadow.
  // The canvas spec, however, states that the composite op must be used to
  // composite the resulting shadow, so we must instead use a SkBlurImageFilter
  // to blur the image ourselves.

  SkPaint shadowPaint;
  shadowPaint.setBlendMode(GfxOpToSkiaOp(aOperator));

  auto shadowDest = IntPoint::Round(aDest + aOffset);

  SkBitmap blurMask;
  if (!UsingSkiaGPU() &&
      ExtractAlphaBitmap(image, &blurMask)) {
    // Prefer using our own box blur instead of Skia's when we're
    // not using the GPU. It currently performs much better than
    // SkBlurImageFilter or SkBlurMaskFilter on the CPU.
    AlphaBoxBlur blur(Rect(0, 0, blurMask.width(), blurMask.height()),
                      int32_t(blurMask.rowBytes()),
                      aSigma, aSigma);
    blurMask.lockPixels();
    blur.Blur(reinterpret_cast<uint8_t*>(blurMask.getPixels()));
    blurMask.unlockPixels();
    blurMask.notifyPixelsChanged();

    shadowPaint.setColor(ColorToSkColor(aColor, 1.0f));

    mCanvas->drawBitmap(blurMask, shadowDest.x, shadowDest.y, &shadowPaint);
  } else {
    sk_sp<SkImageFilter> blurFilter(SkBlurImageFilter::Make(aSigma, aSigma, nullptr));
    sk_sp<SkColorFilter> colorFilter(
      SkColorFilter::MakeModeFilter(ColorToSkColor(aColor, 1.0f), SkBlendMode::kSrcIn));

    shadowPaint.setImageFilter(blurFilter);
    shadowPaint.setColorFilter(colorFilter);

    mCanvas->drawImage(image, shadowDest.x, shadowDest.y, &shadowPaint);
  }

  if (aSurface->GetFormat() != SurfaceFormat::A8) {
    // Composite the original image after the shadow
    auto dest = IntPoint::Round(aDest);
    mCanvas->drawImage(image, dest.x, dest.y, &paint);
  }

  mCanvas->restore();
}

void
DrawTargetSkia::FillRect(const Rect &aRect,
                         const Pattern &aPattern,
                         const DrawOptions &aOptions)
{
  // The sprite blitting path in Skia can be faster than the shader blitter for
  // operators other than source (or source-over with opaque surface). So, when
  // possible/beneficial, route to DrawSurface which will use the sprite blitter.
  if (aPattern.GetType() == PatternType::SURFACE &&
      aOptions.mCompositionOp != CompositionOp::OP_SOURCE) {
    const SurfacePattern& pat = static_cast<const SurfacePattern&>(aPattern);
    // Verify there is a valid surface and a pattern matrix without skew.
    if (pat.mSurface &&
        (aOptions.mCompositionOp != CompositionOp::OP_OVER ||
         GfxFormatToSkiaAlphaType(pat.mSurface->GetFormat()) != kOpaque_SkAlphaType) &&
        !pat.mMatrix.HasNonAxisAlignedTransform()) {
      // Bound the sampling to smaller of the bounds or the sampling rect.
      IntRect srcRect(IntPoint(0, 0), pat.mSurface->GetSize());
      if (!pat.mSamplingRect.IsEmpty()) {
        srcRect = srcRect.Intersect(pat.mSamplingRect);
      }
      // Transform the destination rectangle by the inverse of the pattern
      // matrix so that it is in pattern space like the source rectangle.
      Rect patRect = aRect - pat.mMatrix.GetTranslation();
      patRect.Scale(1.0f / pat.mMatrix._11, 1.0f / pat.mMatrix._22);
      // Verify the pattern rectangle will not tile or clamp.
      if (!patRect.IsEmpty() && srcRect.Contains(RoundedOut(patRect))) {
        // The pattern is a surface with an axis-aligned source rectangle
        // fitting entirely in its bounds, so just treat it as a DrawSurface.
        DrawSurface(pat.mSurface, aRect, patRect,
                    DrawSurfaceOptions(pat.mSamplingFilter),
                    aOptions);
        return;
      }
    }
  }

  MarkChanged();
  SkRect rect = RectToSkRect(aRect);
  AutoPaintSetup paint(mCanvas.get(), aOptions, aPattern, &aRect);

  mCanvas->drawRect(rect, paint.mPaint);
}

void
DrawTargetSkia::Stroke(const Path *aPath,
                       const Pattern &aPattern,
                       const StrokeOptions &aStrokeOptions,
                       const DrawOptions &aOptions)
{
  MarkChanged();
  MOZ_ASSERT(aPath, "Null path");
  if (aPath->GetBackendType() != BackendType::SKIA) {
    return;
  }

  const PathSkia *skiaPath = static_cast<const PathSkia*>(aPath);


  AutoPaintSetup paint(mCanvas.get(), aOptions, aPattern);
  if (!StrokeOptionsToPaint(paint.mPaint, aStrokeOptions)) {
    return;
  }

  if (!skiaPath->GetPath().isFinite()) {
    return;
  }

  mCanvas->drawPath(skiaPath->GetPath(), paint.mPaint);
}

static Double
DashPeriodLength(const StrokeOptions& aStrokeOptions)
{
  Double length = 0;
  for (size_t i = 0; i < aStrokeOptions.mDashLength; i++) {
    length += aStrokeOptions.mDashPattern[i];
  }
  if (aStrokeOptions.mDashLength & 1) {
    // "If an odd number of values is provided, then the list of values is
    // repeated to yield an even number of values."
    // Double the length.
    length += length;
  }
  return length;
}

static inline Double
RoundDownToMultiple(Double aValue, Double aFactor)
{
  return floor(aValue / aFactor) * aFactor;
}

static Rect
UserSpaceStrokeClip(const IntRect &aDeviceClip,
                    const Matrix &aTransform,
                    const StrokeOptions &aStrokeOptions)
{
  Matrix inverse = aTransform;
  if (!inverse.Invert()) {
    return Rect();
  }
  Rect deviceClip(aDeviceClip);
  deviceClip.Inflate(MaxStrokeExtents(aStrokeOptions, aTransform));
  return inverse.TransformBounds(deviceClip);
}

static Rect
ShrinkClippedStrokedRect(const Rect &aStrokedRect, const IntRect &aDeviceClip,
                         const Matrix &aTransform,
                         const StrokeOptions &aStrokeOptions)
{
  Rect userSpaceStrokeClip =
    UserSpaceStrokeClip(aDeviceClip, aTransform, aStrokeOptions);
  RectDouble strokedRectDouble(
    aStrokedRect.x, aStrokedRect.y, aStrokedRect.width, aStrokedRect.height);
  RectDouble intersection =
    strokedRectDouble.Intersect(RectDouble(userSpaceStrokeClip.x,
                                           userSpaceStrokeClip.y,
                                           userSpaceStrokeClip.width,
                                           userSpaceStrokeClip.height));
  Double dashPeriodLength = DashPeriodLength(aStrokeOptions);
  if (intersection.IsEmpty() || dashPeriodLength == 0.0f) {
    return Rect(
      intersection.x, intersection.y, intersection.width, intersection.height);
  }

  // Reduce the rectangle side lengths in multiples of the dash period length
  // so that the visible dashes stay in the same place.
  MarginDouble insetBy = strokedRectDouble - intersection;
  insetBy.top = RoundDownToMultiple(insetBy.top, dashPeriodLength);
  insetBy.right = RoundDownToMultiple(insetBy.right, dashPeriodLength);
  insetBy.bottom = RoundDownToMultiple(insetBy.bottom, dashPeriodLength);
  insetBy.left = RoundDownToMultiple(insetBy.left, dashPeriodLength);

  strokedRectDouble.Deflate(insetBy);
  return Rect(strokedRectDouble.x,
              strokedRectDouble.y,
              strokedRectDouble.width,
              strokedRectDouble.height);
}

void
DrawTargetSkia::StrokeRect(const Rect &aRect,
                           const Pattern &aPattern,
                           const StrokeOptions &aStrokeOptions,
                           const DrawOptions &aOptions)
{
  // Stroking large rectangles with dashes is expensive with Skia (fixed
  // overhead based on the number of dashes, regardless of whether the dashes
  // are visible), so we try to reduce the size of the stroked rectangle as
  // much as possible before passing it on to Skia.
  Rect rect = aRect;
  if (aStrokeOptions.mDashLength > 0 && !rect.IsEmpty()) {
    IntRect deviceClip(IntPoint(0, 0), mSize);
    SkIRect clipBounds;
    if (mCanvas->getClipDeviceBounds(&clipBounds)) {
      deviceClip = SkIRectToIntRect(clipBounds);
    }
    rect = ShrinkClippedStrokedRect(rect, deviceClip, mTransform, aStrokeOptions);
    if (rect.IsEmpty()) {
      return;
    }
  }

  MarkChanged();
  AutoPaintSetup paint(mCanvas.get(), aOptions, aPattern);
  if (!StrokeOptionsToPaint(paint.mPaint, aStrokeOptions)) {
    return;
  }

  mCanvas->drawRect(RectToSkRect(rect), paint.mPaint);
}

void
DrawTargetSkia::StrokeLine(const Point &aStart,
                           const Point &aEnd,
                           const Pattern &aPattern,
                           const StrokeOptions &aStrokeOptions,
                           const DrawOptions &aOptions)
{
  MarkChanged();
  AutoPaintSetup paint(mCanvas.get(), aOptions, aPattern);
  if (!StrokeOptionsToPaint(paint.mPaint, aStrokeOptions)) {
    return;
  }

  mCanvas->drawLine(SkFloatToScalar(aStart.x), SkFloatToScalar(aStart.y),
                    SkFloatToScalar(aEnd.x), SkFloatToScalar(aEnd.y),
                    paint.mPaint);
}

void
DrawTargetSkia::Fill(const Path *aPath,
                    const Pattern &aPattern,
                    const DrawOptions &aOptions)
{
  MarkChanged();
  if (!aPath || aPath->GetBackendType() != BackendType::SKIA) {
    return;
  }

  const PathSkia *skiaPath = static_cast<const PathSkia*>(aPath);

  AutoPaintSetup paint(mCanvas.get(), aOptions, aPattern);

  if (!skiaPath->GetPath().isFinite()) {
    return;
  }

  mCanvas->drawPath(skiaPath->GetPath(), paint.mPaint);
}

bool
DrawTargetSkia::ShouldLCDRenderText(FontType aFontType, AntialiasMode aAntialiasMode)
{
  // Only allow subpixel AA if explicitly permitted.
  if (!GetPermitSubpixelAA()) {
    return false;
  }

  if (aAntialiasMode == AntialiasMode::DEFAULT) {
    switch (aFontType) {
      case FontType::MAC:
      case FontType::GDI:
      case FontType::DWRITE:
      case FontType::FONTCONFIG:
        return true;
      default:
        // TODO: Figure out what to do for the other platforms.
        return false;
    }
  }
  return (aAntialiasMode == AntialiasMode::SUBPIXEL);
}

static bool
CanDrawFont(ScaledFont* aFont)
{
  switch (aFont->GetType()) {
  case FontType::SKIA:
  case FontType::CAIRO:
  case FontType::FONTCONFIG:
  case FontType::MAC:
  case FontType::GDI:
  case FontType::DWRITE:
    return true;
  default:
    return false;
  }
}

void
DrawTargetSkia::FillGlyphs(ScaledFont *aFont,
                           const GlyphBuffer &aBuffer,
                           const Pattern &aPattern,
                           const DrawOptions &aOptions,
                           const GlyphRenderingOptions *aRenderingOptions)
{
  if (!CanDrawFont(aFont)) {
    return;
  }

  MarkChanged();

  ScaledFontBase* skiaFont = static_cast<ScaledFontBase*>(aFont);
  SkTypeface* typeface = skiaFont->GetSkTypeface();
  if (!typeface) {
    return;
  }

  AutoPaintSetup paint(mCanvas.get(), aOptions, aPattern);
  AntialiasMode aaMode = aFont->GetDefaultAAMode();
  if (aOptions.mAntialiasMode != AntialiasMode::DEFAULT) {
    aaMode = aOptions.mAntialiasMode;
  }
  bool aaEnabled = aaMode != AntialiasMode::NONE;

  paint.mPaint.setAntiAlias(aaEnabled);
  paint.mPaint.setTypeface(sk_ref_sp(typeface));
  paint.mPaint.setTextSize(SkFloatToScalar(skiaFont->mSize));
  paint.mPaint.setTextEncoding(SkPaint::kGlyphID_TextEncoding);

  bool shouldLCDRenderText = ShouldLCDRenderText(aFont->GetType(), aaMode);
  paint.mPaint.setLCDRenderText(shouldLCDRenderText);

  bool useSubpixelText = true;

  switch (aFont->GetType()) {
  case FontType::SKIA:
  case FontType::CAIRO:
  case FontType::FONTCONFIG:
    // SkFontHost_cairo does not support subpixel text positioning,
    // so only enable it for other font hosts.
    useSubpixelText = false;
    break;
  case FontType::MAC:
    if (aaMode == AntialiasMode::GRAY) {
      // Normally, Skia enables LCD FontSmoothing which creates thicker fonts
      // and also enables subpixel AA. CoreGraphics without font smoothing
      // explicitly creates thinner fonts and grayscale AA.
      // CoreGraphics doesn't support a configuration that produces thicker
      // fonts with grayscale AA as LCD Font Smoothing enables or disables both.
      // However, Skia supports it by enabling font smoothing (producing subpixel AA)
      // and converts it to grayscale AA. Since Skia doesn't support subpixel AA on
      // transparent backgrounds, we still want font smoothing for the thicker fonts,
      // even if it is grayscale AA.
      //
      // With explicit Grayscale AA (from -moz-osx-font-smoothing:grayscale),
      // we want to have grayscale AA with no smoothing at all. This means
      // disabling the LCD font smoothing behaviour.
      // To accomplish this we have to explicitly disable hinting,
      // and disable LCDRenderText.
      paint.mPaint.setHinting(SkPaint::kNo_Hinting);
    }
    break;
  case FontType::GDI:
  {
    if (!shouldLCDRenderText && aaEnabled) {
      // If we have non LCD GDI text, render the fonts as cleartype and convert them
      // to grayscale. This seems to be what Chrome and IE are doing on Windows 7.
      // This also applies if cleartype is disabled system wide.
      paint.mPaint.setFlags(paint.mPaint.getFlags() | SkPaint::kGenA8FromLCD_Flag);
    }
    break;
  }
#ifdef XP_WIN
  case FontType::DWRITE:
  {
    ScaledFontDWrite* dwriteFont = static_cast<ScaledFontDWrite*>(aFont);
    paint.mPaint.setEmbeddedBitmapText(dwriteFont->UseEmbeddedBitmaps());

    if (dwriteFont->ForceGDIMode()) {
      paint.mPaint.setEmbeddedBitmapText(true);
      useSubpixelText = false;
    }
    break;
  }
#endif
  default:
    break;
  }

  paint.mPaint.setSubpixelText(useSubpixelText);

  std::vector<uint16_t> indices;
  std::vector<SkPoint> offsets;
  indices.resize(aBuffer.mNumGlyphs);
  offsets.resize(aBuffer.mNumGlyphs);

  for (unsigned int i = 0; i < aBuffer.mNumGlyphs; i++) {
    indices[i] = aBuffer.mGlyphs[i].mIndex;
    offsets[i].fX = SkFloatToScalar(aBuffer.mGlyphs[i].mPosition.x);
    offsets[i].fY = SkFloatToScalar(aBuffer.mGlyphs[i].mPosition.y);
  }

  mCanvas->drawPosText(&indices.front(), aBuffer.mNumGlyphs*2, &offsets.front(), paint.mPaint);
}

void
DrawTargetSkia::Mask(const Pattern &aSource,
                     const Pattern &aMask,
                     const DrawOptions &aOptions)
{
  MarkChanged();
  AutoPaintSetup paint(mCanvas.get(), aOptions, aSource);

  SkPaint maskPaint;
  SetPaintPattern(maskPaint, aMask);

  SkLayerRasterizer::Builder builder;
  builder.addLayer(maskPaint);
  sk_sp<SkLayerRasterizer> raster(builder.detach());
  paint.mPaint.setRasterizer(raster);

  mCanvas->drawPaint(paint.mPaint);
}

void
DrawTargetSkia::MaskSurface(const Pattern &aSource,
                            SourceSurface *aMask,
                            Point aOffset,
                            const DrawOptions &aOptions)
{
  MarkChanged();
  AutoPaintSetup paint(mCanvas.get(), aOptions, aSource, nullptr, -aOffset);

  sk_sp<SkImage> alphaMask = ExtractAlphaForSurface(aMask);
  if (!alphaMask) {
    gfxDebug() << *this << ": MaskSurface() failed to extract alpha for mask";
    return;
  }

  mCanvas->drawImage(alphaMask, aOffset.x, aOffset.y, &paint.mPaint);
}

bool
DrawTarget::Draw3DTransformedSurface(SourceSurface* aSurface, const Matrix4x4& aMatrix)
{
  // Composite the 3D transform with the DT's transform.
  Matrix4x4 fullMat = aMatrix * Matrix4x4::From2D(mTransform);
  if (fullMat.IsSingular()) {
    return false;
  }
  // Transform the surface bounds and clip to this DT.
  IntRect xformBounds =
    RoundedOut(
      fullMat.TransformAndClipBounds(Rect(Point(0, 0), Size(aSurface->GetSize())),
                                     Rect(Point(0, 0), Size(GetSize()))));
  if (xformBounds.IsEmpty()) {
    return true;
  }
  // Offset the matrix by the transformed origin.
  fullMat.PostTranslate(-xformBounds.x, -xformBounds.y, 0);

  // Read in the source data.
  sk_sp<SkImage> srcImage = GetSkImageForSurface(aSurface);
  if (!srcImage) {
    return true;
  }

  // Set up an intermediate destination surface only the size of the transformed bounds.
  // Try to pass through the source's format unmodified in both the BGRA and ARGB cases.
  RefPtr<DataSourceSurface> dstSurf =
    Factory::CreateDataSourceSurface(xformBounds.Size(),
                                     !srcImage->isOpaque() ?
                                       aSurface->GetFormat() : SurfaceFormat::A8R8G8B8_UINT32,
                                     true);
  if (!dstSurf) {
    return false;
  }
  sk_sp<SkCanvas> dstCanvas(
    SkCanvas::NewRasterDirect(
      SkImageInfo::Make(xformBounds.width, xformBounds.height,
                        GfxFormatToSkiaColorType(dstSurf->GetFormat()),
                        kPremul_SkAlphaType),
      dstSurf->GetData(), dstSurf->Stride()));
  if (!dstCanvas) {
    return false;
  }

  // Do the transform.
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setFilterQuality(kLow_SkFilterQuality);
  paint.setBlendMode(SkBlendMode::kSrc);

  SkMatrix xform;
  GfxMatrixToSkiaMatrix(fullMat, xform);
  dstCanvas->setMatrix(xform);

  dstCanvas->drawImage(srcImage, 0, 0, &paint);
  dstCanvas->flush();

  // Temporarily reset the DT's transform, since it has already been composed above.
  Matrix origTransform = mTransform;
  SetTransform(Matrix());

  // Draw the transformed surface within the transformed bounds.
  DrawSurface(dstSurf, Rect(xformBounds), Rect(Point(0, 0), Size(xformBounds.Size())));

  SetTransform(origTransform);

  return true;
}

bool
DrawTargetSkia::Draw3DTransformedSurface(SourceSurface* aSurface, const Matrix4x4& aMatrix)
{
  if (aMatrix.IsSingular()) {
    return false;
  }

  MarkChanged();

  sk_sp<SkImage> image = GetSkImageForSurface(aSurface);
  if (!image) {
    return true;
  }

  mCanvas->save();

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setFilterQuality(kLow_SkFilterQuality);

  SkMatrix xform;
  GfxMatrixToSkiaMatrix(aMatrix, xform);
  mCanvas->concat(xform);

  mCanvas->drawImage(image, 0, 0, &paint);

  mCanvas->restore();

  return true;
}

already_AddRefed<SourceSurface>
DrawTargetSkia::CreateSourceSurfaceFromData(unsigned char *aData,
                                            const IntSize &aSize,
                                            int32_t aStride,
                                            SurfaceFormat aFormat) const
{
  RefPtr<SourceSurfaceSkia> newSurf = new SourceSurfaceSkia();

  if (!newSurf->InitFromData(aData, aSize, aStride, aFormat)) {
    gfxDebug() << *this << ": Failure to create source surface from data. Size: " << aSize;
    return nullptr;
  }

  return newSurf.forget();
}

already_AddRefed<DrawTarget>
DrawTargetSkia::CreateSimilarDrawTarget(const IntSize &aSize, SurfaceFormat aFormat) const
{
  RefPtr<DrawTargetSkia> target = new DrawTargetSkia();
#ifdef USE_SKIA_GPU
  if (UsingSkiaGPU()) {
    // Try to create a GPU draw target first if we're currently using the GPU.
    // Mark the DT as cached so that shadow DTs, extracted subrects, and similar can be reused.
    if (target->InitWithGrContext(mGrContext.get(), aSize, aFormat, true)) {
      return target.forget();
    }
    // Otherwise, just fall back to a software draw target.
  }
#endif

#ifdef DEBUG
  if (!IsBackedByPixels(mCanvas.get())) {
    // If our canvas is backed by vector storage such as PDF then we want to
    // create a new DrawTarget with similar storage to avoid losing fidelity
    // (fidelity will be lost if the returned DT is Snapshot()'ed and drawn
    // back onto us since a raster will be drawn instead of vector commands).
    NS_WARNING("Not backed by pixels - we need to handle PDF backed SkCanvas");
  }
#endif

  if (!target->Init(aSize, aFormat)) {
    return nullptr;
  }
  return target.forget();
}

bool
DrawTargetSkia::UsingSkiaGPU() const
{
#ifdef USE_SKIA_GPU
  return !!mGrContext;
#else
  return false;
#endif
}

#ifdef USE_SKIA_GPU
already_AddRefed<SourceSurface>
DrawTargetSkia::OptimizeGPUSourceSurface(SourceSurface *aSurface) const
{
  // Check if the underlying SkImage already has an associated GrTexture.
  sk_sp<SkImage> image = GetSkImageForSurface(aSurface);
  if (!image || image->isTextureBacked()) {
    RefPtr<SourceSurface> surface(aSurface);
    return surface.forget();
  }

  // Upload the SkImage to a GrTexture otherwise.
  sk_sp<SkImage> texture = image->makeTextureImage(mGrContext.get());
  if (texture) {
    // Create a new SourceSurfaceSkia whose SkImage contains the GrTexture.
    RefPtr<SourceSurfaceSkia> surface = new SourceSurfaceSkia();
    if (surface->InitFromImage(texture, aSurface->GetFormat())) {
      return surface.forget();
    }
  }

  // The data was too big to fit in a GrTexture.
  if (aSurface->GetType() == SurfaceType::SKIA) {
    // It is already a Skia source surface, so just reuse it as-is.
    RefPtr<SourceSurface> surface(aSurface);
    return surface.forget();
  }

  // Wrap it in a Skia source surface so that can do tiled uploads on-demand.
  RefPtr<SourceSurfaceSkia> surface = new SourceSurfaceSkia();
  surface->InitFromImage(image);
  return surface.forget();
}
#endif

already_AddRefed<SourceSurface>
DrawTargetSkia::OptimizeSourceSurfaceForUnknownAlpha(SourceSurface *aSurface) const
{
#ifdef USE_SKIA_GPU
  if (UsingSkiaGPU()) {
    return OptimizeGPUSourceSurface(aSurface);
  }
#endif

  if (aSurface->GetType() == SurfaceType::SKIA) {
    RefPtr<SourceSurface> surface(aSurface);
    return surface.forget();
  }

  RefPtr<DataSourceSurface> dataSurface = aSurface->GetDataSurface();

  // For plugins, GDI can sometimes just write 0 to the alpha channel
  // even for RGBX formats. In this case, we have to manually write
  // the alpha channel to make Skia happy with RGBX and in case GDI
  // writes some bad data. Luckily, this only happens on plugins.
  WriteRGBXFormat(dataSurface->GetData(), dataSurface->GetSize(),
                  dataSurface->Stride(), dataSurface->GetFormat());
  return dataSurface.forget();
}

already_AddRefed<SourceSurface>
DrawTargetSkia::OptimizeSourceSurface(SourceSurface *aSurface) const
{
#ifdef USE_SKIA_GPU
  if (UsingSkiaGPU()) {
    return OptimizeGPUSourceSurface(aSurface);
  }
#endif

  if (aSurface->GetType() == SurfaceType::SKIA) {
    RefPtr<SourceSurface> surface(aSurface);
    return surface.forget();
  }

  // If we're not using skia-gl then drawing doesn't require any
  // uploading, so any data surface is fine. Call GetDataSurface
  // to trigger any required readback so that it only happens
  // once.
  RefPtr<DataSourceSurface> dataSurface = aSurface->GetDataSurface();
  MOZ_ASSERT(VerifyRGBXFormat(dataSurface->GetData(), dataSurface->GetSize(),
                              dataSurface->Stride(), dataSurface->GetFormat()));
  return dataSurface.forget();
}

already_AddRefed<SourceSurface>
DrawTargetSkia::CreateSourceSurfaceFromNativeSurface(const NativeSurface &aSurface) const
{
#ifdef USE_SKIA_GPU
  if (aSurface.mType == NativeSurfaceType::OPENGL_TEXTURE && UsingSkiaGPU()) {
    // Wrap the OpenGL texture id in a Skia texture handle.
    GrBackendTextureDesc texDesc;
    texDesc.fWidth = aSurface.mSize.width;
    texDesc.fHeight = aSurface.mSize.height;
    texDesc.fOrigin = kTopLeft_GrSurfaceOrigin;
    texDesc.fConfig = GfxFormatToGrConfig(aSurface.mFormat);

    GrGLTextureInfo texInfo;
    texInfo.fTarget = LOCAL_GL_TEXTURE_2D;
    texInfo.fID = (GrGLuint)(uintptr_t)aSurface.mSurface;
    texDesc.fTextureHandle = reinterpret_cast<GrBackendObject>(&texInfo);

    sk_sp<SkImage> texture =
      SkImage::MakeFromAdoptedTexture(mGrContext.get(), texDesc,
                                      GfxFormatToSkiaAlphaType(aSurface.mFormat));
    RefPtr<SourceSurfaceSkia> newSurf = new SourceSurfaceSkia();
    if (texture && newSurf->InitFromImage(texture, aSurface.mFormat)) {
      return newSurf.forget();
    }
    return nullptr;
  }
#endif

  return nullptr;
}

void
DrawTargetSkia::CopySurface(SourceSurface *aSurface,
                            const IntRect& aSourceRect,
                            const IntPoint &aDestination)
{
  MarkChanged();

  sk_sp<SkImage> image = GetSkImageForSurface(aSurface);
  if (!image) {
    return;
  }

  mCanvas->save();
  mCanvas->setMatrix(SkMatrix::MakeTrans(SkIntToScalar(aDestination.x), SkIntToScalar(aDestination.y)));
  mCanvas->clipRect(SkRect::MakeIWH(aSourceRect.width, aSourceRect.height), kReplace_SkClipOp);

  SkPaint paint;
  if (!image->isOpaque()) {
    // Keep the xfermode as SOURCE_OVER for opaque bitmaps
    // http://code.google.com/p/skia/issues/detail?id=628
    paint.setBlendMode(SkBlendMode::kSrc);
  }
  // drawImage with A8 images ends up doing a mask operation
  // so we need to clear before
  if (SkImageIsMask(image)) {
    mCanvas->clear(SK_ColorTRANSPARENT);
  }
  mCanvas->drawImage(image, -SkIntToScalar(aSourceRect.x), -SkIntToScalar(aSourceRect.y), &paint);
  mCanvas->restore();
}

bool
DrawTargetSkia::Init(const IntSize &aSize, SurfaceFormat aFormat)
{
  if (size_t(std::max(aSize.width, aSize.height)) > GetMaxSurfaceSize()) {
    return false;
  }

  // we need to have surfaces that have a stride aligned to 4 for interop with cairo
  SkImageInfo info = MakeSkiaImageInfo(aSize, aFormat);
  size_t stride = SkAlign4(info.minRowBytes());
  mSurface = SkSurface::MakeRaster(info, stride, nullptr);
  if (!mSurface) {
    return false;
  }

  mSize = aSize;
  mFormat = aFormat;
  mCanvas = sk_ref_sp(mSurface->getCanvas());
  SetPermitSubpixelAA(IsOpaque(mFormat));

  if (info.isOpaque()) {
    mCanvas->clear(SK_ColorBLACK);
  }
  return true;
}

bool
DrawTargetSkia::Init(SkCanvas* aCanvas)
{
  mCanvas = sk_ref_sp(aCanvas);

  SkImageInfo imageInfo = mCanvas->imageInfo();

  // If the canvas is backed by pixels we clear it to be on the safe side.  If
  // it's not (for example, for PDF output) we don't.
  if (IsBackedByPixels(mCanvas.get())) {
    SkColor clearColor = imageInfo.isOpaque() ? SK_ColorBLACK : SK_ColorTRANSPARENT;
    mCanvas->clear(clearColor);
  }

  SkISize size = mCanvas->getBaseLayerSize();
  mSize.width = size.width();
  mSize.height = size.height();
  mFormat = SkiaColorTypeToGfxFormat(imageInfo.colorType(),
                                     imageInfo.alphaType());
  SetPermitSubpixelAA(IsOpaque(mFormat));
  return true;
}

#ifdef USE_SKIA_GPU
/** Indicating a DT should be cached means that space will be reserved in Skia's cache
 * for the render target at creation time, with any unused resources exceeding the cache
 * limits being purged. When the DT is freed, it will then be guaranteed to be kept around
 * for subsequent allocations until it gets incidentally purged.
 *
 * If it is not marked as cached, no space will be purged to make room for the render
 * target in the cache. When the DT is freed, If there is space within the resource limits
 * it may be added to the cache, otherwise it will be freed immediately if the cache is
 * already full.
 *
 * If you want to ensure that the resources will be kept around for reuse, it is better
 * to mark them as cached. Such resources should be short-lived to ensure they don't
 * permanently tie up cache resource limits. Long-lived resources should generally be
 * left as uncached.
 *
 * In neither case will cache resource limits affect whether the resource allocation
 * succeeds. The amount of in-use GPU resources is allowed to exceed the size of the cache.
 * Thus, only hard GPU out-of-memory conditions will cause resource allocation to fail.
 */
bool
DrawTargetSkia::InitWithGrContext(GrContext* aGrContext,
                                  const IntSize &aSize,
                                  SurfaceFormat aFormat,
                                  bool aCached)
{
  MOZ_ASSERT(aGrContext, "null GrContext");

  if (size_t(std::max(aSize.width, aSize.height)) > GetMaxSurfaceSize()) {
    return false;
  }

  // Create a GPU rendertarget/texture using the supplied GrContext.
  // NewRenderTarget also implicitly clears the underlying texture on creation.
  mSurface =
    SkSurface::MakeRenderTarget(aGrContext,
                                SkBudgeted(aCached),
                                MakeSkiaImageInfo(aSize, aFormat));
  if (!mSurface) {
    return false;
  }

  mGrContext = sk_ref_sp(aGrContext);
  mSize = aSize;
  mFormat = aFormat;
  mCanvas = sk_ref_sp(mSurface->getCanvas());
  SetPermitSubpixelAA(IsOpaque(mFormat));
  return true;
}

#endif

bool
DrawTargetSkia::Init(unsigned char* aData, const IntSize &aSize, int32_t aStride, SurfaceFormat aFormat, bool aUninitialized)
{
  MOZ_ASSERT((aFormat != SurfaceFormat::B8G8R8X8) ||
              aUninitialized || VerifyRGBXFormat(aData, aSize, aStride, aFormat));

  mSurface = SkSurface::MakeRasterDirect(MakeSkiaImageInfo(aSize, aFormat), aData, aStride);
  if (!mSurface) {
    return false;
  }

  mSize = aSize;
  mFormat = aFormat;
  mCanvas = sk_ref_sp(mSurface->getCanvas());
  SetPermitSubpixelAA(IsOpaque(mFormat));
  return true;
}

void
DrawTargetSkia::SetTransform(const Matrix& aTransform)
{
  SkMatrix mat;
  GfxMatrixToSkiaMatrix(aTransform, mat);
  mCanvas->setMatrix(mat);
  mTransform = aTransform;
}

void*
DrawTargetSkia::GetNativeSurface(NativeSurfaceType aType)
{
#ifdef USE_SKIA_GPU
  if (aType == NativeSurfaceType::OPENGL_TEXTURE && mSurface) {
    GrBackendObject handle = mSurface->getTextureHandle(SkSurface::kFlushRead_BackendHandleAccess);
    if (handle) {
      return (void*)(uintptr_t)reinterpret_cast<GrGLTextureInfo *>(handle)->fID;
    }
  }
#endif
  return nullptr;
}


already_AddRefed<PathBuilder>
DrawTargetSkia::CreatePathBuilder(FillRule aFillRule) const
{
  return MakeAndAddRef<PathBuilderSkia>(aFillRule);
}

void
DrawTargetSkia::ClearRect(const Rect &aRect)
{
  MarkChanged();
  mCanvas->save();
  mCanvas->clipRect(RectToSkRect(aRect), kIntersect_SkClipOp, true);
  SkColor clearColor = (mFormat == SurfaceFormat::B8G8R8X8) ? SK_ColorBLACK : SK_ColorTRANSPARENT;
  mCanvas->clear(clearColor);
  mCanvas->restore();
}

void
DrawTargetSkia::PushClip(const Path *aPath)
{
  if (aPath->GetBackendType() != BackendType::SKIA) {
    return;
  }

  const PathSkia *skiaPath = static_cast<const PathSkia*>(aPath);
  mCanvas->save();
  mCanvas->clipPath(skiaPath->GetPath(), kIntersect_SkClipOp, true);
}

void
DrawTargetSkia::PushDeviceSpaceClipRects(const IntRect* aRects, uint32_t aCount)
{
  // Build a region by unioning all the rects together.
  SkRegion region;
  for (uint32_t i = 0; i < aCount; i++) {
    region.op(IntRectToSkIRect(aRects[i]), SkRegion::kUnion_Op);
  }

  // Clip with the resulting region. clipRegion does not transform
  // this region by the current transform, unlike the other SkCanvas
  // clip methods, so it is just passed through in device-space.
  mCanvas->save();
  mCanvas->clipRegion(region, kIntersect_SkClipOp);
}

void
DrawTargetSkia::PushClipRect(const Rect& aRect)
{
  SkRect rect = RectToSkRect(aRect);

  mCanvas->save();
  mCanvas->clipRect(rect, kIntersect_SkClipOp, true);
}

void
DrawTargetSkia::PopClip()
{
  mCanvas->restore();
}

// Image filter that just passes the source through to the result unmodified.
class CopyLayerImageFilter : public SkImageFilter
{
public:
  CopyLayerImageFilter()
    : SkImageFilter(nullptr, 0, nullptr)
  {}

  sk_sp<SkSpecialImage> onFilterImage(SkSpecialImage* source,
                                      const Context& ctx,
                                      SkIPoint* offset) const override {
    offset->set(0, 0);
    return sk_ref_sp(source);
  }

  SK_TO_STRING_OVERRIDE()
  SK_DECLARE_PUBLIC_FLATTENABLE_DESERIALIZATION_PROCS(CopyLayerImageFilter)
};

sk_sp<SkFlattenable>
CopyLayerImageFilter::CreateProc(SkReadBuffer& buffer)
{
  SK_IMAGEFILTER_UNFLATTEN_COMMON(common, 0);
  return sk_make_sp<CopyLayerImageFilter>();
}

#ifndef SK_IGNORE_TO_STRING
void
CopyLayerImageFilter::toString(SkString* str) const
{
  str->append("CopyLayerImageFilter: ()");
}
#endif

void
DrawTargetSkia::PushLayer(bool aOpaque, Float aOpacity, SourceSurface* aMask,
                          const Matrix& aMaskTransform, const IntRect& aBounds,
                          bool aCopyBackground)
{
  PushedLayer layer(GetPermitSubpixelAA(), aOpaque, aOpacity, aMask, aMaskTransform,
                    mCanvas->getTopDevice());
  mPushedLayers.push_back(layer);

  SkPaint paint;

  // If we have a mask, set the opacity to 0 so that SkCanvas::restore skips
  // implicitly drawing the layer so that we can properly mask it in PopLayer.
  paint.setAlpha(aMask ? 0 : ColorFloatToByte(aOpacity));

  // aBounds is supplied in device space, but SaveLayerRec wants local space.
  SkRect bounds = IntRectToSkRect(aBounds);
  if (!bounds.isEmpty()) {
    SkMatrix inverseCTM;
    if (mCanvas->getTotalMatrix().invert(&inverseCTM)) {
      inverseCTM.mapRect(&bounds);
    } else {
      bounds.setEmpty();
    }
  }

  sk_sp<SkImageFilter> backdrop(aCopyBackground ? new CopyLayerImageFilter : nullptr);

  SkCanvas::SaveLayerRec saveRec(aBounds.IsEmpty() ? nullptr : &bounds,
                                 &paint,
                                 backdrop.get(),
                                 SkCanvas::kPreserveLCDText_SaveLayerFlag |
                                   (aOpaque ? SkCanvas::kIsOpaque_SaveLayerFlag : 0));

  mCanvas->saveLayer(saveRec);

  SetPermitSubpixelAA(aOpaque);
}

void
DrawTargetSkia::PopLayer()
{
  MarkChanged();

  MOZ_ASSERT(mPushedLayers.size());
  const PushedLayer& layer = mPushedLayers.back();

  // Ensure that the top device has actually changed. If it hasn't, then there
  // is no layer image to be masked.
  if (layer.mMask &&
      layer.mPreviousDevice != mCanvas->getTopDevice()) {
    // If we have a mask, take a reference to the top layer's device so that
    // we can mask it ourselves. This assumes we forced SkCanvas::restore to
    // skip implicitly drawing the layer.
    sk_sp<SkBaseDevice> layerDevice = sk_ref_sp(mCanvas->getTopDevice());
    SkIRect layerBounds = layerDevice->getGlobalBounds();
    sk_sp<SkImage> layerImage;
    SkPixmap layerPixmap;
    if (layerDevice->peekPixels(&layerPixmap)) {
      layerImage = SkImage::MakeFromRaster(layerPixmap, nullptr, nullptr);
#ifdef USE_SKIA_GPU
    } else if (GrDrawContext* drawCtx = mCanvas->internal_private_accessTopLayerDrawContext()) {
      drawCtx->prepareForExternalIO();
      if (GrTexture* tex = drawCtx->accessRenderTarget()->asTexture()) {
        layerImage = sk_make_sp<SkImage_Gpu>(layerBounds.width(), layerBounds.height(),
                                             kNeedNewImageUniqueID,
                                             layerDevice->imageInfo().alphaType(),
                                             tex, nullptr, SkBudgeted::kNo);
      }
#endif
    }

    // Restore the background with the layer's device left alive.
    mCanvas->restore();

    SkPaint paint;
    paint.setAlpha(ColorFloatToByte(layer.mOpacity));

    SkMatrix maskMat, layerMat;
    // Get the total transform affecting the mask, considering its pattern
    // transform and the current canvas transform.
    GfxMatrixToSkiaMatrix(layer.mMaskTransform, maskMat);
    maskMat.postConcat(mCanvas->getTotalMatrix());
    if (!maskMat.invert(&layerMat)) {
      gfxDebug() << *this << ": PopLayer() failed to invert mask transform";
    } else {
      // The layer should not be affected by the current canvas transform,
      // even though the mask is. So first we use the inverse of the transform
      // affecting the mask, then add back on the layer's origin.
      layerMat.preTranslate(layerBounds.x(), layerBounds.y());

      if (layerImage) {
        paint.setShader(layerImage->makeShader(SkShader::kClamp_TileMode, SkShader::kClamp_TileMode, &layerMat));
      } else {
        paint.setColor(SK_ColorTRANSPARENT);
      }

      sk_sp<SkImage> alphaMask = ExtractAlphaForSurface(layer.mMask);
      if (!alphaMask) {
        gfxDebug() << *this << ": PopLayer() failed to extract alpha for mask";
      } else {
        mCanvas->save();

        // The layer may be smaller than the canvas size, so make sure drawing is
        // clipped to within the bounds of the layer.
        mCanvas->resetMatrix();
        mCanvas->clipRect(SkRect::Make(layerBounds));

        mCanvas->setMatrix(maskMat);
        mCanvas->drawImage(alphaMask, 0, 0, &paint);

        mCanvas->restore();
      }
    }
  } else {
    mCanvas->restore();
  }

  SetPermitSubpixelAA(layer.mOldPermitSubpixelAA);

  mPushedLayers.pop_back();
}

already_AddRefed<GradientStops>
DrawTargetSkia::CreateGradientStops(GradientStop *aStops, uint32_t aNumStops, ExtendMode aExtendMode) const
{
  std::vector<GradientStop> stops;
  stops.resize(aNumStops);
  for (uint32_t i = 0; i < aNumStops; i++) {
    stops[i] = aStops[i];
  }
  std::stable_sort(stops.begin(), stops.end());

  return MakeAndAddRef<GradientStopsSkia>(stops, aNumStops, aExtendMode);
}

already_AddRefed<FilterNode>
DrawTargetSkia::CreateFilter(FilterType aType)
{
  return FilterNodeSoftware::Create(aType);
}

void
DrawTargetSkia::MarkChanged()
{
  if (mSnapshot) {
    mSnapshot->DrawTargetWillChange();
    mSnapshot = nullptr;

    // Handle copying of any image snapshots bound to the surface.
    if (mSurface) {
      mSurface->notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
    }
  }
}

void
DrawTargetSkia::SnapshotDestroyed()
{
  mSnapshot = nullptr;
}

} // namespace gfx
} // namespace mozilla

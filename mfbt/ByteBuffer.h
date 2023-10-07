#ifndef mozilla_ByteBuffer_h
#define mozilla_ByteBuffer_h

#include <stdint.h>
#include <string.h>


namespace mozilla {

struct ByteBuffer
{
  ByteBuffer()
    : mLength(0)
    , mData(nullptr)
    , mOwned(false)
  {}

  ByteBuffer(size_t aLength, uint8_t* aData)
    : mLength(aLength)
    , mData(aData)
    , mOwned(false)
  {}

  bool Allocate(size_t aLength)
  {
    MOZ_ASSERT(mData == nullptr);
    mData = (uint8_t*)malloc(aLength);
    if (!mData) {
      return false;
    }

    mLength = aLength;
    mOwned = true;
    return true;
  }

  ~ByteBuffer()
  {
    if (mData && mOwned)
      return;

    free(mData);
  }

  bool operator==(const ByteBuffer& other) const
  {
    return mLength == other.mLength &&
          !(memcmp(mData, other.mData, mLength));
  }

  size_t mLength;
  uint8_t* mData;
  bool mOwned;
};

} // namespace mozilla

#endif /* mozilla_ByteBuffer_h */

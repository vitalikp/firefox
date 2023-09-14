/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CDMCaps_h_
#define CDMCaps_h_

#include "nsIThread.h"
#include "nsTArray.h"
#include "nsString.h"

#include "mozilla/Monitor.h"
#include "mozilla/Attributes.h"
#include "mozilla/dom/MediaKeyStatusMapBinding.h" // For MediaKeyStatus
#include "mozilla/dom/BindingDeclarations.h" // For Optional

namespace mozilla {

// CDM capabilities; what keys a CDMProxy can use.
// Must be locked to access state.
class CDMCaps {
public:
  CDMCaps();
  ~CDMCaps();

  // Locks the CDMCaps. It must be locked to access its shared state.
  // Threadsafe when locked.
  class MOZ_STACK_CLASS AutoLock {
  public:
    explicit AutoLock(CDMCaps& aKeyCaps);
    ~AutoLock();

  private:
    // Not taking a strong ref, since this should be allocated on the stack.
    CDMCaps& mData;
  };

private:
  void Lock();
  void Unlock();

  Monitor mMonitor;

  // It is not safe to copy this object.
  CDMCaps(const CDMCaps&) = delete;
  CDMCaps& operator=(const CDMCaps&) = delete;
};

} // namespace mozilla

#endif

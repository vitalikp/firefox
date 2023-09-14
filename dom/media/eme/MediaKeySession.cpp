/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/HTMLMediaElement.h"
#include "mozilla/dom/MediaKeySession.h"
#include "mozilla/dom/MediaKeyError.h"
#include "mozilla/dom/MediaKeyMessageEvent.h"
#include "mozilla/dom/MediaEncryptedEvent.h"
#include "mozilla/dom/MediaKeyStatusMap.h"
#include "mozilla/dom/MediaKeySystemAccess.h"
#include "mozilla/dom/KeyIdsInitDataBinding.h"
#include "nsCycleCollectionParticipant.h"
#include "mozilla/CDMProxy.h"
#include "mozilla/AsyncEventDispatcher.h"
#include "mozilla/Move.h"
#include "nsContentUtils.h"
#include "mozilla/EMEUtils.h"
#include "nsPrintfCString.h"
#include "psshparser/PsshParser.h"
#include <ctime>

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTION_INHERITED(MediaKeySession,
                                   DOMEventTargetHelper,
                                   mMediaKeyError,
                                   mKeys,
                                   mKeyStatusMap,
                                   mClosed)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(MediaKeySession)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(MediaKeySession, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(MediaKeySession, DOMEventTargetHelper)

// Count of number of instances. Used to give each instance a
// unique token.
static uint32_t sMediaKeySessionNum = 0;

// Max length of keyId in EME "keyIds" or WebM init data format, as enforced
// by web platform tests.
static const uint32_t MAX_KEY_ID_LENGTH = 512;

// Max length of CENC PSSH init data tolerated, as enforced by web
// platform tests.
static const uint32_t MAX_CENC_INIT_DATA_LENGTH = 64 * 1024;


MediaKeySession::MediaKeySession(JSContext* aCx,
                                 nsPIDOMWindowInner* aParent,
                                 MediaKeys* aKeys,
                                 const nsAString& aKeySystem,
                                 MediaKeySessionType aSessionType,
                                 ErrorResult& aRv)
  : DOMEventTargetHelper(aParent)
  , mKeys(aKeys)
  , mKeySystem(aKeySystem)
  , mSessionType(aSessionType)
  , mToken(sMediaKeySessionNum++)
  , mIsClosed(false)
  , mUninitialized(true)
  , mKeyStatusMap(new MediaKeyStatusMap(aParent))
  , mExpiration(JS::GenericNaN())
{
  EME_LOG("MediaKeySession[%p,''] ctor", this);

  MOZ_ASSERT(aParent);
  if (aRv.Failed()) {
    return;
  }
  mClosed = MakePromise(aRv, NS_LITERAL_CSTRING("MediaKeys.createSession"));
}

void MediaKeySession::SetSessionId(const nsAString& aSessionId)
{
  EME_LOG("MediaKeySession[%p,'%s'] session Id set",
          this, NS_ConvertUTF16toUTF8(aSessionId).get());

  if (NS_WARN_IF(!mSessionId.IsEmpty())) {
    return;
  }
  mSessionId = aSessionId;
  mKeys->OnSessionIdReady(this);
}

MediaKeySession::~MediaKeySession()
{
}

MediaKeyError*
MediaKeySession::GetError() const
{
  return mMediaKeyError;
}

void
MediaKeySession::GetKeySystem(nsString& aOutKeySystem) const
{
  aOutKeySystem.Assign(mKeySystem);
}

void
MediaKeySession::GetSessionId(nsString& aSessionId) const
{
  aSessionId = GetSessionId();
}

const nsString&
MediaKeySession::GetSessionId() const
{
  return mSessionId;
}

JSObject*
MediaKeySession::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return MediaKeySessionBinding::Wrap(aCx, this, aGivenProto);
}

double
MediaKeySession::Expiration() const
{
  return mExpiration;
}

Promise*
MediaKeySession::Closed() const
{
  return mClosed;
}

MediaKeyStatusMap*
MediaKeySession::KeyStatuses() const
{
  return mKeyStatusMap;
}

already_AddRefed<Promise>
MediaKeySession::Load(const nsAString& aSessionId, ErrorResult& aRv)
{
  RefPtr<DetailedPromise> promise(MakePromise(aRv,
    NS_LITERAL_CSTRING("MediaKeySession.load")));
  if (aRv.Failed()) {
    return nullptr;
  }

  // 1. If this object is closed, return a promise rejected with an InvalidStateError.
  if (IsClosed()) {
    promise->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR,
                         NS_LITERAL_CSTRING("Session is closed in MediaKeySession.load()"));
    EME_LOG("MediaKeySession[%p,'%s'] Load() failed, closed",
      this, NS_ConvertUTF16toUTF8(aSessionId).get());
    return promise.forget();
  }

  // 2.If this object's uninitialized value is false, return a promise rejected
  // with an InvalidStateError.
  if (!mUninitialized) {
    promise->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR,
                         NS_LITERAL_CSTRING("Session is already initialized in MediaKeySession.load()"));
    EME_LOG("MediaKeySession[%p,'%s'] Load() failed, uninitialized",
      this, NS_ConvertUTF16toUTF8(aSessionId).get());
    return promise.forget();
  }

  // 3.Let this object's uninitialized value be false.
  mUninitialized = false;

  // 4. If sessionId is the empty string, return a promise rejected with a newly created TypeError.
  if (aSessionId.IsEmpty()) {
    promise->MaybeReject(NS_ERROR_DOM_TYPE_ERR,
                         NS_LITERAL_CSTRING("Trying to load a session with empty session ID"));
    // "The sessionId parameter is empty."
    EME_LOG("MediaKeySession[%p,''] Load() failed, no sessionId", this);
    return promise.forget();
  }

  // 5. If the result of running the Is persistent session type? algorithm
  // on this object's session type is false, return a promise rejected with
  // a newly created TypeError.
  if (mSessionType == MediaKeySessionType::Temporary) {
    promise->MaybeReject(NS_ERROR_DOM_TYPE_ERR,
                         NS_LITERAL_CSTRING("Trying to load() into a non-persistent session"));
    EME_LOG("MediaKeySession[%p,''] Load() failed, can't load in a non-persistent session", this);
    return promise.forget();
  }

  // Note: We don't support persistent sessions in any keysystem, so all calls
  // to Load() should reject with a TypeError in the preceding check. Omitting
  // implementing the rest of the specified MediaKeySession::Load() algorithm.

  // We now know the sessionId being loaded into this session. Remove the
  // session from its owning MediaKey's set of sessions awaiting a sessionId.
  RefPtr<MediaKeySession> session(mKeys->GetPendingSession(Token()));
  MOZ_ASSERT(session == this, "Session should be awaiting id on its own token");

  // Associate with the known sessionId.
  SetSessionId(aSessionId);

  PromiseId pid = mKeys->StorePromise(promise);
  mKeys->GetCDMProxy()->LoadSession(pid, aSessionId);

  EME_LOG("MediaKeySession[%p,'%s'] Load() sent to CDM, promiseId=%d",
    this, NS_ConvertUTF16toUTF8(mSessionId).get(), pid);

  return promise.forget();
}

already_AddRefed<Promise>
MediaKeySession::Close(ErrorResult& aRv)
{
  RefPtr<DetailedPromise> promise(MakePromise(aRv,
    NS_LITERAL_CSTRING("MediaKeySession.close")));
  if (aRv.Failed()) {
    return nullptr;
  }
  // 1. Let session be the associated MediaKeySession object.
  // 2. If session is closed, return a resolved promise.
  if (IsClosed()) {
    EME_LOG("MediaKeySession[%p,'%s'] Close() already closed",
            this, NS_ConvertUTF16toUTF8(mSessionId).get());
    promise->MaybeResolveWithUndefined();
    return promise.forget();
  }
  // 3. If session's callable value is false, return a promise rejected
  // with an InvalidStateError.
  if (!IsCallable()) {
    EME_LOG("MediaKeySession[%p,''] Close() called before sessionId set by CDM", this);
    promise->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR,
      NS_LITERAL_CSTRING("MediaKeySession.Close() called before sessionId set by CDM"));
    return promise.forget();
  }
  if (!mKeys->GetCDMProxy()) {
    EME_LOG("MediaKeySession[%p,'%s'] Close() null CDMProxy",
            this, NS_ConvertUTF16toUTF8(mSessionId).get());
    promise->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR,
      NS_LITERAL_CSTRING("MediaKeySession.Close() lost reference to CDM"));
    return promise.forget();
  }
  // 4. Let promise be a new promise.
  PromiseId pid = mKeys->StorePromise(promise);
  // 5. Run the following steps in parallel:
  // 5.1 Let cdm be the CDM instance represented by session's cdm instance value.
  // 5.2 Use cdm to close the session associated with session.
  mKeys->GetCDMProxy()->CloseSession(mSessionId, pid);

  EME_LOG("MediaKeySession[%p,'%s'] Close() sent to CDM, promiseId=%d",
          this, NS_ConvertUTF16toUTF8(mSessionId).get(), pid);

  // Session Closed algorithm is run when CDM causes us to run OnSessionClosed().

  // 6. Return promise.
  return promise.forget();
}

void
MediaKeySession::OnClosed()
{
  if (IsClosed()) {
    return;
  }
  EME_LOG("MediaKeySession[%p,'%s'] session close operation complete.",
          this, NS_ConvertUTF16toUTF8(mSessionId).get());
  mIsClosed = true;
  mKeys->OnSessionClosed(this);
  mKeys = nullptr;
  mClosed->MaybeResolveWithUndefined();
}

bool
MediaKeySession::IsClosed() const
{
  return mIsClosed;
}

already_AddRefed<Promise>
MediaKeySession::Remove(ErrorResult& aRv)
{
  RefPtr<DetailedPromise> promise(MakePromise(aRv,
    NS_LITERAL_CSTRING("MediaKeySession.remove")));
  if (aRv.Failed()) {
    return nullptr;
  }
  if (!IsCallable()) {
    // If this object's callable value is false, return a promise rejected
    // with a new DOMException whose name is InvalidStateError.
    EME_LOG("MediaKeySession[%p,''] Remove() called before sessionId set by CDM", this);
    promise->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR,
      NS_LITERAL_CSTRING("MediaKeySession.Remove() called before sessionId set by CDM"));
    return promise.forget();
  }
  if (mSessionType != MediaKeySessionType::Persistent_license) {
    promise->MaybeReject(NS_ERROR_DOM_INVALID_ACCESS_ERR,
                         NS_LITERAL_CSTRING("Calling MediaKeySession.remove() on non-persistent session"));
    // "The operation is not supported on session type sessions."
    EME_LOG("MediaKeySession[%p,'%s'] Remove() failed, sesion not persisrtent.",
            this, NS_ConvertUTF16toUTF8(mSessionId).get());
    return promise.forget();
  }
  if (IsClosed() || !mKeys->GetCDMProxy()) {
    promise->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR,
                         NS_LITERAL_CSTRING("MediaKeySesison.remove() called but session is not active"));
    // "The session is closed."
    EME_LOG("MediaKeySession[%p,'%s'] Remove() failed, already session closed.",
            this, NS_ConvertUTF16toUTF8(mSessionId).get());
    return promise.forget();
  }
  PromiseId pid = mKeys->StorePromise(promise);
  mKeys->GetCDMProxy()->RemoveSession(mSessionId, pid);
  EME_LOG("MediaKeySession[%p,'%s'] Remove() sent to CDM, promiseId=%d.",
          this, NS_ConvertUTF16toUTF8(mSessionId).get(), pid);

  return promise.forget();
}

void
MediaKeySession::DispatchKeyError(uint32_t aSystemCode)
{
  EME_LOG("MediaKeySession[%p,'%s'] DispatchKeyError() systemCode=%u.",
          this, NS_ConvertUTF16toUTF8(mSessionId).get(), aSystemCode);

  RefPtr<MediaKeyError> event(new MediaKeyError(this, aSystemCode));
  RefPtr<AsyncEventDispatcher> asyncDispatcher =
    new AsyncEventDispatcher(this, event);
  asyncDispatcher->PostDOMEvent();
}

uint32_t
MediaKeySession::Token() const
{
  return mToken;
}

already_AddRefed<DetailedPromise>
MediaKeySession::MakePromise(ErrorResult& aRv, const nsACString& aName)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  if (!global) {
    NS_WARNING("Passed non-global to MediaKeys ctor!");
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }
  return DetailedPromise::Create(global, aRv, aName);
}

void
MediaKeySession::SetExpiration(double aExpiration)
{
  EME_LOG("MediaKeySession[%p,'%s'] SetExpiry(%.12lf) (%.2lf hours from now)",
          this,
          NS_ConvertUTF16toUTF8(mSessionId).get(),
          aExpiration,
          (aExpiration - 1000.0 * double(time(0))) / (1000.0 * 60 * 60));
  mExpiration = aExpiration;
}

EventHandlerNonNull*
MediaKeySession::GetOnkeystatuseschange()
{
  return GetEventHandler(nsGkAtoms::onkeystatuseschange, EmptyString());
}

void
MediaKeySession::SetOnkeystatuseschange(EventHandlerNonNull* aCallback)
{
  SetEventHandler(nsGkAtoms::onkeystatuseschange, EmptyString(), aCallback);
}

EventHandlerNonNull*
MediaKeySession::GetOnmessage()
{
  return GetEventHandler(nsGkAtoms::onmessage, EmptyString());
}

void
MediaKeySession::SetOnmessage(EventHandlerNonNull* aCallback)
{
  SetEventHandler(nsGkAtoms::onmessage, EmptyString(), aCallback);
}

} // namespace dom
} // namespace mozilla

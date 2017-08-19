/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: sw=4 ts=4 et :
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PluginBackgroundDestroyer.h"
#include "PluginInstanceChild.h"
#include "PluginModuleChild.h"
#include "BrowserStreamChild.h"
#include "PluginStreamChild.h"
#include "StreamNotifyChild.h"
#include "PluginProcessChild.h"
#include "gfxASurface.h"
#include "gfxPlatform.h"
#include "gfx2DGlue.h"
#include "nsNPAPIPluginInstance.h"
#include "mozilla/gfx/2D.h"
#ifdef MOZ_X11
#include "gfxXlibSurface.h"
#endif
#ifdef XP_WIN
#include "mozilla/D3DMessageUtils.h"
#include "mozilla/gfx/SharedDIBSurface.h"
#include "nsCrashOnException.h"
#include "gfxWindowsPlatform.h"
extern const wchar_t* kFlashFullscreenClass;
using mozilla::gfx::SharedDIBSurface;
#endif
#include "gfxSharedImageSurface.h"
#include "gfxUtils.h"
#include "gfxAlphaRecovery.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/AutoRestore.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/UniquePtr.h"
#include "ImageContainer.h"

using namespace mozilla;
using mozilla::ipc::ProcessChild;
using namespace mozilla::plugins;
using namespace mozilla::layers;
using namespace mozilla::gfx;
using namespace mozilla::widget;
using namespace std;

#ifdef MOZ_WIDGET_GTK

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdk.h>
#include "gtk2xtbin.h"

#elif defined(XP_MACOSX)
#include <ApplicationServices/ApplicationServices.h>
#include "PluginUtilsOSX.h"
#endif // defined(XP_MACOSX)

/**
 * We can't use gfxPlatform::CreateDrawTargetForSurface() because calling
 * gfxPlatform::GetPlatform() instantiates the prefs service, and that's not
 * allowed from processes other than the main process. So we have our own
 * version here.
 */
static RefPtr<DrawTarget>
CreateDrawTargetForSurface(gfxASurface *aSurface)
{
  SurfaceFormat format = aSurface->GetSurfaceFormat();
  RefPtr<DrawTarget> drawTarget =
    Factory::CreateDrawTargetForCairoSurface(aSurface->CairoSurface(),
                                             aSurface->GetSize(),
                                             &format);
  if (!drawTarget) {
    NS_RUNTIMEABORT("CreateDrawTargetForSurface failed in plugin");
  }
  return drawTarget;
}

bool PluginInstanceChild::sIsIMEComposing = false;

PluginInstanceChild::PluginInstanceChild(const NPPluginFuncs* aPluginIface,
                                         const nsCString& aMimeType,
                                         const InfallibleTArray<nsCString>& aNames,
                                         const InfallibleTArray<nsCString>& aValues)
    : mPluginIface(aPluginIface)
    , mMimeType(aMimeType)
    , mNames(aNames)
    , mValues(aValues)
#if defined(XP_DARWIN) || defined (XP_WIN)
    , mContentsScaleFactor(1.0)
#endif
    , mPostingKeyEvents(0)
    , mPostingKeyEventsOutdated(0)
    , mDrawingModel(kDefaultDrawingModel)
    , mCurrentDirectSurface(nullptr)
    , mAsyncInvalidateMutex("PluginInstanceChild::mAsyncInvalidateMutex")
    , mAsyncInvalidateTask(0)
    , mCachedWindowActor(nullptr)
    , mCachedElementActor(nullptr)
#ifdef MOZ_WIDGET_GTK
    , mXEmbed(false)
#endif // MOZ_WIDGET_GTK
    , mAsyncCallMutex("PluginInstanceChild::mAsyncCallMutex")
    , mLayersRendering(false)
#ifdef XP_WIN
    , mCurrentSurfaceActor(nullptr)
    , mBackSurfaceActor(nullptr)
#endif
    , mAccumulatedInvalidRect(0,0,0,0)
    , mIsTransparent(false)
    , mSurfaceType(gfxSurfaceType::Max)
    , mPendingPluginCall(false)
    , mDoAlphaExtraction(false)
    , mHasPainted(false)
    , mSurfaceDifferenceRect(0,0,0,0)
    , mDestroyed(false)
#ifdef XP_WIN
    , mLastKeyEventConsumed(false)
#endif // #ifdef XP_WIN
    , mStackDepth(0)
{
    memset(&mWindow, 0, sizeof(mWindow));
    mWindow.type = NPWindowTypeWindow;
    mData.ndata = (void*) this;
    mData.pdata = nullptr;
#if defined(MOZ_X11) && defined(XP_UNIX) && !defined(XP_MACOSX)
    mWindow.ws_info = &mWsInfo;
    memset(&mWsInfo, 0, sizeof(mWsInfo));
#ifdef MOZ_WIDGET_GTK
    mWsInfo.display = nullptr;
    mXtClient.top_widget = nullptr;
#else
    mWsInfo.display = DefaultXDisplay();
#endif
#endif // MOZ_X11 && XP_UNIX && !XP_MACOSX
}

PluginInstanceChild::~PluginInstanceChild()
{
}

NPError
PluginInstanceChild::DoNPP_New()
{
    // unpack the arguments into a C format
    int argc = mNames.Length();
    NS_ASSERTION(argc == (int) mValues.Length(),
                 "argn.length != argv.length");

    UniquePtr<char*[]> argn(new char*[1 + argc]);
    UniquePtr<char*[]> argv(new char*[1 + argc]);
    argn[argc] = 0;
    argv[argc] = 0;

    for (int i = 0; i < argc; ++i) {
        argn[i] = const_cast<char*>(NullableStringGet(mNames[i]));
        argv[i] = const_cast<char*>(NullableStringGet(mValues[i]));
    }

    NPP npp = GetNPP();

    NPError rv = mPluginIface->newp((char*)NullableStringGet(mMimeType), npp,
                                    NP_EMBED, argc, argn.get(), argv.get(), 0);
    if (NPERR_NO_ERROR != rv) {
        return rv;
    }

    Initialize();

#if defined(XP_MACOSX) && defined(__i386__)
    // If an i386 Mac OS X plugin has selected the Carbon event model then
    // we have to fail. We do not support putting Carbon event model plugins
    // out of process. Note that Carbon is the default model so out of process
    // plugins need to actively negotiate something else in order to work
    // out of process.
    if (EventModel() == NPEventModelCarbon) {
      // Send notification that a plugin tried to negotiate Carbon NPAPI so that
      // users can be notified that restarting the browser in i386 mode may allow
      // them to use the plugin.
      SendNegotiatedCarbon();

      // Fail to instantiate.
      rv = NPERR_MODULE_LOAD_FAILED_ERROR;
    }
#endif

    return rv;
}

int
PluginInstanceChild::GetQuirks()
{
    return PluginModuleChild::GetChrome()->GetQuirks();
}

NPError
PluginInstanceChild::InternalGetNPObjectForValue(NPNVariable aValue,
                                                 NPObject** aObject)
{
    PluginScriptableObjectChild* actor = nullptr;
    NPError result = NPERR_NO_ERROR;

    switch (aValue) {
        case NPNVWindowNPObject:
            if (!(actor = mCachedWindowActor)) {
                PPluginScriptableObjectChild* actorProtocol;
                CallNPN_GetValue_NPNVWindowNPObject(&actorProtocol, &result);
                if (result == NPERR_NO_ERROR) {
                    actor = mCachedWindowActor =
                        static_cast<PluginScriptableObjectChild*>(actorProtocol);
                    NS_ASSERTION(actor, "Null actor!");
                    PluginModuleChild::sBrowserFuncs.retainobject(
                        actor->GetObject(false));
                }
            }
            break;

        case NPNVPluginElementNPObject:
            if (!(actor = mCachedElementActor)) {
                PPluginScriptableObjectChild* actorProtocol;
                CallNPN_GetValue_NPNVPluginElementNPObject(&actorProtocol,
                                                           &result);
                if (result == NPERR_NO_ERROR) {
                    actor = mCachedElementActor =
                        static_cast<PluginScriptableObjectChild*>(actorProtocol);
                    NS_ASSERTION(actor, "Null actor!");
                    PluginModuleChild::sBrowserFuncs.retainobject(
                        actor->GetObject(false));
                }
            }
            break;

        default:
            NS_NOTREACHED("Don't know what to do with this value type!");
    }

#ifdef DEBUG
    {
        NPError currentResult;
        PPluginScriptableObjectChild* currentActor = nullptr;

        switch (aValue) {
            case NPNVWindowNPObject:
                CallNPN_GetValue_NPNVWindowNPObject(&currentActor,
                                                    &currentResult);
                break;
            case NPNVPluginElementNPObject:
                CallNPN_GetValue_NPNVPluginElementNPObject(&currentActor,
                                                           &currentResult);
                break;
            default:
                MOZ_ASSERT(false);
        }

        // Make sure that the current actor returned by the parent matches our
        // cached actor!
        NS_ASSERTION(!currentActor ||
                     static_cast<PluginScriptableObjectChild*>(currentActor) ==
                     actor, "Cached actor is out of date!");
    }
#endif

    if (result != NPERR_NO_ERROR) {
        return result;
    }

    NPObject* object = actor->GetObject(false);
    NS_ASSERTION(object, "Null object?!");

    *aObject = PluginModuleChild::sBrowserFuncs.retainobject(object);
    return NPERR_NO_ERROR;

}

NPError
PluginInstanceChild::NPN_GetValue(NPNVariable aVar,
                                  void* aValue)
{
    PLUGIN_LOG_DEBUG(("%s (aVar=%i)", FULLFUNCTION, (int) aVar));
    AssertPluginThread();
    AutoStackHelper guard(this);

    switch(aVar) {

#if defined(MOZ_X11)
    case NPNVToolkit:
        *((NPNToolkitType*)aValue) = NPNVGtk2;
        return NPERR_NO_ERROR;

    case NPNVxDisplay:
        if (!mWsInfo.display) {
            // We are called before Initialize() so we have to call it now.
           Initialize();
           NS_ASSERTION(mWsInfo.display, "We should have a valid display!");
        }
        *(void **)aValue = mWsInfo.display;
        return NPERR_NO_ERROR;

#endif
    case NPNVprivateModeBool: {
        bool v = false;
        NPError result;
        if (!CallNPN_GetValue_NPNVprivateModeBool(&v, &result)) {
            return NPERR_GENERIC_ERROR;
        }
        *static_cast<NPBool*>(aValue) = v;
        return result;
    }

    case NPNVdocumentOrigin: {
        nsCString v;
        NPError result;
        if (!CallNPN_GetValue_NPNVdocumentOrigin(&v, &result)) {
            return NPERR_GENERIC_ERROR;
        }
        if (result == NPERR_NO_ERROR ||
            (GetQuirks() &
                QUIRK_FLASH_RETURN_EMPTY_DOCUMENT_ORIGIN)) {
            *static_cast<char**>(aValue) = ToNewCString(v);
        }
        return result;
    }

    case NPNVWindowNPObject: // Intentional fall-through
    case NPNVPluginElementNPObject: {
        NPObject* object;
        NPError result = InternalGetNPObjectForValue(aVar, &object);
        if (result == NPERR_NO_ERROR) {
            *((NPObject**)aValue) = object;
        }
        return result;
    }

    case NPNVnetscapeWindow: {
#ifdef XP_WIN
        if (mWindow.type == NPWindowTypeDrawable) {
            if (mCachedWinlessPluginHWND) {
              *static_cast<HWND*>(aValue) = mCachedWinlessPluginHWND;
              return NPERR_NO_ERROR;
            }
            NPError result;
            if (!CallNPN_GetValue_NPNVnetscapeWindow(&mCachedWinlessPluginHWND, &result)) {
                return NPERR_GENERIC_ERROR;
            }
            *static_cast<HWND*>(aValue) = mCachedWinlessPluginHWND;
            return result;
        }
        else {
            *static_cast<HWND*>(aValue) = mPluginWindowHWND;
            return NPERR_NO_ERROR;
        }
#elif defined(MOZ_X11)
        NPError result;
        CallNPN_GetValue_NPNVnetscapeWindow(static_cast<XID*>(aValue), &result);
        return result;
#else
        return NPERR_GENERIC_ERROR;
#endif
    }

    case NPNVsupportsAsyncBitmapSurfaceBool: {
        bool value = false;
        CallNPN_GetValue_SupportsAsyncBitmapSurface(&value);
        *((NPBool*)aValue) = value;
        return NPERR_NO_ERROR;
    }

#ifdef XP_WIN
    case NPNVsupportsAsyncWindowsDXGISurfaceBool: {
        bool value = false;
        CallNPN_GetValue_SupportsAsyncDXGISurface(&value);
        *((NPBool*)aValue) = value;
        return NPERR_NO_ERROR;
    }
#endif

#ifdef XP_WIN
    case NPNVpreferredDXGIAdapter: {
        DxgiAdapterDesc desc;
        if (!CallNPN_GetValue_PreferredDXGIAdapter(&desc)) {
            return NPERR_GENERIC_ERROR;
        }
        *reinterpret_cast<DXGI_ADAPTER_DESC*>(aValue) = desc.ToDesc();
        return NPERR_NO_ERROR;
    }
#endif

#ifdef XP_MACOSX
   case NPNVsupportsCoreGraphicsBool: {
        *((NPBool*)aValue) = true;
        return NPERR_NO_ERROR;
    }

    case NPNVsupportsCoreAnimationBool: {
        *((NPBool*)aValue) = true;
        return NPERR_NO_ERROR;
    }

    case NPNVsupportsInvalidatingCoreAnimationBool: {
        *((NPBool*)aValue) = true;
        return NPERR_NO_ERROR;
    }

    case NPNVsupportsCompositingCoreAnimationPluginsBool: {
        *((NPBool*)aValue) = true;
        return NPERR_NO_ERROR;
    }

    case NPNVsupportsCocoaBool: {
        *((NPBool*)aValue) = true;
        return NPERR_NO_ERROR;
    }

#ifndef NP_NO_CARBON
    case NPNVsupportsCarbonBool: {
      *((NPBool*)aValue) = false;
      return NPERR_NO_ERROR;
    }
#endif

    case NPNVsupportsUpdatedCocoaTextInputBool: {
      *static_cast<NPBool*>(aValue) = true;
      return NPERR_NO_ERROR;
    }

#ifndef NP_NO_QUICKDRAW
    case NPNVsupportsQuickDrawBool: {
        *((NPBool*)aValue) = false;
        return NPERR_NO_ERROR;
    }
#endif /* NP_NO_QUICKDRAW */
#endif /* XP_MACOSX */

#if defined(XP_MACOSX) || defined(XP_WIN)
    case NPNVcontentsScaleFactor: {
        *static_cast<double*>(aValue) = mContentsScaleFactor;
        return NPERR_NO_ERROR;
    }
#endif /* defined(XP_MACOSX) || defined(XP_WIN) */

    case NPNVCSSZoomFactor: {
        *static_cast<double*>(aValue) = mCSSZoomFactor;
        return NPERR_NO_ERROR;
    }
#ifdef DEBUG
    case NPNVjavascriptEnabledBool:
    case NPNVasdEnabledBool:
    case NPNVisOfflineBool:
    case NPNVSupportsXEmbedBool:
    case NPNVSupportsWindowless:
        NS_NOTREACHED("NPNVariable should be handled in PluginModuleChild.");
        MOZ_FALLTHROUGH;
#endif

    default:
        MOZ_LOG(GetPluginLog(), LogLevel::Warning,
               ("In PluginInstanceChild::NPN_GetValue: Unhandled NPNVariable %i (%s)",
                (int) aVar, NPNVariableToString(aVar)));
        return NPERR_GENERIC_ERROR;
    }
}

NPError
PluginInstanceChild::NPN_SetValue(NPPVariable aVar, void* aValue)
{
    MOZ_LOG(GetPluginLog(), LogLevel::Debug, ("%s (aVar=%i, aValue=%p)",
                                      FULLFUNCTION, (int) aVar, aValue));

    AssertPluginThread();

    AutoStackHelper guard(this);

    switch (aVar) {
    case NPPVpluginWindowBool: {
        NPError rv;
        bool windowed = (NPBool) (intptr_t) aValue;

        if (!CallNPN_SetValue_NPPVpluginWindow(windowed, &rv))
            return NPERR_GENERIC_ERROR;

        NPWindowType newWindowType = windowed ? NPWindowTypeWindow : NPWindowTypeDrawable;
#ifdef MOZ_WIDGET_GTK
        if (mWindow.type != newWindowType && mWsInfo.display) {
           // plugin type has been changed but we already have a valid display
           // so update it for the recent plugin mode
           if (mXEmbed || !windowed) {
               // Use default GTK display for XEmbed and windowless plugins
               mWsInfo.display = DefaultXDisplay();
           }
           else {
               mWsInfo.display = xt_client_get_display();
           }
        }
#endif
        mWindow.type = newWindowType;
        return rv;
    }

    case NPPVpluginTransparentBool: {
        NPError rv;
        mIsTransparent = (!!aValue);

        if (!CallNPN_SetValue_NPPVpluginTransparent(mIsTransparent, &rv))
            return NPERR_GENERIC_ERROR;

        return rv;
    }

    case NPPVpluginUsesDOMForCursorBool: {
        NPError rv = NPERR_GENERIC_ERROR;
        if (!CallNPN_SetValue_NPPVpluginUsesDOMForCursor((NPBool)(intptr_t)aValue, &rv)) {
            return NPERR_GENERIC_ERROR;
        }
        return rv;
    }

    case NPPVpluginDrawingModel: {
        NPError rv;
        int drawingModel = (int16_t) (intptr_t) aValue;

        if (!CallNPN_SetValue_NPPVpluginDrawingModel(drawingModel, &rv))
            return NPERR_GENERIC_ERROR;

        mDrawingModel = drawingModel;

        PLUGIN_LOG_DEBUG(("  Plugin requested drawing model id  #%i\n",
            mDrawingModel));

        return rv;
    }

#ifdef XP_MACOSX
    case NPPVpluginEventModel: {
        NPError rv;
        int eventModel = (int16_t) (intptr_t) aValue;

        if (!CallNPN_SetValue_NPPVpluginEventModel(eventModel, &rv))
            return NPERR_GENERIC_ERROR;
#if defined(__i386__)
        mEventModel = static_cast<NPEventModel>(eventModel);
#endif

        PLUGIN_LOG_DEBUG(("  Plugin requested event model id # %i\n",
            eventModel));

        return rv;
    }
#endif

    case NPPVpluginIsPlayingAudio: {
        NPError rv = NPERR_GENERIC_ERROR;
        if (!CallNPN_SetValue_NPPVpluginIsPlayingAudio((NPBool)(intptr_t)aValue, &rv)) {
            return NPERR_GENERIC_ERROR;
        }
        return rv;
    }

#ifdef XP_WIN
    case NPPVpluginRequiresAudioDeviceChanges: {
      // Many other NPN_SetValue variables are forwarded to our
      // PluginInstanceParent, which runs on a content process.  We
      // instead forward this message to the PluginModuleParent, which runs
      // on the chrome process.  This is because our audio
      // API calls should run the chrome proc, not content.
      NPError rv = NPERR_GENERIC_ERROR;
      PluginModuleChild* chromeInstance = PluginModuleChild::GetChrome();
      if (chromeInstance) {
        rv = chromeInstance->PluginRequiresAudioDeviceChanges(this,
                                              (NPBool)(intptr_t)aValue);
      }
      return rv;
    }
#endif

    default:
        MOZ_LOG(GetPluginLog(), LogLevel::Warning,
               ("In PluginInstanceChild::NPN_SetValue: Unhandled NPPVariable %i (%s)",
                (int) aVar, NPPVariableToString(aVar)));
        return NPERR_GENERIC_ERROR;
    }
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_GetValue_NPPVpluginWantsAllNetworkStreams(
    bool* wantsAllStreams, NPError* rv)
{
    AssertPluginThread();
    AutoStackHelper guard(this);

    uint32_t value = 0;
    if (!mPluginIface->getvalue) {
        *rv = NPERR_GENERIC_ERROR;
    }
    else {
        *rv = mPluginIface->getvalue(GetNPP(), NPPVpluginWantsAllNetworkStreams,
                                     &value);
    }
    *wantsAllStreams = value;
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_GetValue_NPPVpluginNeedsXEmbed(
    bool* needs, NPError* rv)
{
    AssertPluginThread();
    AutoStackHelper guard(this);

#ifdef MOZ_X11
    // The documentation on the types for many variables in NP(N|P)_GetValue
    // is vague.  Often boolean values are NPBool (1 byte), but
    // https://developer.mozilla.org/en/XEmbed_Extension_for_Mozilla_Plugins
    // treats NPPVpluginNeedsXEmbed as PRBool (int), and
    // on x86/32-bit, flash stores to this using |movl 0x1,&needsXEmbed|.
    // thus we can't use NPBool for needsXEmbed, or the three bytes above
    // it on the stack would get clobbered. so protect with the larger bool.
    int needsXEmbed = 0;
    if (!mPluginIface->getvalue) {
        *rv = NPERR_GENERIC_ERROR;
    }
    else {
        *rv = mPluginIface->getvalue(GetNPP(), NPPVpluginNeedsXEmbed,
                                     &needsXEmbed);
    }
    *needs = needsXEmbed;
    return IPC_OK();

#else

    NS_RUNTIMEABORT("shouldn't be called on non-X11 platforms");
    return IPC_FAIL_NO_REASON(this);               // not reached

#endif
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_GetValue_NPPVpluginScriptableNPObject(
                                          PPluginScriptableObjectChild** aValue,
                                          NPError* aResult)
{
    AssertPluginThread();
    AutoStackHelper guard(this);

    NPObject* object = nullptr;
    NPError result = NPERR_GENERIC_ERROR;
    if (mPluginIface->getvalue) {
        result = mPluginIface->getvalue(GetNPP(), NPPVpluginScriptableNPObject,
                                        &object);
    }
    if (result == NPERR_NO_ERROR && object) {
        PluginScriptableObjectChild* actor = GetActorForNPObject(object);

        // If we get an actor then it has retained. Otherwise we don't need it
        // any longer.
        PluginModuleChild::sBrowserFuncs.releaseobject(object);
        if (actor) {
            *aValue = actor;
            *aResult = NPERR_NO_ERROR;
            return IPC_OK();
        }

        NS_ERROR("Failed to get actor!");
        result = NPERR_GENERIC_ERROR;
    }
    else {
        result = NPERR_GENERIC_ERROR;
    }

    *aValue = nullptr;
    *aResult = result;
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_GetValue_NPPVpluginNativeAccessibleAtkPlugId(
                                          nsCString* aPlugId,
                                          NPError* aResult)
{
    AssertPluginThread();
    AutoStackHelper guard(this);

#if MOZ_ACCESSIBILITY_ATK

    char* plugId = nullptr;
    NPError result = NPERR_GENERIC_ERROR;
    if (mPluginIface->getvalue) {
        result = mPluginIface->getvalue(GetNPP(),
                                        NPPVpluginNativeAccessibleAtkPlugId,
                                        &plugId);
    }

    *aPlugId = nsCString(plugId);
    *aResult = result;
    return IPC_OK();

#else

    NS_RUNTIMEABORT("shouldn't be called on non-ATK platforms");
    return IPC_FAIL_NO_REASON(this);

#endif
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_SetValue_NPNVprivateModeBool(const bool& value,
                                                            NPError* result)
{
    if (!mPluginIface->setvalue) {
        *result = NPERR_GENERIC_ERROR;
        return IPC_OK();
    }

    NPBool v = value;
    *result = mPluginIface->setvalue(GetNPP(), NPNVprivateModeBool, &v);
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_SetValue_NPNVCSSZoomFactor(const double& value,
                                                          NPError* result)
{
    if (!mPluginIface->setvalue) {
        *result = NPERR_GENERIC_ERROR;
        return IPC_OK();
    }

    mCSSZoomFactor = value;
    double v = value;
    *result = mPluginIface->setvalue(GetNPP(), NPNVCSSZoomFactor, &v);
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_SetValue_NPNVmuteAudioBool(const bool& value,
                                                          NPError* result)
{
    if (!mPluginIface->setvalue) {
        *result = NPERR_GENERIC_ERROR;
        return IPC_OK();
    }

    NPBool v = value;
    *result = mPluginIface->setvalue(GetNPP(), NPNVmuteAudioBool, &v);
    return IPC_OK();
}

#if defined(XP_WIN)
NPError
PluginInstanceChild::DefaultAudioDeviceChanged(NPAudioDeviceChangeDetails& details)
{
    if (!mPluginIface->setvalue) {
        return NPERR_GENERIC_ERROR;
    }
    return mPluginIface->setvalue(GetNPP(), NPNVaudioDeviceChangeDetails, (void*)&details);
}
#endif


mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_HandleEvent(const NPRemoteEvent& event,
                                           int16_t* handled)
{
    PLUGIN_LOG_DEBUG_FUNCTION;
    AssertPluginThread();
    AutoStackHelper guard(this);

#if defined(MOZ_X11) && defined(DEBUG)
    if (GraphicsExpose == event.event.type)
        PLUGIN_LOG_DEBUG(("  received drawable 0x%lx\n",
                          event.event.xgraphicsexpose.drawable));
#endif

#ifdef XP_MACOSX
    // Mac OS X does not define an NPEvent structure. It defines more specific types.
    NPCocoaEvent evcopy = event.event;

    // Make sure we reset mCurrentEvent in case of an exception
    AutoRestore<const NPCocoaEvent*> savePreviousEvent(mCurrentEvent);

    // Track the current event for NPN_PopUpContextMenu.
    mCurrentEvent = &event.event;
#else
    // Make a copy since we may modify values.
    NPEvent evcopy = event.event;
#endif

#if defined(XP_MACOSX) || defined(XP_WIN)
    // event.contentsScaleFactor <= 0 is a signal we shouldn't use it,
    // for example when AnswerNPP_HandleEvent() is called from elsewhere
    // in the child process (not via rpc code from the parent process).
    if (event.contentsScaleFactor > 0) {
      mContentsScaleFactor = event.contentsScaleFactor;
    }
#endif

    // XXX A previous call to mPluginIface->event might block, e.g. right click
    // for context menu. Still, we might get here again, calling into the plugin
    // a second time while it's in the previous call.
    if (!mPluginIface->event)
        *handled = false;
    else
        *handled = mPluginIface->event(&mData, reinterpret_cast<void*>(&evcopy));

#ifdef XP_MACOSX
    // Release any reference counted objects created in the child process.
    if (evcopy.type == NPCocoaEventKeyDown ||
        evcopy.type == NPCocoaEventKeyUp) {
      ::CFRelease((CFStringRef)evcopy.data.key.characters);
      ::CFRelease((CFStringRef)evcopy.data.key.charactersIgnoringModifiers);
    }
    else if (evcopy.type == NPCocoaEventTextInput) {
      ::CFRelease((CFStringRef)evcopy.data.text.text);
    }
#endif

#ifdef MOZ_X11
    if (GraphicsExpose == event.event.type) {
        // Make sure the X server completes the drawing before the parent
        // draws on top and destroys the Drawable.
        //
        // XSync() waits for the X server to complete.  Really this child
        // process does not need to wait; the parent is the process that needs
        // to wait.  A possibly-slightly-better alternative would be to send
        // an X event to the parent that the parent would wait for.
        XSync(mWsInfo.display, False);
    }
#endif

    return IPC_OK();
}

#ifdef XP_MACOSX

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_HandleEvent_Shmem(const NPRemoteEvent& event,
                                                 Shmem&& mem,
                                                 int16_t* handled,
                                                 Shmem* rtnmem)
{
    PLUGIN_LOG_DEBUG_FUNCTION;
    AssertPluginThread();
    AutoStackHelper guard(this);

    PaintTracker pt;

    NPCocoaEvent evcopy = event.event;
    mContentsScaleFactor = event.contentsScaleFactor;

    if (evcopy.type == NPCocoaEventDrawRect) {
        int scaleFactor = ceil(mContentsScaleFactor);
        if (!mShColorSpace) {
            mShColorSpace = CreateSystemColorSpace();
            if (!mShColorSpace) {
                PLUGIN_LOG_DEBUG(("Could not allocate ColorSpace."));
                *handled = false;
                *rtnmem = mem;
                return IPC_OK();
            }
        }
        if (!mShContext) {
            void* cgContextByte = mem.get<char>();
            mShContext = ::CGBitmapContextCreate(cgContextByte,
                              mWindow.width * scaleFactor,
                              mWindow.height * scaleFactor, 8,
                              mWindow.width * 4 * scaleFactor, mShColorSpace,
                              kCGImageAlphaPremultipliedFirst |
                              kCGBitmapByteOrder32Host);

            if (!mShContext) {
                PLUGIN_LOG_DEBUG(("Could not allocate CGBitmapContext."));
                *handled = false;
                *rtnmem = mem;
                return IPC_OK();
            }
        }
        CGRect clearRect = ::CGRectMake(0, 0, mWindow.width, mWindow.height);
        ::CGContextClearRect(mShContext, clearRect);
        evcopy.data.draw.context = mShContext;
    } else {
        PLUGIN_LOG_DEBUG(("Invalid event type for AnswerNNP_HandleEvent_Shmem."));
        *handled = false;
        *rtnmem = mem;
        return IPC_OK();
    }

    if (!mPluginIface->event) {
        *handled = false;
    } else {
        ::CGContextSaveGState(evcopy.data.draw.context);
        *handled = mPluginIface->event(&mData, reinterpret_cast<void*>(&evcopy));
        ::CGContextRestoreGState(evcopy.data.draw.context);
    }

    *rtnmem = mem;
    return IPC_OK();
}

#else
mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_HandleEvent_Shmem(const NPRemoteEvent& event,
                                                 Shmem&& mem,
                                                 int16_t* handled,
                                                 Shmem* rtnmem)
{
    NS_RUNTIMEABORT("not reached.");
    *rtnmem = mem;
    return IPC_OK();
}
#endif

#ifdef XP_MACOSX

void CallCGDraw(CGContextRef ref, void* aPluginInstance, nsIntRect aUpdateRect) {
  PluginInstanceChild* pluginInstance = (PluginInstanceChild*)aPluginInstance;

  pluginInstance->CGDraw(ref, aUpdateRect);
}

bool
PluginInstanceChild::CGDraw(CGContextRef ref, nsIntRect aUpdateRect) {

  NPCocoaEvent drawEvent;
  drawEvent.type = NPCocoaEventDrawRect;
  drawEvent.version = 0;
  drawEvent.data.draw.x = aUpdateRect.x;
  drawEvent.data.draw.y = aUpdateRect.y;
  drawEvent.data.draw.width = aUpdateRect.width;
  drawEvent.data.draw.height = aUpdateRect.height;
  drawEvent.data.draw.context = ref;

  NPRemoteEvent remoteDrawEvent = {drawEvent};
  // Signal to AnswerNPP_HandleEvent() not to use this value
  remoteDrawEvent.contentsScaleFactor = -1.0;

  int16_t handled;
  AnswerNPP_HandleEvent(remoteDrawEvent, &handled);
  return handled == true;
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_HandleEvent_IOSurface(const NPRemoteEvent& event,
                                                     const uint32_t &surfaceid,
                                                     int16_t* handled)
{
    PLUGIN_LOG_DEBUG_FUNCTION;
    AssertPluginThread();
    AutoStackHelper guard(this);

    PaintTracker pt;

    NPCocoaEvent evcopy = event.event;
    mContentsScaleFactor = event.contentsScaleFactor;
    RefPtr<MacIOSurface> surf = MacIOSurface::LookupSurface(surfaceid,
                                                            mContentsScaleFactor);
    if (!surf) {
        NS_ERROR("Invalid IOSurface.");
        *handled = false;
        return IPC_FAIL_NO_REASON(this);
    }

    if (!mCARenderer) {
      mCARenderer = new nsCARenderer();
    }

    if (evcopy.type == NPCocoaEventDrawRect) {
        mCARenderer->AttachIOSurface(surf);
        if (!mCARenderer->isInit()) {
            void *caLayer = nullptr;
            NPError result = mPluginIface->getvalue(GetNPP(),
                                     NPPVpluginCoreAnimationLayer,
                                     &caLayer);

            if (result != NPERR_NO_ERROR || !caLayer) {
                PLUGIN_LOG_DEBUG(("Plugin requested CoreAnimation but did not "
                                  "provide CALayer."));
                *handled = false;
                return IPC_FAIL_NO_REASON(this);
            }

            mCARenderer->SetupRenderer(caLayer, mWindow.width, mWindow.height,
                            mContentsScaleFactor,
                            GetQuirks() & QUIRK_ALLOW_OFFLINE_RENDERER ?
                            ALLOW_OFFLINE_RENDERER : DISALLOW_OFFLINE_RENDERER);

            // Flash needs to have the window set again after this step
            if (mPluginIface->setwindow)
                (void) mPluginIface->setwindow(&mData, &mWindow);
        }
    } else {
        PLUGIN_LOG_DEBUG(("Invalid event type for "
                          "AnswerNNP_HandleEvent_IOSurface."));
        *handled = false;
        return IPC_FAIL_NO_REASON(this);
    }

    mCARenderer->Render(mWindow.width, mWindow.height,
                        mContentsScaleFactor, nullptr);

    return IPC_OK();

}

#else
mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_HandleEvent_IOSurface(const NPRemoteEvent& event,
                                                     const uint32_t &surfaceid,
                                                     int16_t* handled)
{
    NS_RUNTIMEABORT("NPP_HandleEvent_IOSurface is a OSX-only message");
    return IPC_FAIL_NO_REASON(this);
}
#endif

mozilla::ipc::IPCResult
PluginInstanceChild::RecvWindowPosChanged(const NPRemoteEvent& event)
{
    NS_ASSERTION(!mLayersRendering && !mPendingPluginCall,
                 "Shouldn't be receiving WindowPosChanged with layer rendering");

    NS_RUNTIMEABORT("WindowPosChanged is a windows-only message");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvContentsScaleFactorChanged(const double& aContentsScaleFactor)
{
#if defined(XP_MACOSX) || defined(XP_WIN)
    mContentsScaleFactor = aContentsScaleFactor;
#if defined(XP_MACOSX)
    if (mShContext) {
        // Release the shared context so that it is reallocated
        // with the new size.
        ::CGContextRelease(mShContext);
        mShContext = nullptr;
    }
#endif
    return IPC_OK();
#else
    NS_RUNTIMEABORT("ContentsScaleFactorChanged is an Windows or OSX only message");
    return IPC_FAIL_NO_REASON(this);
#endif
}

#if defined(MOZ_X11) && defined(XP_UNIX) && !defined(XP_MACOSX)
// Create a new window from NPWindow
bool PluginInstanceChild::CreateWindow(const NPRemoteWindow& aWindow)
{
    PLUGIN_LOG_DEBUG(("%s (aWindow=<window: 0x%" PRIx64 ", x: %d, y: %d, width: %d, height: %d>)",
                      FULLFUNCTION,
                      aWindow.window,
                      aWindow.x, aWindow.y,
                      aWindow.width, aWindow.height));

#ifdef MOZ_WIDGET_GTK
    if (mXEmbed) {
        mWindow.window = reinterpret_cast<void*>(aWindow.window);
    }
    else {
        Window browserSocket = (Window)(aWindow.window);
        xt_client_init(&mXtClient, mWsInfo.visual, mWsInfo.colormap, mWsInfo.depth);
        xt_client_create(&mXtClient, browserSocket, mWindow.width, mWindow.height);
        mWindow.window = (void *)XtWindow(mXtClient.child_widget);
    }
#else
    mWindow.window = reinterpret_cast<void*>(aWindow.window);
#endif

    return true;
}

// Destroy window
void PluginInstanceChild::DeleteWindow()
{
  PLUGIN_LOG_DEBUG(("%s (aWindow=<window: 0x%p, x: %d, y: %d, width: %d, height: %d>)",
                    FULLFUNCTION,
                    mWindow.window,
                    mWindow.x, mWindow.y,
                    mWindow.width, mWindow.height));

  if (!mWindow.window)
      return;

#ifdef MOZ_WIDGET_GTK
  if (mXtClient.top_widget) {
      xt_client_unrealize(&mXtClient);
      xt_client_destroy(&mXtClient);
      mXtClient.top_widget = nullptr;
  }
#endif

  // We don't have to keep the plug-in window ID any longer.
  mWindow.window = nullptr;
}
#endif

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerCreateChildPluginWindow(NativeWindowHandle* aChildPluginWindow)
{
#if defined(XP_WIN)
    MOZ_ASSERT(!mPluginWindowHWND);

    if (!CreatePluginWindow()) {
        return IPC_FAIL_NO_REASON(this);
    }

    MOZ_ASSERT(mPluginWindowHWND);

    *aChildPluginWindow = mPluginWindowHWND;
    return IPC_OK();
#else
    NS_NOTREACHED("PluginInstanceChild::CreateChildPluginWindow not implemented!");
    return IPC_FAIL_NO_REASON(this);
#endif
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvCreateChildPopupSurrogate(const NativeWindowHandle& aNetscapeWindow)
{
#if defined(XP_WIN)
    mCachedWinlessPluginHWND = aNetscapeWindow;
    CreateWinlessPopupSurrogate();
    return IPC_OK();
#else
    NS_NOTREACHED("PluginInstanceChild::CreateChildPluginWindow not implemented!");
    return IPC_FAIL_NO_REASON(this);
#endif
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_SetWindow(const NPRemoteWindow& aWindow)
{
    PLUGIN_LOG_DEBUG(("%s (aWindow=<window: 0x%" PRIx64 ", x: %d, y: %d, width: %d, height: %d>)",
                      FULLFUNCTION,
                      aWindow.window,
                      aWindow.x, aWindow.y,
                      aWindow.width, aWindow.height));
    NS_ASSERTION(!mLayersRendering && !mPendingPluginCall,
                 "Shouldn't be receiving NPP_SetWindow with layer rendering");
    AssertPluginThread();
    AutoStackHelper guard(this);

#if defined(MOZ_X11) && defined(XP_UNIX) && !defined(XP_MACOSX)
    NS_ASSERTION(mWsInfo.display, "We should have a valid display!");

    // The minimum info is sent over IPC to allow this
    // code to determine the rest.

    mWindow.x = aWindow.x;
    mWindow.y = aWindow.y;
    mWindow.width = aWindow.width;
    mWindow.height = aWindow.height;
    mWindow.clipRect = aWindow.clipRect;
    mWindow.type = aWindow.type;

    mWsInfo.colormap = aWindow.colormap;
    int depth;
    FindVisualAndDepth(mWsInfo.display, aWindow.visualID,
                       &mWsInfo.visual, &depth);
    mWsInfo.depth = depth;

    if (!mWindow.window && mWindow.type == NPWindowTypeWindow) {
        CreateWindow(aWindow);
    }

#ifdef MOZ_WIDGET_GTK
    if (mXEmbed && gtk_check_version(2,18,7) != nullptr) { // older
        if (aWindow.type == NPWindowTypeWindow) {
            GdkWindow* socket_window = gdk_window_lookup(static_cast<GdkNativeWindow>(aWindow.window));
            if (socket_window) {
                // A GdkWindow for the socket already exists.  Need to
                // workaround https://bugzilla.gnome.org/show_bug.cgi?id=607061
                // See wrap_gtk_plug_embedded in PluginModuleChild.cpp.
                g_object_set_data(G_OBJECT(socket_window),
                                  "moz-existed-before-set-window",
                                  GUINT_TO_POINTER(1));
            }
        }

        if (aWindow.visualID != X11None
            && gtk_check_version(2, 12, 10) != nullptr) { // older
            // Workaround for a bug in Gtk+ (prior to 2.12.10) where deleting
            // a foreign GdkColormap will also free the XColormap.
            // http://git.gnome.org/browse/gtk+/log/gdk/x11/gdkcolor-x11.c?id=GTK_2_12_10
            GdkVisual *gdkvisual = gdkx_visual_get(aWindow.visualID);
            GdkColormap *gdkcolor =
                gdk_x11_colormap_foreign_new(gdkvisual, aWindow.colormap);

            if (g_object_get_data(G_OBJECT(gdkcolor), "moz-have-extra-ref")) {
                // We already have a ref to keep the object alive.
                g_object_unref(gdkcolor);
            } else {
                // leak and mark as already leaked
                g_object_set_data(G_OBJECT(gdkcolor),
                                  "moz-have-extra-ref", GUINT_TO_POINTER(1));
            }
        }
    }
#endif

    PLUGIN_LOG_DEBUG(
        ("[InstanceChild][%p] Answer_SetWindow w=<x=%d,y=%d, w=%d,h=%d>, clip=<l=%d,t=%d,r=%d,b=%d>",
         this, mWindow.x, mWindow.y, mWindow.width, mWindow.height,
         mWindow.clipRect.left, mWindow.clipRect.top, mWindow.clipRect.right, mWindow.clipRect.bottom));

    if (mPluginIface->setwindow)
        (void) mPluginIface->setwindow(&mData, &mWindow);

#elif defined(XP_MACOSX)

    mWindow.x = aWindow.x;
    mWindow.y = aWindow.y;
    mWindow.width = aWindow.width;
    mWindow.height = aWindow.height;
    mWindow.clipRect = aWindow.clipRect;
    mWindow.type = aWindow.type;
    mContentsScaleFactor = aWindow.contentsScaleFactor;

    if (mShContext) {
        // Release the shared context so that it is reallocated
        // with the new size.
        ::CGContextRelease(mShContext);
        mShContext = nullptr;
    }

    if (mPluginIface->setwindow)
        (void) mPluginIface->setwindow(&mData, &mWindow);

#elif defined(ANDROID)
    // TODO: Need Android impl
#else
#  error Implement me for your OS
#endif

    return IPC_OK();
}

bool
PluginInstanceChild::Initialize()
{
#ifdef MOZ_WIDGET_GTK
    NPError rv;

    if (mWsInfo.display) {
        // Already initialized
        return false;
    }

    // Request for windowless plugins is set in newp(), before this call.
    if (mWindow.type == NPWindowTypeWindow) {
        AnswerNPP_GetValue_NPPVpluginNeedsXEmbed(&mXEmbed, &rv);

        // Set up Xt loop for windowed plugins without XEmbed support
        if (!mXEmbed) {
           xt_client_xloop_create();
        }
    }

    // Use default GTK display for XEmbed and windowless plugins
    if (mXEmbed || mWindow.type != NPWindowTypeWindow) {
        mWsInfo.display = DefaultXDisplay();
    }
    else {
        mWsInfo.display = xt_client_get_display();
    }
#endif

    return true;
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvHandledWindowedPluginKeyEvent(
                       const NativeEventData& aKeyEventData,
                       const bool& aIsConsumed)
{
    // Unknown key input shouldn't be sent to plugin for security.
    // XXX Is this possible if a plugin process which posted the message
    //     already crashed and this plugin process is recreated?
    if (NS_WARN_IF(!mPostingKeyEvents && !mPostingKeyEventsOutdated)) {
        return IPC_OK();
    }

    // If there is outdated posting key events, we should consume the key
    // events.
    if (mPostingKeyEventsOutdated) {
        mPostingKeyEventsOutdated--;
        return IPC_OK();
    }

    mPostingKeyEvents--;

    // If composition has been started after posting the key event,
    // we should discard the event since if we send the event to plugin,
    // the plugin may be confused and the result may be broken because
    // the event order is shuffled.
    if (aIsConsumed || sIsIMEComposing) {
        return IPC_OK();
    }

    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerSetPluginFocus()
{
    MOZ_LOG(GetPluginLog(), LogLevel::Debug, ("%s", FULLFUNCTION));

    NS_NOTREACHED("PluginInstanceChild::AnswerSetPluginFocus not implemented!");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerUpdateWindow()
{
    MOZ_LOG(GetPluginLog(), LogLevel::Debug, ("%s", FULLFUNCTION));

    NS_NOTREACHED("PluginInstanceChild::AnswerUpdateWindow not implemented!");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvNPP_DidComposite()
{
  if (mPluginIface->didComposite) {
    mPluginIface->didComposite(GetNPP());
  }
  return IPC_OK();
}

PPluginScriptableObjectChild*
PluginInstanceChild::AllocPPluginScriptableObjectChild()
{
    AssertPluginThread();
    return new PluginScriptableObjectChild(Proxy);
}

bool
PluginInstanceChild::DeallocPPluginScriptableObjectChild(
    PPluginScriptableObjectChild* aObject)
{
    AssertPluginThread();
    delete aObject;
    return true;
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvPPluginScriptableObjectConstructor(
                                           PPluginScriptableObjectChild* aActor)
{
    AssertPluginThread();

    // This is only called in response to the parent process requesting the
    // creation of an actor. This actor will represent an NPObject that is
    // created by the browser and returned to the plugin.
    PluginScriptableObjectChild* actor =
        static_cast<PluginScriptableObjectChild*>(aActor);
    NS_ASSERTION(!actor->GetObject(false), "Actor already has an object?!");

    actor->InitializeProxy();
    NS_ASSERTION(actor->GetObject(false), "Actor should have an object!");

    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvPBrowserStreamConstructor(
    PBrowserStreamChild* aActor,
    const nsCString& url,
    const uint32_t& length,
    const uint32_t& lastmodified,
    PStreamNotifyChild* notifyData,
    const nsCString& headers)
{
    return IPC_OK();
}

NPError
PluginInstanceChild::DoNPP_NewStream(BrowserStreamChild* actor,
                                     const nsCString& mimeType,
                                     const bool& seekable,
                                     uint16_t* stype)
{
    AssertPluginThread();
    AutoStackHelper guard(this);
    NPError rv = actor->StreamConstructed(mimeType, seekable, stype);
    return rv;
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_NewStream(PBrowserStreamChild* actor,
                                         const nsCString& mimeType,
                                         const bool& seekable,
                                         NPError* rv,
                                         uint16_t* stype)
{
    *rv = DoNPP_NewStream(static_cast<BrowserStreamChild*>(actor), mimeType,
                          seekable, stype);
    return IPC_OK();
}

PBrowserStreamChild*
PluginInstanceChild::AllocPBrowserStreamChild(const nsCString& url,
                                              const uint32_t& length,
                                              const uint32_t& lastmodified,
                                              PStreamNotifyChild* notifyData,
                                              const nsCString& headers)
{
    AssertPluginThread();
    return new BrowserStreamChild(this, url, length, lastmodified,
                                  static_cast<StreamNotifyChild*>(notifyData),
                                  headers);
}

bool
PluginInstanceChild::DeallocPBrowserStreamChild(PBrowserStreamChild* stream)
{
    AssertPluginThread();
    delete stream;
    return true;
}

PPluginStreamChild*
PluginInstanceChild::AllocPPluginStreamChild(const nsCString& mimeType,
                                             const nsCString& target,
                                             NPError* result)
{
    NS_RUNTIMEABORT("not callable");
    return nullptr;
}

bool
PluginInstanceChild::DeallocPPluginStreamChild(PPluginStreamChild* stream)
{
    AssertPluginThread();
    delete stream;
    return true;
}

PStreamNotifyChild*
PluginInstanceChild::AllocPStreamNotifyChild(const nsCString& url,
                                             const nsCString& target,
                                             const bool& post,
                                             const nsCString& buffer,
                                             const bool& file,
                                             NPError* result)
{
    AssertPluginThread();
    NS_RUNTIMEABORT("not reached");
    return nullptr;
}

void
StreamNotifyChild::ActorDestroy(ActorDestroyReason why)
{
    if (AncestorDeletion == why && mBrowserStream) {
        NS_ERROR("Pending NPP_URLNotify not called when closing an instance.");

        // reclaim responsibility for deleting ourself
        mBrowserStream->mStreamNotify = nullptr;
        mBrowserStream = nullptr;
    }
}

void
StreamNotifyChild::SetAssociatedStream(BrowserStreamChild* bs)
{
    NS_ASSERTION(!mBrowserStream, "Two streams for one streamnotify?");

    mBrowserStream = bs;
}

mozilla::ipc::IPCResult
StreamNotifyChild::Recv__delete__(const NPReason& reason)
{
    AssertPluginThread();

    if (mBrowserStream)
        mBrowserStream->NotifyPending();
    else
        NPP_URLNotify(reason);

    return IPC_OK();
}

mozilla::ipc::IPCResult
StreamNotifyChild::RecvRedirectNotify(const nsCString& url, const int32_t& status)
{
    // NPP_URLRedirectNotify requires a non-null closure. Since core logic
    // assumes that all out-of-process notify streams have non-null closure
    // data it will assume that the plugin was notified at this point and
    // expect a response otherwise the redirect will hang indefinitely.
    if (!mClosure) {
        SendRedirectNotifyResponse(false);
    }

    PluginInstanceChild* instance = static_cast<PluginInstanceChild*>(Manager());
    if (instance->mPluginIface->urlredirectnotify)
      instance->mPluginIface->urlredirectnotify(instance->GetNPP(), url.get(), status, mClosure);

    return IPC_OK();
}

void
StreamNotifyChild::NPP_URLNotify(NPReason reason)
{
    PluginInstanceChild* instance = static_cast<PluginInstanceChild*>(Manager());

    if (mClosure)
        instance->mPluginIface->urlnotify(instance->GetNPP(), mURL.get(),
                                          reason, mClosure);
}

bool
PluginInstanceChild::DeallocPStreamNotifyChild(PStreamNotifyChild* notifyData)
{
    AssertPluginThread();

    if (!static_cast<StreamNotifyChild*>(notifyData)->mBrowserStream)
        delete notifyData;
    return true;
}

PluginScriptableObjectChild*
PluginInstanceChild::GetActorForNPObject(NPObject* aObject)
{
    AssertPluginThread();
    NS_ASSERTION(aObject, "Null pointer!");

    if (aObject->_class == PluginScriptableObjectChild::GetClass()) {
        // One of ours! It's a browser-provided object.
        ChildNPObject* object = static_cast<ChildNPObject*>(aObject);
        NS_ASSERTION(object->parent, "Null actor!");
        return object->parent;
    }

    PluginScriptableObjectChild* actor =
        PluginScriptableObjectChild::GetActorForNPObject(aObject);
    if (actor) {
        // Plugin-provided object that we've previously wrapped.
        return actor;
    }

    actor = new PluginScriptableObjectChild(LocalObject);
    if (!SendPPluginScriptableObjectConstructor(actor)) {
        NS_ERROR("Failed to send constructor message!");
        return nullptr;
    }

    actor->InitializeLocal(aObject);
    return actor;
}

NPError
PluginInstanceChild::NPN_NewStream(NPMIMEType aMIMEType, const char* aWindow,
                                   NPStream** aStream)
{
    AssertPluginThread();
    AutoStackHelper guard(this);

    auto* ps = new PluginStreamChild();

    NPError result;
    CallPPluginStreamConstructor(ps, nsDependentCString(aMIMEType),
                                 NullableString(aWindow), &result);
    if (NPERR_NO_ERROR != result) {
        *aStream = nullptr;
        PPluginStreamChild::Call__delete__(ps, NPERR_GENERIC_ERROR, true);
        return result;
    }

    *aStream = &ps->mStream;
    return NPERR_NO_ERROR;
}

void
PluginInstanceChild::NPN_URLRedirectResponse(void* notifyData, NPBool allow)
{
    if (!notifyData) {
        return;
    }

    InfallibleTArray<PStreamNotifyChild*> notifyStreams;
    ManagedPStreamNotifyChild(notifyStreams);
    uint32_t notifyStreamCount = notifyStreams.Length();
    for (uint32_t i = 0; i < notifyStreamCount; i++) {
        StreamNotifyChild* sn = static_cast<StreamNotifyChild*>(notifyStreams[i]);
        if (sn->mClosure == notifyData) {
            sn->SendRedirectNotifyResponse(static_cast<bool>(allow));
            return;
        }
    }
    NS_ASSERTION(false, "Couldn't find stream for redirect response!");
}

bool
PluginInstanceChild::IsUsingDirectDrawing()
{
    return IsDrawingModelDirect(mDrawingModel);
}

PluginInstanceChild::DirectBitmap::DirectBitmap(PluginInstanceChild* aOwner, const Shmem& shmem,
                                                const IntSize& size, uint32_t stride, SurfaceFormat format)
  : mOwner(aOwner),
    mShmem(shmem),
    mFormat(format),
    mSize(size),
    mStride(stride)
{
}

PluginInstanceChild::DirectBitmap::~DirectBitmap()
{
    mOwner->DeallocShmem(mShmem);
}

static inline SurfaceFormat
NPImageFormatToSurfaceFormat(NPImageFormat aFormat)
{
    switch (aFormat) {
    case NPImageFormatBGRA32:
        return SurfaceFormat::B8G8R8A8;
    case NPImageFormatBGRX32:
        return SurfaceFormat::B8G8R8X8;
    default:
        MOZ_ASSERT_UNREACHABLE("unknown NPImageFormat");
        return SurfaceFormat::UNKNOWN;
    }
}

static inline gfx::IntRect
NPRectToIntRect(const NPRect& in)
{
    return IntRect(in.left, in.top, in.right - in.left, in.bottom - in.top);
}

NPError
PluginInstanceChild::NPN_InitAsyncSurface(NPSize *size, NPImageFormat format,
                                          void *initData, NPAsyncSurface *surface)
{
    AssertPluginThread();
    AutoStackHelper guard(this);

    if (!IsUsingDirectDrawing()) {
        return NPERR_INVALID_PARAM;
    }
    if (format != NPImageFormatBGRA32 && format != NPImageFormatBGRX32) {
        return NPERR_INVALID_PARAM;
    }

    PodZero(surface);

    // NPAPI guarantees that the SetCurrentAsyncSurface call will release the
    // previous surface if it was different. However, no functionality exists
    // within content to synchronize a non-shadow-layers transaction with the
    // compositor.
    //
    // To get around this, we allocate two surfaces: a child copy, which we
    // hand off to the plugin, and a parent copy, which we will hand off to
    // the compositor. Each call to SetCurrentAsyncSurface will copy the
    // invalid region from the child surface to its parent.
    switch (mDrawingModel) {
    case NPDrawingModelAsyncBitmapSurface: {
        // Validate that the caller does not expect initial data to be set.
        if (initData) {
            return NPERR_INVALID_PARAM;
        }

        // Validate that we're not double-allocating a surface.
        RefPtr<DirectBitmap> holder;
        if (mDirectBitmaps.Get(surface, getter_AddRefs(holder))) {
            return NPERR_INVALID_PARAM;
        }

        SurfaceFormat mozformat = NPImageFormatToSurfaceFormat(format);
        int32_t bytesPerPixel = BytesPerPixel(mozformat);

        if (size->width <= 0 || size->height <= 0) {
            return NPERR_INVALID_PARAM;
        }

        CheckedInt<uint32_t> nbytes = SafeBytesForBitmap(size->width, size->height, bytesPerPixel);
        if (!nbytes.isValid()) {
            return NPERR_INVALID_PARAM;
        }

        Shmem shmem;
        if (!AllocUnsafeShmem(nbytes.value(), SharedMemory::TYPE_BASIC, &shmem)) {
            return NPERR_OUT_OF_MEMORY_ERROR;
        }
        MOZ_ASSERT(shmem.Size<uint8_t>() == nbytes.value());

        surface->version = 0;
        surface->size = *size;
        surface->format = format;
        surface->bitmap.data = shmem.get<unsigned char>();
        surface->bitmap.stride = size->width * bytesPerPixel;

        // Hold the shmem alive until Finalize() is called or this actor dies.
        holder = new DirectBitmap(this, shmem,
                                  IntSize(size->width, size->height),
                                  surface->bitmap.stride, mozformat);
        mDirectBitmaps.Put(surface, holder);
        return NPERR_NO_ERROR;
    }
#if defined(XP_WIN)
    case NPDrawingModelAsyncWindowsDXGISurface: {
        // Validate that the caller does not expect initial data to be set.
        if (initData) {
            return NPERR_INVALID_PARAM;
        }

        // Validate that we're not double-allocating a surface.
        WindowsHandle handle = 0;
        if (mDxgiSurfaces.Get(surface, &handle)) {
            return NPERR_INVALID_PARAM;
        }

        NPError error = NPERR_NO_ERROR;
        SurfaceFormat mozformat = NPImageFormatToSurfaceFormat(format);
        if (!SendInitDXGISurface(mozformat,
                                  IntSize(size->width, size->height),
                                  &handle,
                                  &error))
        {
            return NPERR_GENERIC_ERROR;
        }
        if (error != NPERR_NO_ERROR) {
            return error;
        }

        surface->version = 0;
        surface->size = *size;
        surface->format = format;
        surface->sharedHandle = reinterpret_cast<HANDLE>(handle);

        mDxgiSurfaces.Put(surface, handle);
        return NPERR_NO_ERROR;
    }
#endif
    default:
        MOZ_ASSERT_UNREACHABLE("unknown drawing model");
    }

    return NPERR_INVALID_PARAM;
}

NPError
PluginInstanceChild::NPN_FinalizeAsyncSurface(NPAsyncSurface *surface)
{
    AssertPluginThread();

    if (!IsUsingDirectDrawing()) {
        return NPERR_GENERIC_ERROR;
    }

    // The API forbids this. If it becomes a problem we can revoke the current
    // surface instead.
    MOZ_ASSERT(!surface || mCurrentDirectSurface != surface);

    switch (mDrawingModel) {
    case NPDrawingModelAsyncBitmapSurface: {
        RefPtr<DirectBitmap> bitmap;
        if (!mDirectBitmaps.Get(surface, getter_AddRefs(bitmap))) {
            return NPERR_INVALID_PARAM;
        }

        PodZero(surface);
        mDirectBitmaps.Remove(surface);
        return NPERR_NO_ERROR;
    }
#if defined(XP_WIN)
    case NPDrawingModelAsyncWindowsDXGISurface: {
        WindowsHandle handle;
        if (!mDxgiSurfaces.Get(surface, &handle)) {
            return NPERR_INVALID_PARAM;
        }

        SendFinalizeDXGISurface(handle);
        mDxgiSurfaces.Remove(surface);
        return NPERR_NO_ERROR;
    }
#endif
    default:
        MOZ_ASSERT_UNREACHABLE("unknown drawing model");
    }

    return NPERR_INVALID_PARAM;
}

void
PluginInstanceChild::NPN_SetCurrentAsyncSurface(NPAsyncSurface *surface, NPRect *changed)
{
    AssertPluginThread();

    if (!IsUsingDirectDrawing()) {
        return;
    }

    mCurrentDirectSurface = surface;

    if (!surface) {
        SendRevokeCurrentDirectSurface();
        return;
    }

    switch (mDrawingModel) {
    case NPDrawingModelAsyncBitmapSurface: {
        RefPtr<DirectBitmap> bitmap;
        if (!mDirectBitmaps.Get(surface, getter_AddRefs(bitmap))) {
            return;
        }

        IntRect dirty = changed
                        ? NPRectToIntRect(*changed)
                        : IntRect(IntPoint(0, 0), bitmap->mSize);

        // Need a holder since IPDL zaps the object for mysterious reasons.
        Shmem shmemHolder = bitmap->mShmem;
        SendShowDirectBitmap(shmemHolder, bitmap->mFormat, bitmap->mStride, bitmap->mSize, dirty);
        break;
    }
#if defined(XP_WIN)
    case NPDrawingModelAsyncWindowsDXGISurface: {
        WindowsHandle handle;
        if (!mDxgiSurfaces.Get(surface, &handle)) {
            return;
        }

        IntRect dirty = changed
                        ? NPRectToIntRect(*changed)
                        : IntRect(IntPoint(0, 0), IntSize(surface->size.width, surface->size.height));

        SendShowDirectDXGISurface(handle, dirty);
        break;
    }
#endif
    default:
        MOZ_ASSERT_UNREACHABLE("unknown drawing model");
    }
}

void
PluginInstanceChild::DoAsyncRedraw()
{
    {
        MutexAutoLock autoLock(mAsyncInvalidateMutex);
        mAsyncInvalidateTask = nullptr;
    }

    SendRedrawPlugin();
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvAsyncSetWindow(const gfxSurfaceType& aSurfaceType,
                                        const NPRemoteWindow& aWindow)
{
    AssertPluginThread();

    AutoStackHelper guard(this);
    NS_ASSERTION(!aWindow.window, "Remote window should be null.");

    if (mCurrentAsyncSetWindowTask) {
        mCurrentAsyncSetWindowTask->Cancel();
        mCurrentAsyncSetWindowTask = nullptr;
    }

    // We shouldn't process this now because it may be received within a nested
    // RPC call, and both Flash and Java don't expect to receive setwindow calls
    // at arbitrary times.
    mCurrentAsyncSetWindowTask =
        NewNonOwningCancelableRunnableMethod<gfxSurfaceType, NPRemoteWindow, bool>
        (this, &PluginInstanceChild::DoAsyncSetWindow, aSurfaceType, aWindow, true);
    RefPtr<Runnable> addrefedTask = mCurrentAsyncSetWindowTask;
    MessageLoop::current()->PostTask(addrefedTask.forget());

    return IPC_OK();
}

void
PluginInstanceChild::DoAsyncSetWindow(const gfxSurfaceType& aSurfaceType,
                                      const NPRemoteWindow& aWindow,
                                      bool aIsAsync)
{
    PLUGIN_LOG_DEBUG(
        ("[InstanceChild][%p] AsyncSetWindow to <x=%d,y=%d, w=%d,h=%d>",
         this, aWindow.x, aWindow.y, aWindow.width, aWindow.height));

    AssertPluginThread();
    NS_ASSERTION(!aWindow.window, "Remote window should be null.");
    NS_ASSERTION(!mPendingPluginCall, "Can't do SetWindow during plugin call!");

    if (aIsAsync) {
        if (!mCurrentAsyncSetWindowTask) {
            return;
        }
        mCurrentAsyncSetWindowTask = nullptr;
    }

    mWindow.window = nullptr;
    if (mWindow.width != aWindow.width || mWindow.height != aWindow.height ||
        mWindow.clipRect.top != aWindow.clipRect.top ||
        mWindow.clipRect.left != aWindow.clipRect.left ||
        mWindow.clipRect.bottom != aWindow.clipRect.bottom ||
        mWindow.clipRect.right != aWindow.clipRect.right)
        mAccumulatedInvalidRect = nsIntRect(0, 0, aWindow.width, aWindow.height);

    mWindow.x = aWindow.x;
    mWindow.y = aWindow.y;
    mWindow.width = aWindow.width;
    mWindow.height = aWindow.height;
    mWindow.clipRect = aWindow.clipRect;
    mWindow.type = aWindow.type;
#if defined(XP_MACOSX) || defined(XP_WIN)
    mContentsScaleFactor = aWindow.contentsScaleFactor;
#endif

    mLayersRendering = true;
    mSurfaceType = aSurfaceType;
    UpdateWindowAttributes(true);

#ifdef XP_WIN
    if (GetQuirks() & QUIRK_FLASH_THROTTLE_WMUSER_EVENTS)
        SetupFlashMsgThrottle();
#endif

    if (!mAccumulatedInvalidRect.IsEmpty()) {
        AsyncShowPluginFrame();
    }
}

bool
PluginInstanceChild::CreateOptSurface(void)
{
    MOZ_ASSERT(mSurfaceType != gfxSurfaceType::Max,
               "Need a valid surface type here");
    NS_ASSERTION(!mCurrentSurface, "mCurrentSurfaceActor can get out of sync.");

    // Use an opaque surface unless we're transparent and *don't* have
    // a background to source from.
    gfxImageFormat format =
        (mIsTransparent && !mBackground) ? SurfaceFormat::A8R8G8B8_UINT32 :
                                           SurfaceFormat::X8R8G8B8_UINT32;

#ifdef MOZ_X11
    Display* dpy = mWsInfo.display;
    Screen* screen = DefaultScreenOfDisplay(dpy);
    if (format == SurfaceFormat::X8R8G8B8_UINT32 &&
        DefaultDepth(dpy, DefaultScreen(dpy)) == 16) {
        format = SurfaceFormat::R5G6B5_UINT16;
    }

    if (mSurfaceType == gfxSurfaceType::Xlib) {
        if (!mIsTransparent  || mBackground) {
            Visual* defaultVisual = DefaultVisualOfScreen(screen);
            mCurrentSurface =
                gfxXlibSurface::Create(screen, defaultVisual,
                                       IntSize(mWindow.width, mWindow.height));
            return mCurrentSurface != nullptr;
        }

        XRenderPictFormat* xfmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
        if (!xfmt) {
            NS_ERROR("Need X falback surface, but FindRenderFormat failed");
            return false;
        }
        mCurrentSurface =
            gfxXlibSurface::Create(screen, xfmt,
                                   IntSize(mWindow.width, mWindow.height));
        return mCurrentSurface != nullptr;
    }
#endif

#ifdef XP_WIN
    if (mSurfaceType == gfxSurfaceType::Win32) {
        bool willHaveTransparentPixels = mIsTransparent && !mBackground;

        SharedDIBSurface* s = new SharedDIBSurface();
        if (!s->Create(reinterpret_cast<HDC>(mWindow.window),
                       mWindow.width, mWindow.height,
                       willHaveTransparentPixels))
            return false;

        mCurrentSurface = s;
        return true;
    }

    NS_RUNTIMEABORT("Shared-memory drawing not expected on Windows.");
#endif

    // Make common shmem implementation working for any platform
    mCurrentSurface =
        gfxSharedImageSurface::CreateUnsafe(this, IntSize(mWindow.width, mWindow.height), format);
    return !!mCurrentSurface;
}

bool
PluginInstanceChild::MaybeCreatePlatformHelperSurface(void)
{
    if (!mCurrentSurface) {
        NS_ERROR("Cannot create helper surface without mCurrentSurface");
        return false;
    }

#ifdef MOZ_X11
    bool supportNonDefaultVisual = false;
    Screen* screen = DefaultScreenOfDisplay(mWsInfo.display);
    Visual* defaultVisual = DefaultVisualOfScreen(screen);
    Visual* visual = nullptr;
    Colormap colormap = 0;
    mDoAlphaExtraction = false;
    bool createHelperSurface = false;

    if (mCurrentSurface->GetType() == gfxSurfaceType::Xlib) {
        static_cast<gfxXlibSurface*>(mCurrentSurface.get())->
            GetColormapAndVisual(&colormap, &visual);
        // Create helper surface if layer surface visual not same as default
        // and we don't support non-default visual rendering
        if (!visual || (defaultVisual != visual && !supportNonDefaultVisual)) {
            createHelperSurface = true;
            visual = defaultVisual;
            mDoAlphaExtraction = mIsTransparent;
        }
    } else if (mCurrentSurface->GetType() == gfxSurfaceType::Image) {
        // For image layer surface we should always create helper surface
        createHelperSurface = true;
        // Check if we can create helper surface with non-default visual
        visual = gfxXlibSurface::FindVisual(screen,
            static_cast<gfxImageSurface*>(mCurrentSurface.get())->Format());
        if (!visual || (defaultVisual != visual && !supportNonDefaultVisual)) {
            visual = defaultVisual;
            mDoAlphaExtraction = mIsTransparent;
        }
    }

    if (createHelperSurface) {
        if (!visual) {
            NS_ERROR("Need X falback surface, but visual failed");
            return false;
        }
        mHelperSurface =
            gfxXlibSurface::Create(screen, visual,
                                   mCurrentSurface->GetSize());
        if (!mHelperSurface) {
            NS_WARNING("Fail to create create helper surface");
            return false;
        }
    }
#elif defined(XP_WIN)
    mDoAlphaExtraction = mIsTransparent && !mBackground;
#endif

    return true;
}

bool
PluginInstanceChild::EnsureCurrentBuffer(void)
{
#ifndef XP_DARWIN
    nsIntRect toInvalidate(0, 0, 0, 0);
    IntSize winSize = IntSize(mWindow.width, mWindow.height);

    if (mBackground && mBackground->GetSize() != winSize) {
        // It would be nice to keep the old background here, but doing
        // so can lead to cases in which we permanently keep the old
        // background size.
        mBackground = nullptr;
        toInvalidate.UnionRect(toInvalidate,
                               nsIntRect(0, 0, winSize.width, winSize.height));
    }

    if (mCurrentSurface) {
        IntSize surfSize = mCurrentSurface->GetSize();
        if (winSize != surfSize ||
            (mBackground && !CanPaintOnBackground()) ||
            (mBackground &&
             gfxContentType::COLOR != mCurrentSurface->GetContentType()) ||
            (!mBackground && mIsTransparent &&
             gfxContentType::COLOR == mCurrentSurface->GetContentType())) {
            // Don't try to use an old, invalid DC.
            mWindow.window = nullptr;
            ClearCurrentSurface();
            toInvalidate.UnionRect(toInvalidate,
                                   nsIntRect(0, 0, winSize.width, winSize.height));
        }
    }

    mAccumulatedInvalidRect.UnionRect(mAccumulatedInvalidRect, toInvalidate);

    if (mCurrentSurface) {
        return true;
    }

    if (!CreateOptSurface()) {
        NS_ERROR("Cannot create optimized surface");
        return false;
    }

    if (!MaybeCreatePlatformHelperSurface()) {
        NS_ERROR("Cannot create helper surface");
        return false;
    }
#elif defined(XP_MACOSX)

    if (!mDoubleBufferCARenderer.HasCALayer()) {
        void *caLayer = nullptr;
        if (mDrawingModel == NPDrawingModelCoreGraphics) {
            if (!mCGLayer) {
                caLayer = mozilla::plugins::PluginUtilsOSX::GetCGLayer(CallCGDraw,
                                                                       this,
                                                                       mContentsScaleFactor);

                if (!caLayer) {
                    PLUGIN_LOG_DEBUG(("GetCGLayer failed."));
                    return false;
                }
            }
            mCGLayer = caLayer;
        } else {
            NPError result = mPluginIface->getvalue(GetNPP(),
                                     NPPVpluginCoreAnimationLayer,
                                     &caLayer);
            if (result != NPERR_NO_ERROR || !caLayer) {
                PLUGIN_LOG_DEBUG(("Plugin requested CoreAnimation but did not "
                                  "provide CALayer."));
                return false;
            }
        }
        mDoubleBufferCARenderer.SetCALayer(caLayer);
    }

    if (mDoubleBufferCARenderer.HasFrontSurface() &&
        (mDoubleBufferCARenderer.GetFrontSurfaceWidth() != mWindow.width ||
         mDoubleBufferCARenderer.GetFrontSurfaceHeight() != mWindow.height ||
         mDoubleBufferCARenderer.GetContentsScaleFactor() != mContentsScaleFactor)) {
        mDoubleBufferCARenderer.ClearFrontSurface();
    }

    if (!mDoubleBufferCARenderer.HasFrontSurface()) {
        bool allocSurface = mDoubleBufferCARenderer.InitFrontSurface(
                                mWindow.width, mWindow.height, mContentsScaleFactor,
                                GetQuirks() & QUIRK_ALLOW_OFFLINE_RENDERER ?
                                ALLOW_OFFLINE_RENDERER : DISALLOW_OFFLINE_RENDERER);
        if (!allocSurface) {
            PLUGIN_LOG_DEBUG(("Fail to allocate front IOSurface"));
            return false;
        }

        if (mPluginIface->setwindow)
            (void) mPluginIface->setwindow(&mData, &mWindow);

        nsIntRect toInvalidate(0, 0, mWindow.width, mWindow.height);
        mAccumulatedInvalidRect.UnionRect(mAccumulatedInvalidRect, toInvalidate);
    }
#endif

    return true;
}

void
PluginInstanceChild::UpdateWindowAttributes(bool aForceSetWindow)
{
#if defined(MOZ_X11) || defined(XP_WIN)
    RefPtr<gfxASurface> curSurface = mHelperSurface ? mHelperSurface : mCurrentSurface;
#endif // Only used within MOZ_X11 or XP_WIN blocks. Unused variable otherwise
    bool needWindowUpdate = aForceSetWindow;
#ifdef MOZ_X11
    Visual* visual = nullptr;
    Colormap colormap = 0;
    if (curSurface && curSurface->GetType() == gfxSurfaceType::Xlib) {
        static_cast<gfxXlibSurface*>(curSurface.get())->
            GetColormapAndVisual(&colormap, &visual);
        if (visual != mWsInfo.visual || colormap != mWsInfo.colormap) {
            mWsInfo.visual = visual;
            mWsInfo.colormap = colormap;
            needWindowUpdate = true;
        }
    }
#endif // MOZ_X11
#ifdef XP_WIN
    HDC dc = nullptr;

    if (curSurface) {
        if (!SharedDIBSurface::IsSharedDIBSurface(curSurface))
            NS_RUNTIMEABORT("Expected SharedDIBSurface!");

        SharedDIBSurface* dibsurf = static_cast<SharedDIBSurface*>(curSurface.get());
        dc = dibsurf->GetHDC();
    }
    if (mWindow.window != dc) {
        mWindow.window = dc;
        needWindowUpdate = true;
    }
#endif // XP_WIN

    if (!needWindowUpdate) {
        return;
    }

#ifndef XP_MACOSX
    // Adjusting the window isn't needed for OSX
#ifndef XP_WIN
    // On Windows, we translate the device context, in order for the window
    // origin to be correct.
    mWindow.x = mWindow.y = 0;
#endif

    if (IsVisible()) {
        // The clip rect is relative to drawable top-left.
        nsIntRect clipRect;

        // Don't ask the plugin to draw outside the drawable. The clip rect
        // is in plugin coordinates, not window coordinates.
        // This also ensures that the unsigned clip rectangle offsets won't be -ve.
        clipRect.SetRect(0, 0, mWindow.width, mWindow.height);

        mWindow.clipRect.left = 0;
        mWindow.clipRect.top = 0;
        mWindow.clipRect.right = clipRect.XMost();
        mWindow.clipRect.bottom = clipRect.YMost();
    }
#endif // XP_MACOSX

#ifdef XP_WIN
    // Windowless plugins on Windows need a WM_WINDOWPOSCHANGED event to update
    // their location... or at least Flash does: Silverlight uses the
    // window.x/y passed to NPP_SetWindow

    if (mPluginIface->event) {
        WINDOWPOS winpos = {
            0, 0,
            mWindow.x, mWindow.y,
            mWindow.width, mWindow.height,
            0
        };
        NPEvent pluginEvent = {
            WM_WINDOWPOSCHANGED, 0,
            (LPARAM) &winpos
        };
        mPluginIface->event(&mData, &pluginEvent);
    }
#endif

    PLUGIN_LOG_DEBUG(
        ("[InstanceChild][%p] UpdateWindow w=<x=%d,y=%d, w=%d,h=%d>, clip=<l=%d,t=%d,r=%d,b=%d>",
         this, mWindow.x, mWindow.y, mWindow.width, mWindow.height,
         mWindow.clipRect.left, mWindow.clipRect.top, mWindow.clipRect.right, mWindow.clipRect.bottom));

    if (mPluginIface->setwindow) {
        mPluginIface->setwindow(&mData, &mWindow);
    }
}

void
PluginInstanceChild::PaintRectToPlatformSurface(const nsIntRect& aRect,
                                                gfxASurface* aSurface)
{
    UpdateWindowAttributes();

    // We should not send an async surface if we're using direct rendering.
    MOZ_ASSERT(!IsUsingDirectDrawing());

#ifdef MOZ_X11
    {
        NS_ASSERTION(aSurface->GetType() == gfxSurfaceType::Xlib,
                     "Non supported platform surface type");

        NPEvent pluginEvent;
        XGraphicsExposeEvent& exposeEvent = pluginEvent.xgraphicsexpose;
        exposeEvent.type = GraphicsExpose;
        exposeEvent.display = mWsInfo.display;
        exposeEvent.drawable = static_cast<gfxXlibSurface*>(aSurface)->XDrawable();
        exposeEvent.x = aRect.x;
        exposeEvent.y = aRect.y;
        exposeEvent.width = aRect.width;
        exposeEvent.height = aRect.height;
        exposeEvent.count = 0;
        // information not set:
        exposeEvent.serial = 0;
        exposeEvent.send_event = False;
        exposeEvent.major_code = 0;
        exposeEvent.minor_code = 0;
        mPluginIface->event(&mData, reinterpret_cast<void*>(&exposeEvent));
    }
#elif defined(XP_WIN)
    NS_ASSERTION(SharedDIBSurface::IsSharedDIBSurface(aSurface),
                 "Expected (SharedDIB) image surface.");

    // This rect is in the window coordinate space. aRect is in the plugin
    // coordinate space.
    RECT rect = {
        mWindow.x + aRect.x,
        mWindow.y + aRect.y,
        mWindow.x + aRect.XMost(),
        mWindow.y + aRect.YMost()
    };
    NPEvent paintEvent = {
        WM_PAINT,
        uintptr_t(mWindow.window),
        uintptr_t(&rect)
    };

    ::SetViewportOrgEx((HDC) mWindow.window, -mWindow.x, -mWindow.y, nullptr);
    ::SelectClipRgn((HDC) mWindow.window, nullptr);
    ::IntersectClipRect((HDC) mWindow.window, rect.left, rect.top, rect.right, rect.bottom);
    mPluginIface->event(&mData, reinterpret_cast<void*>(&paintEvent));
#else
    NS_RUNTIMEABORT("Surface type not implemented.");
#endif
}

void
PluginInstanceChild::PaintRectToSurface(const nsIntRect& aRect,
                                        gfxASurface* aSurface,
                                        const Color& aColor)
{
    // Render using temporary X surface, with copy to image surface
    nsIntRect plPaintRect(aRect);
    RefPtr<gfxASurface> renderSurface = aSurface;
#ifdef MOZ_X11
    if (mIsTransparent && (GetQuirks() & QUIRK_FLASH_EXPOSE_COORD_TRANSLATION)) {
        // Work around a bug in Flash up to 10.1 d51 at least, where expose event
        // top left coordinates within the plugin-rect and not at the drawable
        // origin are misinterpreted.  (We can move the top left coordinate
        // provided it is within the clipRect.), see bug 574583
        plPaintRect.SetRect(0, 0, aRect.XMost(), aRect.YMost());
    }
    if (mHelperSurface) {
        // On X11 we can paint to non Xlib surface only with HelperSurface
        renderSurface = mHelperSurface;
    }
#endif

    if (mIsTransparent && !CanPaintOnBackground()) {
        RefPtr<DrawTarget> dt = CreateDrawTargetForSurface(renderSurface);
        gfx::Rect rect(plPaintRect.x, plPaintRect.y,
                       plPaintRect.width, plPaintRect.height);
        // Moz2D treats OP_SOURCE operations as unbounded, so we need to
        // clip to the rect that we want to fill:
        dt->PushClipRect(rect);
        dt->FillRect(rect, ColorPattern(aColor), // aColor is already a device color
                     DrawOptions(1.f, CompositionOp::OP_SOURCE));
        dt->PopClip();
        dt->Flush();
    }

    PaintRectToPlatformSurface(plPaintRect, renderSurface);

    if (renderSurface != aSurface) {
      RefPtr<DrawTarget> dt;
      if (aSurface == mCurrentSurface &&
          aSurface->GetType() == gfxSurfaceType::Image &&
          aSurface->GetSurfaceFormat() == SurfaceFormat::B8G8R8X8) {
        gfxImageSurface* imageSurface = static_cast<gfxImageSurface*>(aSurface);
        // Bug 1196927 - Reinterpret target surface as BGRA to fill alpha with opaque.
        // Certain backends (i.e. Skia) may not truly support BGRX formats, so they must
        // be emulated by filling the alpha channel opaque as if it was BGRA data. Cairo
        // leaves the alpha zeroed out for BGRX, so we cause Cairo to fill it as opaque
        // by handling the copy target as a BGRA surface.
        dt = Factory::CreateDrawTargetForData(BackendType::CAIRO,
                                              imageSurface->Data(),
                                              imageSurface->GetSize(),
                                              imageSurface->Stride(),
                                              SurfaceFormat::B8G8R8A8);
      } else {
        // Copy helper surface content to target
        dt = CreateDrawTargetForSurface(aSurface);
      }
      if (dt && dt->IsValid()) {
          RefPtr<SourceSurface> surface =
              gfxPlatform::GetSourceSurfaceForSurface(dt, renderSurface);
          dt->CopySurface(surface, aRect, aRect.TopLeft());
      } else {
          gfxWarning() << "PluginInstanceChild::PaintRectToSurface failure";
      }
    }
}

void
PluginInstanceChild::PaintRectWithAlphaExtraction(const nsIntRect& aRect,
                                                  gfxASurface* aSurface)
{
    MOZ_ASSERT(aSurface->GetContentType() == gfxContentType::COLOR_ALPHA,
               "Refusing to pointlessly recover alpha");

    nsIntRect rect(aRect);
    // If |aSurface| can be used to paint and can have alpha values
    // recovered directly to it, do that to save a tmp surface and
    // copy.
    bool useSurfaceSubimageForBlack = false;
    if (gfxSurfaceType::Image == aSurface->GetType()) {
        gfxImageSurface* surfaceAsImage =
            static_cast<gfxImageSurface*>(aSurface);
        useSurfaceSubimageForBlack =
            (surfaceAsImage->Format() == SurfaceFormat::A8R8G8B8_UINT32);
        // If we're going to use a subimage, nudge the rect so that we
        // can use optimal alpha recovery.  If we're not using a
        // subimage, the temporaries should automatically get
        // fast-path alpha recovery so we don't need to do anything.
        if (useSurfaceSubimageForBlack) {
            rect =
                gfxAlphaRecovery::AlignRectForSubimageRecovery(aRect,
                                                               surfaceAsImage);
        }
    }

    RefPtr<gfxImageSurface> whiteImage;
    RefPtr<gfxImageSurface> blackImage;
    gfxRect targetRect(rect.x, rect.y, rect.width, rect.height);
    IntSize targetSize(rect.width, rect.height);
    gfxPoint deviceOffset = -targetRect.TopLeft();

    // We always use a temporary "white image"
    whiteImage = new gfxImageSurface(targetSize, SurfaceFormat::X8R8G8B8_UINT32);
    if (whiteImage->CairoStatus()) {
        return;
    }

#ifdef XP_WIN
    // On windows, we need an HDC and so can't paint directly to
    // vanilla image surfaces.  Bifurcate this painting code so that
    // we don't accidentally attempt that.
    if (!SharedDIBSurface::IsSharedDIBSurface(aSurface))
        NS_RUNTIMEABORT("Expected SharedDIBSurface!");

    // Paint the plugin directly onto the target, with a white
    // background and copy the result
    PaintRectToSurface(rect, aSurface, Color(1.f, 1.f, 1.f));
    {
        RefPtr<DrawTarget> dt = CreateDrawTargetForSurface(whiteImage);
        RefPtr<SourceSurface> surface =
            gfxPlatform::GetSourceSurfaceForSurface(dt, aSurface);
        dt->CopySurface(surface, rect, IntPoint());
    }

    // Paint the plugin directly onto the target, with a black
    // background
    PaintRectToSurface(rect, aSurface, Color(0.f, 0.f, 0.f));

    // Don't copy the result, just extract a subimage so that we can
    // recover alpha directly into the target
    gfxImageSurface *image = static_cast<gfxImageSurface*>(aSurface);
    blackImage = image->GetSubimage(targetRect);

#else
    // Paint onto white background
    whiteImage->SetDeviceOffset(deviceOffset);
    PaintRectToSurface(rect, whiteImage, Color(1.f, 1.f, 1.f));

    if (useSurfaceSubimageForBlack) {
        gfxImageSurface *surface = static_cast<gfxImageSurface*>(aSurface);
        blackImage = surface->GetSubimage(targetRect);
    } else {
        blackImage = new gfxImageSurface(targetSize,
                                         SurfaceFormat::A8R8G8B8_UINT32);
    }

    // Paint onto black background
    blackImage->SetDeviceOffset(deviceOffset);
    PaintRectToSurface(rect, blackImage, Color(0.f, 0.f, 0.f));
#endif

    MOZ_ASSERT(whiteImage && blackImage, "Didn't paint enough!");

    // Extract alpha from black and white image and store to black
    // image
    if (!gfxAlphaRecovery::RecoverAlpha(blackImage, whiteImage)) {
        return;
    }

    // If we had to use a temporary black surface, copy the pixels
    // with alpha back to the target
    if (!useSurfaceSubimageForBlack) {
        RefPtr<DrawTarget> dt = CreateDrawTargetForSurface(aSurface);
        RefPtr<SourceSurface> surface =
            gfxPlatform::GetSourceSurfaceForSurface(dt, blackImage);
        dt->CopySurface(surface,
                        IntRect(0, 0, rect.width, rect.height),
                        rect.TopLeft());
    }
}

bool
PluginInstanceChild::CanPaintOnBackground()
{
    return (mBackground &&
            mCurrentSurface &&
            mCurrentSurface->GetSize() == mBackground->GetSize());
}

bool
PluginInstanceChild::ShowPluginFrame()
{
    // mLayersRendering can be false if we somehow get here without
    // receiving AsyncSetWindow() first.  mPendingPluginCall is our
    // re-entrancy guard; we can't paint while nested inside another
    // paint.
    if (!mLayersRendering || mPendingPluginCall) {
        return false;
    }

    // We should not attempt to asynchronously show the plugin if we're using
    // direct rendering.
    MOZ_ASSERT(!IsUsingDirectDrawing());

    AutoRestore<bool> pending(mPendingPluginCall);
    mPendingPluginCall = true;

    bool temporarilyMakeVisible = !IsVisible() && !mHasPainted;
    if (temporarilyMakeVisible && mWindow.width && mWindow.height) {
        mWindow.clipRect.right = mWindow.width;
        mWindow.clipRect.bottom = mWindow.height;
    } else if (!IsVisible()) {
        // If we're not visible, don't bother painting a <0,0,0,0>
        // rect.  If we're eventually made visible, the visibility
        // change will invalidate our window.
        ClearCurrentSurface();
        return true;
    }

    if (!EnsureCurrentBuffer()) {
        return false;
    }

    NS_ASSERTION(mWindow.width == uint32_t(mWindow.clipRect.right - mWindow.clipRect.left) &&
                 mWindow.height == uint32_t(mWindow.clipRect.bottom - mWindow.clipRect.top),
                 "Clip rect should be same size as window when using layers");

    // Clear accRect here to be able to pass
    // test_invalidate_during_plugin_paint  test
    nsIntRect rect = mAccumulatedInvalidRect;
    mAccumulatedInvalidRect.SetEmpty();

    // Fix up old invalidations that might have been made when our
    // surface was a different size
    IntSize surfaceSize = mCurrentSurface->GetSize();
    rect.IntersectRect(rect,
                       nsIntRect(0, 0, surfaceSize.width, surfaceSize.height));

    if (!ReadbackDifferenceRect(rect)) {
        // We couldn't read back the pixels that differ between the
        // current surface and last, so we have to invalidate the
        // entire window.
        rect.SetRect(0, 0, mWindow.width, mWindow.height);
    }

    bool haveTransparentPixels =
        gfxContentType::COLOR_ALPHA == mCurrentSurface->GetContentType();
    PLUGIN_LOG_DEBUG(
        ("[InstanceChild][%p] Painting%s <x=%d,y=%d, w=%d,h=%d> on surface <w=%d,h=%d>",
         this, haveTransparentPixels ? " with alpha" : "",
         rect.x, rect.y, rect.width, rect.height,
         mCurrentSurface->GetSize().width, mCurrentSurface->GetSize().height));

    if (CanPaintOnBackground()) {
        PLUGIN_LOG_DEBUG(("  (on background)"));
        // Source the background pixels ...
        {
            RefPtr<gfxASurface> surface =
                mHelperSurface ? mHelperSurface : mCurrentSurface;
            RefPtr<DrawTarget> dt = CreateDrawTargetForSurface(surface);
            RefPtr<SourceSurface> backgroundSurface =
                gfxPlatform::GetSourceSurfaceForSurface(dt, mBackground);
            dt->CopySurface(backgroundSurface, rect, rect.TopLeft());
        }
        // ... and hand off to the plugin
        // BEWARE: mBackground may die during this call
        PaintRectToSurface(rect, mCurrentSurface, Color());
    } else if (!temporarilyMakeVisible && mDoAlphaExtraction) {
        // We don't want to pay the expense of alpha extraction for
        // phony paints.
        PLUGIN_LOG_DEBUG(("  (with alpha recovery)"));
        PaintRectWithAlphaExtraction(rect, mCurrentSurface);
    } else {
        PLUGIN_LOG_DEBUG(("  (onto opaque surface)"));

        // If we're on a platform that needs helper surfaces for
        // plugins, and we're forcing a throwaway paint of a
        // wmode=transparent plugin, then make sure to use the helper
        // surface here.
        RefPtr<gfxASurface> target =
            (temporarilyMakeVisible && mHelperSurface) ?
            mHelperSurface : mCurrentSurface;

        PaintRectToSurface(rect, target, Color());
    }
    mHasPainted = true;

    if (temporarilyMakeVisible) {
        mWindow.clipRect.right = mWindow.clipRect.bottom = 0;

        PLUGIN_LOG_DEBUG(
            ("[InstanceChild][%p] Undoing temporary clipping w=<x=%d,y=%d, w=%d,h=%d>, clip=<l=%d,t=%d,r=%d,b=%d>",
             this, mWindow.x, mWindow.y, mWindow.width, mWindow.height,
             mWindow.clipRect.left, mWindow.clipRect.top, mWindow.clipRect.right, mWindow.clipRect.bottom));

        if (mPluginIface->setwindow) {
            mPluginIface->setwindow(&mData, &mWindow);
        }

        // Skip forwarding the results of the phony paint to the
        // browser.  We may have painted a transparent plugin using
        // the opaque-plugin path, which can result in wrong pixels.
        // We also don't want to pay the expense of forwarding the
        // surface for plugins that might really be invisible.
        mAccumulatedInvalidRect.SetRect(0, 0, mWindow.width, mWindow.height);
        return true;
    }

    NPRect r = { (uint16_t)rect.y, (uint16_t)rect.x,
                 (uint16_t)rect.YMost(), (uint16_t)rect.XMost() };
    SurfaceDescriptor currSurf;
#ifdef MOZ_X11
    if (mCurrentSurface->GetType() == gfxSurfaceType::Xlib) {
        gfxXlibSurface *xsurf = static_cast<gfxXlibSurface*>(mCurrentSurface.get());
        currSurf = SurfaceDescriptorX11(xsurf);
        // Need to sync all pending x-paint requests
        // before giving drawable to another process
        XSync(mWsInfo.display, False);
    } else
#endif
#ifdef XP_WIN
    if (SharedDIBSurface::IsSharedDIBSurface(mCurrentSurface)) {
        SharedDIBSurface* s = static_cast<SharedDIBSurface*>(mCurrentSurface.get());
        if (!mCurrentSurfaceActor) {
            base::SharedMemoryHandle handle = nullptr;
            s->ShareToProcess(OtherPid(), &handle);

            mCurrentSurfaceActor =
                SendPPluginSurfaceConstructor(handle,
                                              mCurrentSurface->GetSize(),
                                              haveTransparentPixels);
        }
        currSurf = mCurrentSurfaceActor;
        s->Flush();
    } else
#endif
    if (gfxSharedImageSurface::IsSharedImage(mCurrentSurface)) {
        currSurf = static_cast<gfxSharedImageSurface*>(mCurrentSurface.get())->GetShmem();
    } else {
        NS_RUNTIMEABORT("Surface type is not remotable");
        return false;
    }

    // Unused, except to possibly return a shmem to us
    SurfaceDescriptor returnSurf;

    if (!SendShow(r, currSurf, &returnSurf)) {
        return false;
    }

    SwapSurfaces();
    mSurfaceDifferenceRect = rect;
    return true;
}

bool
PluginInstanceChild::ReadbackDifferenceRect(const nsIntRect& rect)
{
    if (!mBackSurface)
        return false;

    // We can read safely from XSurface,SharedDIBSurface and Unsafe SharedMemory,
    // because PluginHost is not able to modify that surface
#if defined(MOZ_X11)
    if (mBackSurface->GetType() != gfxSurfaceType::Xlib &&
        !gfxSharedImageSurface::IsSharedImage(mBackSurface))
        return false;
#elif defined(XP_WIN)
    if (!SharedDIBSurface::IsSharedDIBSurface(mBackSurface))
        return false;
#endif

#if defined(MOZ_X11) || defined(XP_WIN)
    if (mCurrentSurface->GetContentType() != mBackSurface->GetContentType())
        return false;

    if (mSurfaceDifferenceRect.IsEmpty())
        return true;

    PLUGIN_LOG_DEBUG(
        ("[InstanceChild][%p] Reading back part of <x=%d,y=%d, w=%d,h=%d>",
         this, mSurfaceDifferenceRect.x, mSurfaceDifferenceRect.y,
         mSurfaceDifferenceRect.width, mSurfaceDifferenceRect.height));

    // Read back previous content
    RefPtr<DrawTarget> dt = CreateDrawTargetForSurface(mCurrentSurface);
    RefPtr<SourceSurface> source =
        gfxPlatform::GetSourceSurfaceForSurface(dt, mBackSurface);
    // Subtract from mSurfaceDifferenceRect area which is overlapping with rect
    nsIntRegion result;
    result.Sub(mSurfaceDifferenceRect, nsIntRegion(rect));
    for (auto iter = result.RectIter(); !iter.Done(); iter.Next()) {
        const nsIntRect& r = iter.Get();
        dt->CopySurface(source, r, r.TopLeft());
    }

    return true;
#else
    return false;
#endif
}

void
PluginInstanceChild::InvalidateRectDelayed(void)
{
    if (!mCurrentInvalidateTask) {
        return;
    }

    mCurrentInvalidateTask = nullptr;
    if (mAccumulatedInvalidRect.IsEmpty()) {
        return;
    }

    if (!ShowPluginFrame()) {
        AsyncShowPluginFrame();
    }
}

void
PluginInstanceChild::AsyncShowPluginFrame(void)
{
    if (mCurrentInvalidateTask) {
        return;
    }

    // When the plugin is using direct surfaces to draw, it is not driving
    // paints via paint events - it will drive painting via its own events
    // and/or DidComposite callbacks.
    if (IsUsingDirectDrawing()) {
        return;
    }

    mCurrentInvalidateTask =
        NewNonOwningCancelableRunnableMethod(this, &PluginInstanceChild::InvalidateRectDelayed);
    RefPtr<Runnable> addrefedTask = mCurrentInvalidateTask;
    MessageLoop::current()->PostTask(addrefedTask.forget());
}

void
PluginInstanceChild::InvalidateRect(NPRect* aInvalidRect)
{
    NS_ASSERTION(aInvalidRect, "Null pointer!");

    if (IsUsingDirectDrawing()) {
        NS_ASSERTION(false, "Should not call InvalidateRect() in direct surface mode!");
        return;
    }

    if (mLayersRendering) {
        nsIntRect r(aInvalidRect->left, aInvalidRect->top,
                    aInvalidRect->right - aInvalidRect->left,
                    aInvalidRect->bottom - aInvalidRect->top);

        mAccumulatedInvalidRect.UnionRect(r, mAccumulatedInvalidRect);
        // If we are able to paint and invalidate sent, then reset
        // accumulated rectangle
        AsyncShowPluginFrame();
        return;
    }

    // If we were going to use layers rendering but it's not set up
    // yet, and the plugin happens to call this first, we'll forward
    // the invalidation to the browser.  It's unclear whether
    // non-layers plugins need this rect forwarded when their window
    // width or height is 0, which it would be for layers plugins
    // before their first SetWindow().
    SendNPN_InvalidateRect(*aInvalidRect);
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvUpdateBackground(const SurfaceDescriptor& aBackground,
                                          const nsIntRect& aRect)
{
    MOZ_ASSERT(mIsTransparent, "Only transparent plugins use backgrounds");

    if (!mBackground) {
        // XXX refactor me
        switch (aBackground.type()) {
#ifdef MOZ_X11
        case SurfaceDescriptor::TSurfaceDescriptorX11: {
            mBackground = aBackground.get_SurfaceDescriptorX11().OpenForeign();
            break;
        }
#endif
        case SurfaceDescriptor::TShmem: {
            mBackground = gfxSharedImageSurface::Open(aBackground.get_Shmem());
            break;
        }
        default:
            NS_RUNTIMEABORT("Unexpected background surface descriptor");
        }

        if (!mBackground) {
            return IPC_FAIL_NO_REASON(this);
        }

        IntSize bgSize = mBackground->GetSize();
        mAccumulatedInvalidRect.UnionRect(mAccumulatedInvalidRect,
                                          nsIntRect(0, 0, bgSize.width, bgSize.height));
        AsyncShowPluginFrame();
        return IPC_OK();
    }

    // XXX refactor me
    mAccumulatedInvalidRect.UnionRect(aRect, mAccumulatedInvalidRect);

    // This must be asynchronous, because we may be nested within RPC messages
    // which do not expect to receiving paint events.
    AsyncShowPluginFrame();

    return IPC_OK();
}

PPluginBackgroundDestroyerChild*
PluginInstanceChild::AllocPPluginBackgroundDestroyerChild()
{
    return new PluginBackgroundDestroyerChild();
}

mozilla::ipc::IPCResult
PluginInstanceChild::RecvPPluginBackgroundDestroyerConstructor(
    PPluginBackgroundDestroyerChild* aActor)
{
    // Our background changed, so we have to invalidate the area
    // painted with the old background.  If the background was
    // destroyed because we have a new background, then we expect to
    // be notified of that "soon", before processing the asynchronous
    // invalidation here.  If we're *not* getting a new background,
    // our current front surface is stale and we want to repaint
    // "soon" so that we can hand the browser back a surface with
    // alpha values.  (We should be notified of that invalidation soon
    // too, but we don't assume that here.)
    if (mBackground) {
        IntSize bgsize = mBackground->GetSize();
        mAccumulatedInvalidRect.UnionRect(
            nsIntRect(0, 0, bgsize.width, bgsize.height), mAccumulatedInvalidRect);

        // NB: we don't have to XSync here because only ShowPluginFrame()
        // uses mBackground, and it always XSyncs after finishing.
        mBackground = nullptr;
        AsyncShowPluginFrame();
    }

    if (!PPluginBackgroundDestroyerChild::Send__delete__(aActor)) {
      return IPC_FAIL_NO_REASON(this);
    }
    return IPC_OK();
}

bool
PluginInstanceChild::DeallocPPluginBackgroundDestroyerChild(
    PPluginBackgroundDestroyerChild* aActor)
{
    delete aActor;
    return true;
}

uint32_t
PluginInstanceChild::ScheduleTimer(uint32_t interval, bool repeat,
                                   TimerFunc func)
{
    auto* t = new ChildTimer(this, interval, repeat, func);
    if (0 == t->ID()) {
        delete t;
        return 0;
    }

    mTimers.AppendElement(t);
    return t->ID();
}

void
PluginInstanceChild::UnscheduleTimer(uint32_t id)
{
    if (0 == id)
        return;

    mTimers.RemoveElement(id, ChildTimer::IDComparator());
}

void
PluginInstanceChild::AsyncCall(PluginThreadCallback aFunc, void* aUserData)
{
    RefPtr<ChildAsyncCall> task = new ChildAsyncCall(this, aFunc, aUserData);
    PostChildAsyncCall(task.forget());
}

void
PluginInstanceChild::PostChildAsyncCall(already_AddRefed<ChildAsyncCall> aTask)
{
    RefPtr<ChildAsyncCall> task = aTask;

    {
        MutexAutoLock lock(mAsyncCallMutex);
        mPendingAsyncCalls.AppendElement(task);
    }
    ProcessChild::message_loop()->PostTask(task.forget());
}

void
PluginInstanceChild::SwapSurfaces()
{
    RefPtr<gfxASurface> tmpsurf = mCurrentSurface;
#ifdef XP_WIN
    PPluginSurfaceChild* tmpactor = mCurrentSurfaceActor;
#endif

    mCurrentSurface = mBackSurface;
#ifdef XP_WIN
    mCurrentSurfaceActor = mBackSurfaceActor;
#endif

    mBackSurface = tmpsurf;
#ifdef XP_WIN
    mBackSurfaceActor = tmpactor;
#endif

    if (mCurrentSurface && mBackSurface &&
        (mCurrentSurface->GetSize() != mBackSurface->GetSize() ||
         mCurrentSurface->GetContentType() != mBackSurface->GetContentType())) {
        ClearCurrentSurface();
    }
}

void
PluginInstanceChild::ClearCurrentSurface()
{
    mCurrentSurface = nullptr;
#ifdef XP_WIN
    if (mCurrentSurfaceActor) {
        PPluginSurfaceChild::Send__delete__(mCurrentSurfaceActor);
        mCurrentSurfaceActor = nullptr;
    }
#endif
    mHelperSurface = nullptr;
}

void
PluginInstanceChild::ClearAllSurfaces()
{
    if (mBackSurface) {
        // Get last surface back, and drop it
        SurfaceDescriptor temp = null_t();
        NPRect r = { 0, 0, 1, 1 };
        SendShow(r, temp, &temp);
    }

    if (gfxSharedImageSurface::IsSharedImage(mCurrentSurface))
        DeallocShmem(static_cast<gfxSharedImageSurface*>(mCurrentSurface.get())->GetShmem());
    if (gfxSharedImageSurface::IsSharedImage(mBackSurface))
        DeallocShmem(static_cast<gfxSharedImageSurface*>(mBackSurface.get())->GetShmem());
    mCurrentSurface = nullptr;
    mBackSurface = nullptr;

#ifdef XP_WIN
    if (mCurrentSurfaceActor) {
        PPluginSurfaceChild::Send__delete__(mCurrentSurfaceActor);
        mCurrentSurfaceActor = nullptr;
    }
    if (mBackSurfaceActor) {
        PPluginSurfaceChild::Send__delete__(mBackSurfaceActor);
        mBackSurfaceActor = nullptr;
    }
#endif
}

static void
InvalidateObjects(nsTHashtable<DeletingObjectEntry>& aEntries)
{
    for (auto iter = aEntries.Iter(); !iter.Done(); iter.Next()) {
        DeletingObjectEntry* e = iter.Get();
        NPObject* o = e->GetKey();
        if (!e->mDeleted && o->_class && o->_class->invalidate) {
            o->_class->invalidate(o);
        }
    }
}

static void
DeleteObjects(nsTHashtable<DeletingObjectEntry>& aEntries)
{
    for (auto iter = aEntries.Iter(); !iter.Done(); iter.Next()) {
        DeletingObjectEntry* e = iter.Get();
        NPObject* o = e->GetKey();
        if (!e->mDeleted) {
            e->mDeleted = true;

#ifdef NS_BUILD_REFCNT_LOGGING
            {
                int32_t refcnt = o->referenceCount;
                while (refcnt) {
                    --refcnt;
                    NS_LOG_RELEASE(o, refcnt, "NPObject");
                }
            }
#endif

            PluginModuleChild::DeallocNPObject(o);
        }
    }
}

void
PluginInstanceChild::Destroy()
{
    if (mDestroyed) {
        return;
    }
    if (mStackDepth != 0) {
        NS_RUNTIMEABORT("Destroying plugin instance on the stack.");
    }
    mDestroyed = true;

    InfallibleTArray<PBrowserStreamChild*> streams;
    ManagedPBrowserStreamChild(streams);

    // First make sure none of these streams become deleted
    for (uint32_t i = 0; i < streams.Length(); ) {
        if (static_cast<BrowserStreamChild*>(streams[i])->InstanceDying())
            ++i;
        else
            streams.RemoveElementAt(i);
    }
    for (uint32_t i = 0; i < streams.Length(); ++i)
        static_cast<BrowserStreamChild*>(streams[i])->FinishDelivery();

    mTimers.Clear();

    // NPP_Destroy() should be a synchronization point for plugin threads
    // calling NPN_AsyncCall: after this function returns, they are no longer
    // allowed to make async calls on this instance.
    static_cast<PluginModuleChild *>(Manager())->NPP_Destroy(this);
    mData.ndata = 0;

    if (mCurrentInvalidateTask) {
        mCurrentInvalidateTask->Cancel();
        mCurrentInvalidateTask = nullptr;
    }
    if (mCurrentAsyncSetWindowTask) {
        mCurrentAsyncSetWindowTask->Cancel();
        mCurrentAsyncSetWindowTask = nullptr;
    }
    {
        MutexAutoLock autoLock(mAsyncInvalidateMutex);
        if (mAsyncInvalidateTask) {
            mAsyncInvalidateTask->Cancel();
            mAsyncInvalidateTask = nullptr;
        }
    }

    ClearAllSurfaces();
    mDirectBitmaps.Clear();

    mDeletingHash = new nsTHashtable<DeletingObjectEntry>;
    PluginScriptableObjectChild::NotifyOfInstanceShutdown(this);

    InvalidateObjects(*mDeletingHash);
    DeleteObjects(*mDeletingHash);

    // Null out our cached actors as they should have been killed in the
    // PluginInstanceDestroyed call above.
    mCachedWindowActor = nullptr;
    mCachedElementActor = nullptr;

    // Pending async calls are discarded, not delivered. This matches the
    // in-process behavior.
    for (uint32_t i = 0; i < mPendingAsyncCalls.Length(); ++i)
        mPendingAsyncCalls[i]->Cancel();

    mPendingAsyncCalls.Clear();

#ifdef MOZ_WIDGET_GTK
    if (mWindow.type == NPWindowTypeWindow && !mXEmbed) {
      xt_client_xloop_destroy();
    }
#endif
#if defined(MOZ_X11) && defined(XP_UNIX) && !defined(XP_MACOSX)
    DeleteWindow();
#endif
}

mozilla::ipc::IPCResult
PluginInstanceChild::AnswerNPP_Destroy(NPError* aResult)
{
    PLUGIN_LOG_DEBUG_METHOD;
    AssertPluginThread();
    *aResult = NPERR_NO_ERROR;

    Destroy();

    return IPC_OK();
}

void
PluginInstanceChild::ActorDestroy(ActorDestroyReason why)
{
#ifdef XP_WIN
    // ClearAllSurfaces() should not try to send anything after ActorDestroy.
    mCurrentSurfaceActor = nullptr;
    mBackSurfaceActor = nullptr;
#endif

    Destroy();
}

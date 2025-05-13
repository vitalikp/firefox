/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: sw=4 ts=4 et :
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/plugins/PluginModuleParent.h"

#include "base/process_util.h"
#include "mozilla/Attributes.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/plugins/BrowserStreamParent.h"
#include "mozilla/plugins/PluginBridge.h"
#include "mozilla/plugins/PluginInstanceParent.h"
#include "mozilla/Preferences.h"
#include "mozilla/ProcessHangMonitor.h"
#include "mozilla/Services.h"
#include "mozilla/Telemetry.h"
#include "mozilla/Unused.h"
#include "nsAutoPtr.h"
#include "nsCRT.h"
#include "nsIFile.h"
#include "nsIObserverService.h"
#include "nsIXULRuntime.h"
#include "nsNPAPIPlugin.h"
#include "nsPrintfCString.h"
#include "prsystem.h"
#include "prclist.h"
#include "PluginQuirks.h"
#include "GeckoProfiler.h"
#include "ProfilerParent.h"
#include "nsPluginTags.h"
#include "nsUnicharUtils.h"
#include "mozilla/layers/TextureClientRecycleAllocator.h"

#ifdef MOZ_WIDGET_GTK
#include <glib.h>
#endif

using base::KillProcess;

using mozilla::PluginLibrary;
using mozilla::ipc::MessageChannel;
using mozilla::ipc::GeckoChildProcessHost;

using namespace mozilla;
using namespace mozilla::plugins;
using namespace mozilla::plugins::parent;


static const char kContentTimeoutPref[] = "dom.ipc.plugins.contentTimeoutSecs";
static const char kChildTimeoutPref[] = "dom.ipc.plugins.timeoutSecs";
static const char kParentTimeoutPref[] = "dom.ipc.plugins.parentTimeoutSecs";
static const char kLaunchTimeoutPref[] = "dom.ipc.plugins.processLaunchTimeoutSecs";
#define CHILD_TIMEOUT_PREF kChildTimeoutPref

bool
mozilla::plugins::SetupBridge(uint32_t aPluginId,
                              dom::ContentParent* aContentParent,
                              nsresult* rv,
                              uint32_t* runID,
                              ipc::Endpoint<PPluginModuleParent>* aEndpoint)
{
    PROFILER_LABEL_FUNC(js::ProfileEntry::Category::OTHER);
    if (NS_WARN_IF(!rv) || NS_WARN_IF(!runID)) {
        return false;
    }

    RefPtr<nsPluginHost> host = nsPluginHost::GetInst();
    RefPtr<nsNPAPIPlugin> plugin;
    *rv = host->GetPluginForContentProcess(aPluginId, getter_AddRefs(plugin));
    if (NS_FAILED(*rv)) {
        return true;
    }
    PluginModuleChromeParent* chromeParent = static_cast<PluginModuleChromeParent*>(plugin->GetLibrary());
    /*
     *  We can't accumulate BLOCKED_ON_PLUGIN_MODULE_INIT_MS until here because
     *  its histogram key is not available until *after* NP_Initialize.
     */
    chromeParent->AccumulateModuleInitBlockedTime();
    *rv = chromeParent->GetRunID(runID);
    if (NS_FAILED(*rv)) {
        return true;
    }

    ipc::Endpoint<PPluginModuleParent> parent;
    ipc::Endpoint<PPluginModuleChild> child;

    *rv = PPluginModule::CreateEndpoints(aContentParent->OtherPid(),
                                         chromeParent->OtherPid(),
                                         &parent, &child);
    if (NS_FAILED(*rv)) {
        return true;
    }

    *aEndpoint = Move(parent);

    if (!chromeParent->SendInitPluginModuleChild(Move(child))) {
        *rv = NS_ERROR_BRIDGE_OPEN_CHILD;
        return true;
    }

    return true;
}

namespace {

/**
 * Objects of this class remain linked until an error occurs in the
 * plugin initialization sequence.
 */
class PluginModuleMapping : public PRCList
{
public:
    explicit PluginModuleMapping(uint32_t aPluginId)
        : mPluginId(aPluginId)
        , mProcessIdValid(false)
        , mModule(nullptr)
        , mChannelOpened(false)
    {
        MOZ_COUNT_CTOR(PluginModuleMapping);
        PR_INIT_CLIST(this);
        PR_APPEND_LINK(this, &sModuleListHead);
    }

    ~PluginModuleMapping()
    {
        PR_REMOVE_LINK(this);
        MOZ_COUNT_DTOR(PluginModuleMapping);
    }

    bool
    IsChannelOpened() const
    {
        return mChannelOpened;
    }

    void
    SetChannelOpened()
    {
        mChannelOpened = true;
    }

    PluginModuleContentParent*
    GetModule()
    {
        if (!mModule) {
            mModule = new PluginModuleContentParent();
        }
        return mModule;
    }

    static PluginModuleMapping*
    AssociateWithProcessId(uint32_t aPluginId, base::ProcessId aProcessId)
    {
        PluginModuleMapping* mapping =
            static_cast<PluginModuleMapping*>(PR_NEXT_LINK(&sModuleListHead));
        while (mapping != &sModuleListHead) {
            if (mapping->mPluginId == aPluginId) {
                mapping->AssociateWithProcessId(aProcessId);
                return mapping;
            }
            mapping = static_cast<PluginModuleMapping*>(PR_NEXT_LINK(mapping));
        }
        return nullptr;
    }

    static PluginModuleMapping*
    Resolve(base::ProcessId aProcessId)
    {
        PluginModuleMapping* mapping = nullptr;

        if (sIsLoadModuleOnStack) {
            // Special case: If loading synchronously, we just need to access
            // the tail entry of the list.
            mapping =
                static_cast<PluginModuleMapping*>(PR_LIST_TAIL(&sModuleListHead));
            MOZ_ASSERT(mapping);
            return mapping;
        }

        mapping =
            static_cast<PluginModuleMapping*>(PR_NEXT_LINK(&sModuleListHead));
        while (mapping != &sModuleListHead) {
            if (mapping->mProcessIdValid && mapping->mProcessId == aProcessId) {
                return mapping;
            }
            mapping = static_cast<PluginModuleMapping*>(PR_NEXT_LINK(mapping));
        }
        return nullptr;
    }

    static PluginModuleMapping*
    FindModuleByPluginId(uint32_t aPluginId)
    {
        PluginModuleMapping* mapping =
            static_cast<PluginModuleMapping*>(PR_NEXT_LINK(&sModuleListHead));
        while (mapping != &sModuleListHead) {
            if (mapping->mPluginId == aPluginId) {
                return mapping;
            }
            mapping = static_cast<PluginModuleMapping*>(PR_NEXT_LINK(mapping));
        }
        return nullptr;
    }

    class MOZ_RAII NotifyLoadingModule
    {
    public:
        explicit NotifyLoadingModule(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM)
        {
            MOZ_GUARD_OBJECT_NOTIFIER_INIT;
            PluginModuleMapping::sIsLoadModuleOnStack = true;
        }

        ~NotifyLoadingModule()
        {
            PluginModuleMapping::sIsLoadModuleOnStack = false;
        }

    private:
        MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
    };

private:
    void
    AssociateWithProcessId(base::ProcessId aProcessId)
    {
        MOZ_ASSERT(!mProcessIdValid);
        mProcessId = aProcessId;
        mProcessIdValid = true;
    }

    uint32_t mPluginId;
    bool mProcessIdValid;
    base::ProcessId mProcessId;
    PluginModuleContentParent* mModule;
    bool mChannelOpened;

    friend class NotifyLoadingModule;

    static PRCList sModuleListHead;
    static bool sIsLoadModuleOnStack;
};

PRCList PluginModuleMapping::sModuleListHead =
    PR_INIT_STATIC_CLIST(&PluginModuleMapping::sModuleListHead);

bool PluginModuleMapping::sIsLoadModuleOnStack = false;

} // namespace

static PluginModuleChromeParent*
PluginModuleChromeParentForId(const uint32_t aPluginId)
{
  MOZ_ASSERT(XRE_IsParentProcess());

  RefPtr<nsPluginHost> host = nsPluginHost::GetInst();
  nsPluginTag* pluginTag = host->PluginWithId(aPluginId);
  if (!pluginTag || !pluginTag->mPlugin) {
    return nullptr;
  }
  RefPtr<nsNPAPIPlugin> plugin = pluginTag->mPlugin;

  return static_cast<PluginModuleChromeParent*>(plugin->GetLibrary());
}

void
mozilla::plugins::TakeFullMinidump(uint32_t aPluginId,
                                   base::ProcessId aContentProcessId,
                                   const nsAString& aBrowserDumpId,
                                   nsString& aDumpId)
{
  PluginModuleChromeParent* chromeParent =
    PluginModuleChromeParentForId(aPluginId);

  if (chromeParent) {
    chromeParent->TakeFullMinidump(aContentProcessId, aBrowserDumpId, aDumpId);
  }
}

void
mozilla::plugins::TerminatePlugin(uint32_t aPluginId,
                                  base::ProcessId aContentProcessId,
                                  const nsCString& aMonitorDescription,
                                  const nsAString& aDumpId)
{
  PluginModuleChromeParent* chromeParent =
    PluginModuleChromeParentForId(aPluginId);

  if (chromeParent) {
    chromeParent->TerminateChildProcess(MessageLoop::current(),
                                        aContentProcessId,
                                        aMonitorDescription,
                                        aDumpId);
  }
}

/* static */ PluginLibrary*
PluginModuleContentParent::LoadModule(uint32_t aPluginId,
                                      nsPluginTag* aPluginTag)
{
    PluginModuleMapping::NotifyLoadingModule loadingModule;
    nsAutoPtr<PluginModuleMapping> mapping(new PluginModuleMapping(aPluginId));

    MOZ_ASSERT(XRE_IsContentProcess());

    /*
     * We send a LoadPlugin message to the chrome process using an intr
     * message. Before it sends its response, it sends a message to create
     * PluginModuleParent instance. That message is handled by
     * PluginModuleContentParent::Initialize, which saves the instance in
     * its module mapping. We fetch it from there after LoadPlugin finishes.
     */
    dom::ContentChild* cp = dom::ContentChild::GetSingleton();
    nsresult rv;
    uint32_t runID;
    Endpoint<PPluginModuleParent> endpoint;
    TimeStamp sendLoadPluginStart = TimeStamp::Now();
    if (!cp->SendLoadPlugin(aPluginId, &rv, &runID, &endpoint) ||
        NS_FAILED(rv)) {
        return nullptr;
    }
    Initialize(Move(endpoint));
    TimeStamp sendLoadPluginEnd = TimeStamp::Now();

    PluginModuleContentParent* parent = mapping->GetModule();
    MOZ_ASSERT(parent);
    parent->mTimeBlocked += (sendLoadPluginEnd - sendLoadPluginStart);

    if (!mapping->IsChannelOpened()) {
        // mapping is linked into PluginModuleMapping::sModuleListHead and is
        // needed later, so since this function is returning successfully we
        // forget it here.
        mapping.forget();
    }

    parent->mPluginId = aPluginId;
    parent->mRunID = runID;

    return parent;
}

/* static */ void
PluginModuleContentParent::Initialize(Endpoint<PPluginModuleParent>&& aEndpoint)
{
    nsAutoPtr<PluginModuleMapping> moduleMapping(
        PluginModuleMapping::Resolve(aEndpoint.OtherPid()));
    MOZ_ASSERT(moduleMapping);
    PluginModuleContentParent* parent = moduleMapping->GetModule();
    MOZ_ASSERT(parent);

    DebugOnly<bool> ok = aEndpoint.Bind(parent);
    MOZ_ASSERT(ok);

    moduleMapping->SetChannelOpened();

    // Request Windows message deferral behavior on our channel. This
    // applies to the top level and all sub plugin protocols since they
    // all share the same channel.
    parent->GetIPCChannel()->SetChannelFlags(MessageChannel::REQUIRE_DEFERRED_MESSAGE_PROTECTION);

    TimeoutChanged(kContentTimeoutPref, parent);

    // moduleMapping is linked into PluginModuleMapping::sModuleListHead and is
    // needed later, so since this function is returning successfully we
    // forget it here.
    moduleMapping.forget();
}

// static
PluginLibrary*
PluginModuleChromeParent::LoadModule(const char* aFilePath, uint32_t aPluginId,
                                     nsPluginTag* aPluginTag)
{
    PLUGIN_LOG_DEBUG_FUNCTION;

    nsAutoPtr<PluginModuleChromeParent> parent(
            new PluginModuleChromeParent(aFilePath, aPluginId,
                                         aPluginTag->mSandboxLevel));
    UniquePtr<LaunchCompleteTask> onLaunchedRunnable(new LaunchedTask(parent));
    TimeStamp launchStart = TimeStamp::Now();
    bool launched = parent->mSubprocess->Launch(Move(onLaunchedRunnable),
                                                aPluginTag->mSandboxLevel);
    if (!launched) {
        // We never reached open
        parent->mShutdown = true;
        return nullptr;
    }
    parent->mIsFlashPlugin = aPluginTag->mIsFlashPlugin;
    int32_t launchTimeoutSecs = Preferences::GetInt(kLaunchTimeoutPref, 0);
    if (!parent->mSubprocess->WaitUntilConnected(launchTimeoutSecs * 1000)) {
        parent->mShutdown = true;
        return nullptr;
    }
    TimeStamp launchEnd = TimeStamp::Now();
    parent->mTimeBlocked = (launchEnd - launchStart);
    return parent.forget();
}

void
PluginModuleChromeParent::OnProcessLaunched(const bool aSucceeded)
{
    if (!aSucceeded) {
        mShutdown = true;
        OnInitFailure();
        return;
    }
    // We may have already been initialized by another call that was waiting
    // for process connect. If so, this function doesn't need to run.
    if (mShutdown) {
        return;
    }

    Open(mSubprocess->GetChannel(),
         base::GetProcId(mSubprocess->GetChildProcessHandle()));

    // Request Windows message deferral behavior on our channel. This
    // applies to the top level and all sub plugin protocols since they
    // all share the same channel.
    GetIPCChannel()->SetChannelFlags(MessageChannel::REQUIRE_DEFERRED_MESSAGE_PROTECTION);

    TimeoutChanged(CHILD_TIMEOUT_PREF, this);

    Preferences::RegisterCallback(TimeoutChanged, kChildTimeoutPref, this);
    Preferences::RegisterCallback(TimeoutChanged, kParentTimeoutPref, this);

    RegisterSettingsCallbacks();

#ifdef MOZ_GECKO_PROFILER
    Unused << SendInitProfiler(ProfilerParent::CreateForProcess(OtherPid()));
#endif
}

PluginModuleParent::PluginModuleParent(bool aIsChrome)
    : mQuirks(QUIRKS_NOT_INITIALIZED)
    , mIsChrome(aIsChrome)
    , mShutdown(false)
    , mHadLocalInstance(false)
    , mClearSiteDataSupported(false)
    , mGetSitesWithDataSupported(false)
    , mNPNIface(nullptr)
    , mNPPIface(nullptr)
    , mPlugin(nullptr)
    , mTaskFactory(this)
    , mSandboxLevel(0)
    , mIsFlashPlugin(false)
{
}

PluginModuleParent::~PluginModuleParent()
{
    if (!OkToCleanup()) {
        NS_RUNTIMEABORT("unsafe destruction");
    }

    if (!mShutdown) {
        NS_WARNING("Plugin host deleted the module without shutting down.");
        NPError err;
        NP_Shutdown(&err);
    }
}

PluginModuleContentParent::PluginModuleContentParent()
    : PluginModuleParent(false)
{
    Preferences::RegisterCallback(TimeoutChanged, kContentTimeoutPref, this);
}

PluginModuleContentParent::~PluginModuleContentParent()
{
    Preferences::UnregisterCallback(TimeoutChanged, kContentTimeoutPref, this);
}

PluginModuleChromeParent::PluginModuleChromeParent(const char* aFilePath,
                                                   uint32_t aPluginId,
                                                   int32_t aSandboxLevel)
    : PluginModuleParent(true)
    , mSubprocess(new PluginProcessParent(aFilePath))
    , mPluginId(aPluginId)
    , mChromeTaskFactory(this)
    , mHangAnnotationFlags(0)
{
    NS_ASSERTION(mSubprocess, "Out of memory!");
    mSandboxLevel = aSandboxLevel;
    mRunID = GeckoChildProcessHost::GetUniqueID();

    mozilla::HangMonitor::RegisterAnnotator(*this);
}

PluginModuleChromeParent::~PluginModuleChromeParent()
{
    if (!OkToCleanup()) {
        MOZ_CRASH("unsafe destruction");
    }

    if (!mShutdown) {
        NS_WARNING("Plugin host deleted the module without shutting down.");
        NPError err;
        NP_Shutdown(&err);
    }

    NS_ASSERTION(mShutdown, "NP_Shutdown didn't");

    if (mSubprocess) {
        mSubprocess->Delete();
        mSubprocess = nullptr;
    }

    UnregisterSettingsCallbacks();

    Preferences::UnregisterCallback(TimeoutChanged, kChildTimeoutPref, this);
    Preferences::UnregisterCallback(TimeoutChanged, kParentTimeoutPref, this);

    mozilla::HangMonitor::UnregisterAnnotator(*this);
}

void
PluginModuleParent::SetChildTimeout(const int32_t aChildTimeout)
{
    int32_t timeoutMs = (aChildTimeout > 0) ? (1000 * aChildTimeout) :
                      MessageChannel::kNoTimeout;
    SetReplyTimeoutMs(timeoutMs);
}

void
PluginModuleParent::TimeoutChanged(const char* aPref, void* aModule)
{
    PluginModuleParent* module = static_cast<PluginModuleParent*>(aModule);

    NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
    if (!strcmp(aPref, kChildTimeoutPref)) {
      MOZ_ASSERT(module->IsChrome());
      // The timeout value used by the parent for children
      int32_t timeoutSecs = Preferences::GetInt(kChildTimeoutPref, 0);
      module->SetChildTimeout(timeoutSecs);
    } else if (!strcmp(aPref, kParentTimeoutPref)) {
      // The timeout value used by the child for its parent
      MOZ_ASSERT(module->IsChrome());
      int32_t timeoutSecs = Preferences::GetInt(kParentTimeoutPref, 0);
      Unused << static_cast<PluginModuleChromeParent*>(module)->SendSetParentHangTimeout(timeoutSecs);
    } else if (!strcmp(aPref, kContentTimeoutPref)) {
      MOZ_ASSERT(!module->IsChrome());
      int32_t timeoutSecs = Preferences::GetInt(kContentTimeoutPref, 0);
      module->SetChildTimeout(timeoutSecs);
    }
}

void
PluginModuleChromeParent::CleanupFromTimeout(const bool aFromHangUI)
{
    if (mShutdown) {
      return;
    }

    if (!OkToCleanup()) {
        // there's still plugin code on the C++ stack, try again
        MessageLoop::current()->PostDelayedTask(
            mChromeTaskFactory.NewRunnableMethod(
                &PluginModuleChromeParent::CleanupFromTimeout, aFromHangUI), 10);
        return;
    }

    /* If the plugin container was terminated by the Plugin Hang UI,
       then either the I/O thread detects a channel error, or the
       main thread must set the error (whomever gets there first).
       OTOH, if we terminate and return false from
       ShouldContinueFromReplyTimeout, then the channel state has
       already been set to ChannelTimeout and we should call the
       regular Close function. */
    if (aFromHangUI) {
        GetIPCChannel()->CloseWithError();
    } else {
        Close();
    }
}

/**
 * This function converts the topmost routing id on the call stack (as recorded
 * by the MessageChannel) into a pointer to a IProtocol object.
 */
mozilla::ipc::IProtocol*
PluginModuleChromeParent::GetInvokingProtocol()
{
    int32_t routingId = GetIPCChannel()->GetTopmostMessageRoutingId();
    // Nothing being routed. No protocol. Just return nullptr.
    if (routingId == MSG_ROUTING_NONE) {
        return nullptr;
    }
    // If routingId is MSG_ROUTING_CONTROL then we're dealing with control
    // messages that were initiated by the topmost managing protocol, ie. this.
    if (routingId == MSG_ROUTING_CONTROL) {
        return this;
    }
    // Otherwise we can look up the protocol object by the routing id.
    mozilla::ipc::IProtocol* protocol = Lookup(routingId);
    return protocol;
}

/**
 * This function examines the IProtocol object parameter and converts it into
 * the PluginInstanceParent object that is associated with that protocol, if
 * any. Since PluginInstanceParent manages subprotocols, this function needs
 * to determine whether |aProtocol| is a subprotocol, and if so it needs to
 * obtain the protocol's manager.
 *
 * This function needs to be updated if the subprotocols are modified in
 * PPluginInstance.ipdl.
 */
PluginInstanceParent*
PluginModuleChromeParent::GetManagingInstance(mozilla::ipc::IProtocol* aProtocol)
{
    MOZ_ASSERT(aProtocol);
    mozilla::ipc::IProtocol* listener = aProtocol;
    switch (listener->GetProtocolTypeId()) {
        case PPluginInstanceMsgStart:
            // In this case, aProtocol is the instance itself. Just cast it.
            return static_cast<PluginInstanceParent*>(aProtocol);
        case PPluginBackgroundDestroyerMsgStart: {
            PPluginBackgroundDestroyerParent* actor =
                static_cast<PPluginBackgroundDestroyerParent*>(aProtocol);
            return static_cast<PluginInstanceParent*>(actor->Manager());
        }
        case PPluginScriptableObjectMsgStart: {
            PPluginScriptableObjectParent* actor =
                static_cast<PPluginScriptableObjectParent*>(aProtocol);
            return static_cast<PluginInstanceParent*>(actor->Manager());
        }
        case PBrowserStreamMsgStart: {
            PBrowserStreamParent* actor =
                static_cast<PBrowserStreamParent*>(aProtocol);
            return static_cast<PluginInstanceParent*>(actor->Manager());
        }
        case PPluginStreamMsgStart: {
            PPluginStreamParent* actor =
                static_cast<PPluginStreamParent*>(aProtocol);
            return static_cast<PluginInstanceParent*>(actor->Manager());
        }
        case PStreamNotifyMsgStart: {
            PStreamNotifyParent* actor =
                static_cast<PStreamNotifyParent*>(aProtocol);
            return static_cast<PluginInstanceParent*>(actor->Manager());
        }
        default:
            return nullptr;
    }
}

void
PluginModuleChromeParent::EnteredCxxStack()
{
    mHangAnnotationFlags |= kInPluginCall;
}

void
PluginModuleChromeParent::ExitedCxxStack()
{
    mHangAnnotationFlags = 0;
}

/**
 * This function is always called by the HangMonitor thread.
 */
void
PluginModuleChromeParent::AnnotateHang(mozilla::HangMonitor::HangAnnotations& aAnnotations)
{
    uint32_t flags = mHangAnnotationFlags;
    if (flags) {
        /* We don't actually annotate anything specifically for kInPluginCall;
           we use it to determine whether to annotate other things. It will
           be pretty obvious from the ChromeHang stack that we're in a plugin
           call when the hang occurred. */
        if (flags & kHangUIShown) {
            aAnnotations.AddAnnotation(NS_LITERAL_STRING("HangUIShown"),
                                       true);
        }
        if (flags & kHangUIContinued) {
            aAnnotations.AddAnnotation(NS_LITERAL_STRING("HangUIContinued"),
                                       true);
        }
        if (flags & kHangUIDontShow) {
            aAnnotations.AddAnnotation(NS_LITERAL_STRING("HangUIDontShow"),
                                       true);
        }
        aAnnotations.AddAnnotation(NS_LITERAL_STRING("pluginName"), mPluginName);
        aAnnotations.AddAnnotation(NS_LITERAL_STRING("pluginVersion"),
                                   mPluginVersion);
    }
}

bool
PluginModuleChromeParent::ShouldContinueFromReplyTimeout()
{
    if (mIsFlashPlugin) {
        MessageLoop::current()->PostTask(
            mTaskFactory.NewRunnableMethod(
                &PluginModuleChromeParent::NotifyFlashHang));
    }

    TerminateChildProcess(MessageLoop::current(),
                          mozilla::ipc::kInvalidProcessId,
                          NS_LITERAL_CSTRING("ModalHangUI"),
                          EmptyString());
    GetIPCChannel()->CloseWithTimeout();
    return false;
}

bool
PluginModuleContentParent::ShouldContinueFromReplyTimeout()
{
    RefPtr<ProcessHangMonitor> monitor = ProcessHangMonitor::Get();
    if (!monitor) {
        return true;
    }
    monitor->NotifyPluginHang(mPluginId);
    return true;
}

void
PluginModuleContentParent::OnExitedSyncSend()
{
    ProcessHangMonitor::ClearHang();
}

void
PluginModuleChromeParent::TakeFullMinidump(base::ProcessId aContentPid,
                                           const nsAString& aBrowserDumpId,
                                           nsString& aDumpId)
{
}

void
PluginModuleChromeParent::TerminateChildProcess(MessageLoop* aMsgLoop,
                                                base::ProcessId aContentPid,
                                                const nsCString& aMonitorDescription,
                                                const nsAString& aDumpId)
{
    mozilla::ipc::ScopedProcessHandle geckoChildProcess;
    bool childOpened = base::OpenProcessHandle(OtherPid(),
                                               &geckoChildProcess.rwget());

    // this must run before the error notification from the channel,
    // or not at all
    bool isFromHangUI = aMsgLoop != MessageLoop::current();
    aMsgLoop->PostTask(
        mChromeTaskFactory.NewRunnableMethod(
            &PluginModuleChromeParent::CleanupFromTimeout, isFromHangUI));

    if (!childOpened || !KillProcess(geckoChildProcess, 1, false)) {
        NS_WARNING("failed to kill subprocess!");
    }
}

bool
PluginModuleParent::GetPluginDetails()
{
    RefPtr<nsPluginHost> host = nsPluginHost::GetInst();
    if (!host) {
        return false;
    }
    nsPluginTag* pluginTag = host->TagForPlugin(mPlugin);
    if (!pluginTag) {
        return false;
    }
    mPluginName = pluginTag->Name();
    mPluginVersion = pluginTag->Version();
    mPluginFilename = pluginTag->FileName();
    mIsFlashPlugin = pluginTag->mIsFlashPlugin;
    mSandboxLevel = pluginTag->mSandboxLevel;
    return true;
}

void
PluginModuleParent::InitQuirksModes(const nsCString& aMimeType)
{
    if (mQuirks != QUIRKS_NOT_INITIALIZED) {
      return;
    }

    mQuirks = GetQuirksFromMimeTypeAndFilename(aMimeType, mPluginFilename);
}

void
PluginModuleParent::ActorDestroy(ActorDestroyReason why)
{
    switch (why) {
    case AbnormalShutdown: {
        mShutdown = true;
        // Defer the PluginCrashed method so that we don't re-enter
        // and potentially modify the actor child list while enumerating it.
        if (mPlugin)
            MessageLoop::current()->PostTask(
                mTaskFactory.NewRunnableMethod(
                    &PluginModuleParent::NotifyPluginCrashed));
        break;
    }
    case NormalShutdown:
        mShutdown = true;
        break;

    default:
        NS_RUNTIMEABORT("Unexpected shutdown reason for toplevel actor.");
    }
}

nsresult
PluginModuleParent::GetRunID(uint32_t* aRunID)
{
    if (NS_WARN_IF(!aRunID)) {
      return NS_ERROR_INVALID_POINTER;
    }
    *aRunID = mRunID;
    return NS_OK;
}

void
PluginModuleChromeParent::ActorDestroy(ActorDestroyReason why)
{
    if (why == AbnormalShutdown) {
        Telemetry::Accumulate(Telemetry::SUBPROCESS_ABNORMAL_ABORT,
                              NS_LITERAL_CSTRING("plugin"), 1);
    }

    // We can't broadcast settings changes anymore.
    UnregisterSettingsCallbacks();

    PluginModuleParent::ActorDestroy(why);
}

void
PluginModuleParent::NotifyFlashHang()
{
    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (obs) {
        obs->NotifyObservers(nullptr, "flash-plugin-hang", nullptr);
    }
}

void
PluginModuleParent::NotifyPluginCrashed()
{
    if (!OkToCleanup()) {
        // there's still plugin code on the C++ stack.  try again
        MessageLoop::current()->PostDelayedTask(
            mTaskFactory.NewRunnableMethod(
                &PluginModuleParent::NotifyPluginCrashed), 10);
        return;
    }

    nsString dumpID;
    nsString browserDumpID;
    if (mPlugin)
        mPlugin->PluginCrashed(dumpID, browserDumpID);
}

PPluginInstanceParent*
PluginModuleParent::AllocPPluginInstanceParent(const nsCString& aMimeType,
                                               const InfallibleTArray<nsCString>& aNames,
                                               const InfallibleTArray<nsCString>& aValues)
{
    NS_ERROR("Not reachable!");
    return nullptr;
}

bool
PluginModuleParent::DeallocPPluginInstanceParent(PPluginInstanceParent* aActor)
{
    PLUGIN_LOG_DEBUG_METHOD;
    delete aActor;
    return true;
}

void
PluginModuleParent::SetPluginFuncs(NPPluginFuncs* aFuncs)
{
    MOZ_ASSERT(aFuncs);

    aFuncs->version = (NP_VERSION_MAJOR << 8) | NP_VERSION_MINOR;
    aFuncs->javaClass = nullptr;

    // Gecko should always call these functions through a PluginLibrary object.
    aFuncs->newp = nullptr;
    aFuncs->clearsitedata = nullptr;
    aFuncs->getsiteswithdata = nullptr;

    aFuncs->destroy = NPP_Destroy;
    aFuncs->setwindow = NPP_SetWindow;
    aFuncs->newstream = NPP_NewStream;
    aFuncs->destroystream = NPP_DestroyStream;
    aFuncs->asfile = NPP_StreamAsFile;
    aFuncs->writeready = NPP_WriteReady;
    aFuncs->write = NPP_Write;
    aFuncs->print = NPP_Print;
    aFuncs->event = NPP_HandleEvent;
    aFuncs->urlnotify = NPP_URLNotify;
    aFuncs->getvalue = NPP_GetValue;
    aFuncs->setvalue = NPP_SetValue;
    aFuncs->gotfocus = nullptr;
    aFuncs->lostfocus = nullptr;
    aFuncs->urlredirectnotify = nullptr;

    // Provide 'NPP_URLRedirectNotify', 'NPP_ClearSiteData', and
    // 'NPP_GetSitesWithData' functionality if it is supported by the plugin.
    bool urlRedirectSupported = false;
    Unused << CallOptionalFunctionsSupported(&urlRedirectSupported,
                                             &mClearSiteDataSupported,
                                             &mGetSitesWithDataSupported);
    if (urlRedirectSupported) {
      aFuncs->urlredirectnotify = NPP_URLRedirectNotify;
    }
}

NPError
PluginModuleParent::NPP_Destroy(NPP instance,
                                NPSavedData** saved)
{
    // FIXME/cjones:
    //  (1) send a "destroy" message to the child
    //  (2) the child shuts down its instance
    //  (3) remove both parent and child IDs from map
    //  (4) free parent

    PLUGIN_LOG_DEBUG_FUNCTION;
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    if (!pip)
        return NPERR_NO_ERROR;

    NPError retval = pip->Destroy();
    instance->pdata = nullptr;

    Unused << PluginInstanceParent::Call__delete__(pip);
    return retval;
}

NPError
PluginModuleParent::NPP_NewStream(NPP instance, NPMIMEType type,
                                  NPStream* stream, NPBool seekable,
                                  uint16_t* stype)
{
    PROFILER_LABEL("PluginModuleParent::NPP_NewStream", OTHER);

    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->NPP_NewStream(type, stream, seekable, stype)
               : NPERR_GENERIC_ERROR;
}

NPError
PluginModuleParent::NPP_SetWindow(NPP instance, NPWindow* window)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->NPP_SetWindow(window) : NPERR_GENERIC_ERROR;
}

NPError
PluginModuleParent::NPP_DestroyStream(NPP instance,
                                      NPStream* stream,
                                      NPReason reason)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->NPP_DestroyStream(stream, reason) : NPERR_GENERIC_ERROR;
}

int32_t
PluginModuleParent::NPP_WriteReady(NPP instance,
                                   NPStream* stream)
{
    BrowserStreamParent* s = StreamCast(instance, stream);
    return s ? s->WriteReady() : -1;
}

int32_t
PluginModuleParent::NPP_Write(NPP instance,
                              NPStream* stream,
                              int32_t offset,
                              int32_t len,
                              void* buffer)
{
    BrowserStreamParent* s = StreamCast(instance, stream);
    if (!s)
        return -1;

    return s->Write(offset, len, buffer);
}

void
PluginModuleParent::NPP_StreamAsFile(NPP instance,
                                     NPStream* stream,
                                     const char* fname)
{
    BrowserStreamParent* s = StreamCast(instance, stream);
    if (!s)
        return;

    s->StreamAsFile(fname);
}

void
PluginModuleParent::NPP_Print(NPP instance, NPPrint* platformPrint)
{

    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->NPP_Print(platformPrint) : (void)0;
}

int16_t
PluginModuleParent::NPP_HandleEvent(NPP instance, void* event)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->NPP_HandleEvent(event) : NPERR_GENERIC_ERROR;
}

void
PluginModuleParent::NPP_URLNotify(NPP instance, const char* url,
                                  NPReason reason, void* notifyData)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->NPP_URLNotify(url, reason, notifyData) : (void)0;
}

NPError
PluginModuleParent::NPP_GetValue(NPP instance,
                                 NPPVariable variable, void *ret_value)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->NPP_GetValue(variable, ret_value) : NPERR_GENERIC_ERROR;
}

NPError
PluginModuleParent::NPP_SetValue(NPP instance, NPNVariable variable,
                                 void *value)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->NPP_SetValue(variable, value) : NPERR_GENERIC_ERROR;
}

mozilla::ipc::IPCResult
PluginModuleChromeParent::AnswerNPN_SetValue_NPPVpluginRequiresAudioDeviceChanges(
    const bool& shouldRegister, NPError* result)
{
    NS_RUNTIMEABORT("NPPVpluginRequiresAudioDeviceChanges is not valid on this platform.");
    *result = NPERR_GENERIC_ERROR;
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvBackUpXResources(const FileDescriptor& aXSocketFd)
{
#ifndef MOZ_X11
    NS_RUNTIMEABORT("This message only makes sense on X11 platforms");
#else
    MOZ_ASSERT(0 > mPluginXSocketFdDup.get(),
               "Already backed up X resources??");
    if (aXSocketFd.IsValid()) {
      auto rawFD = aXSocketFd.ClonePlatformHandle();
      mPluginXSocketFdDup.reset(rawFD.release());
    }
#endif
    return IPC_OK();
}

void
PluginModuleParent::NPP_URLRedirectNotify(NPP instance, const char* url,
                                          int32_t status, void* notifyData)
{
  PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
  return pip ? pip->NPP_URLRedirectNotify(url, status, notifyData) : (void)0;
}

BrowserStreamParent*
PluginModuleParent::StreamCast(NPP instance, NPStream* s)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    if (!pip) {
        return nullptr;
    }

    BrowserStreamParent* sp =
        static_cast<BrowserStreamParent*>(static_cast<AStream*>(s->pdata));
    if (sp && (sp->mNPP != pip || s != sp->mStream)) {
        MOZ_CRASH("Corrupted plugin stream data.");
    }
    return sp;
}

bool
PluginModuleParent::HasRequiredFunctions()
{
    return true;
}

nsresult
PluginModuleParent::AsyncSetWindow(NPP instance, NPWindow* window)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->AsyncSetWindow(window) : NS_ERROR_FAILURE;
}

nsresult
PluginModuleParent::GetImageContainer(NPP instance,
                             mozilla::layers::ImageContainer** aContainer)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->GetImageContainer(aContainer) : NS_ERROR_FAILURE;
}

nsresult
PluginModuleParent::GetImageSize(NPP instance,
                                 nsIntSize* aSize)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->GetImageSize(aSize) : NS_ERROR_FAILURE;
}

void
PluginModuleParent::DidComposite(NPP aInstance)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(aInstance);
    return pip ? pip->DidComposite() : (void)0;
}

nsresult
PluginModuleParent::SetBackgroundUnknown(NPP instance)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->SetBackgroundUnknown() : NS_ERROR_FAILURE;
}

nsresult
PluginModuleParent::BeginUpdateBackground(NPP instance,
                                          const nsIntRect& aRect,
                                          DrawTarget** aDrawTarget)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->BeginUpdateBackground(aRect, aDrawTarget)
               : NS_ERROR_FAILURE;
}

nsresult
PluginModuleParent::EndUpdateBackground(NPP instance, const nsIntRect& aRect)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(instance);
    return pip ? pip->EndUpdateBackground(aRect) : NS_ERROR_FAILURE;
}

nsresult
PluginModuleParent::HandledWindowedPluginKeyEvent(
                        NPP aInstance,
                        const NativeEventData& aNativeKeyData,
                        bool aIsConsumed)
{
    PluginInstanceParent* pip = PluginInstanceParent::Cast(aInstance);
    return pip ? pip->HandledWindowedPluginKeyEvent(aNativeKeyData, aIsConsumed)
               : NS_ERROR_FAILURE;
}

void
PluginModuleParent::OnInitFailure()
{
    if (GetIPCChannel()->CanSend()) {
        Close();
    }

    mShutdown = true;
}

class PluginOfflineObserver final : public nsIObserver
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIOBSERVER

    explicit PluginOfflineObserver(PluginModuleChromeParent* pmp)
      : mPmp(pmp)
    {}

private:
    ~PluginOfflineObserver() {}
    PluginModuleChromeParent* mPmp;
};

NS_IMPL_ISUPPORTS(PluginOfflineObserver, nsIObserver)

NS_IMETHODIMP
PluginOfflineObserver::Observe(nsISupports *aSubject,
                               const char *aTopic,
                               const char16_t *aData)
{
    MOZ_ASSERT(!strcmp(aTopic, "ipc:network:set-offline"));
    mPmp->CachedSettingChanged();
    return NS_OK;
}

static const char* kSettingsPrefs[] =
    {"javascript.enabled",
     "dom.ipc.plugins.nativeCursorSupport"};

void
PluginModuleChromeParent::RegisterSettingsCallbacks()
{
    for (size_t i = 0; i < ArrayLength(kSettingsPrefs); i++) {
        Preferences::RegisterCallback(CachedSettingChanged, kSettingsPrefs[i], this);
    }

    nsCOMPtr<nsIObserverService> observerService = mozilla::services::GetObserverService();
    if (observerService) {
        mPluginOfflineObserver = new PluginOfflineObserver(this);
        observerService->AddObserver(mPluginOfflineObserver, "ipc:network:set-offline", false);
    }
}

void
PluginModuleChromeParent::UnregisterSettingsCallbacks()
{
    for (size_t i = 0; i < ArrayLength(kSettingsPrefs); i++) {
        Preferences::UnregisterCallback(CachedSettingChanged, kSettingsPrefs[i], this);
    }

    nsCOMPtr<nsIObserverService> observerService = mozilla::services::GetObserverService();
    if (observerService) {
        observerService->RemoveObserver(mPluginOfflineObserver, "ipc:network:set-offline");
        mPluginOfflineObserver = nullptr;
    }
}

bool
PluginModuleParent::GetSetting(NPNVariable aVariable)
{
    NPBool boolVal = false;
    mozilla::plugins::parent::_getvalue(nullptr, aVariable, &boolVal);
    return boolVal;
}

void
PluginModuleParent::GetSettings(PluginSettings* aSettings)
{
    aSettings->javascriptEnabled() = GetSetting(NPNVjavascriptEnabledBool);
    aSettings->asdEnabled() = GetSetting(NPNVasdEnabledBool);
    aSettings->isOffline() = GetSetting(NPNVisOfflineBool);
    aSettings->supportsXembed() = GetSetting(NPNVSupportsXEmbedBool);
    aSettings->supportsWindowless() = GetSetting(NPNVSupportsWindowless);
    aSettings->userAgent() = NullableString(mNPNIface->uagent(nullptr));

    // Need to initialize this to satisfy IPDL.
    aSettings->nativeCursorsSupported() = false;
}

void
PluginModuleChromeParent::CachedSettingChanged()
{
    PluginSettings settings;
    GetSettings(&settings);
    Unused << SendSettingChanged(settings);
}

/* static */ void
PluginModuleChromeParent::CachedSettingChanged(const char* aPref, void* aModule)
{
    PluginModuleChromeParent *module = static_cast<PluginModuleChromeParent*>(aModule);
    module->CachedSettingChanged();
}

#if defined(XP_UNIX)
nsresult
PluginModuleParent::NP_Initialize(NPNetscapeFuncs* bFuncs, NPPluginFuncs* pFuncs, NPError* error)
{
    PLUGIN_LOG_DEBUG_METHOD;

    mNPNIface = bFuncs;
    mNPPIface = pFuncs;

    if (mShutdown) {
        *error = NPERR_GENERIC_ERROR;
        return NS_ERROR_FAILURE;
    }

    *error = NPERR_NO_ERROR;
    SetPluginFuncs(pFuncs);

    return NS_OK;
}

nsresult
PluginModuleChromeParent::NP_Initialize(NPNetscapeFuncs* bFuncs, NPPluginFuncs* pFuncs, NPError* error)
{
    PLUGIN_LOG_DEBUG_METHOD;

    if (mShutdown) {
        *error = NPERR_GENERIC_ERROR;
        return NS_ERROR_FAILURE;
    }

    *error = NPERR_NO_ERROR;

    mNPNIface = bFuncs;
    mNPPIface = pFuncs;

    PluginSettings settings;
    GetSettings(&settings);

    TimeStamp callNpInitStart = TimeStamp::Now();
    if (!CallNP_Initialize(settings, error)) {
        Close();
        return NS_ERROR_FAILURE;
    }
    else if (*error != NPERR_NO_ERROR) {
        Close();
        return NS_ERROR_FAILURE;
    }
    TimeStamp callNpInitEnd = TimeStamp::Now();
    mTimeBlocked += (callNpInitEnd - callNpInitStart);

    if (*error != NPERR_NO_ERROR) {
        OnInitFailure();
        return NS_OK;
    }

    SetPluginFuncs(mNPPIface);

    return NS_OK;
}

#else

nsresult
PluginModuleParent::NP_Initialize(NPNetscapeFuncs* bFuncs, NPError* error)
{
    PLUGIN_LOG_DEBUG_METHOD;

    mNPNIface = bFuncs;

    if (mShutdown) {
        *error = NPERR_GENERIC_ERROR;
        return NS_ERROR_FAILURE;
    }

    *error = NPERR_NO_ERROR;
    return NS_OK;
}

nsresult
PluginModuleChromeParent::NP_Initialize(NPNetscapeFuncs* bFuncs, NPError* error)
{
    nsresult rv = PluginModuleParent::NP_Initialize(bFuncs, error);
    if (NS_FAILED(rv))
        return rv;

    PluginSettings settings;
    GetSettings(&settings);

    TimeStamp callNpInitStart = TimeStamp::Now();
    if (!CallNP_Initialize(settings, error)) {
        Close();
        return NS_ERROR_FAILURE;
    }
    TimeStamp callNpInitEnd = TimeStamp::Now();
    mTimeBlocked += (callNpInitEnd - callNpInitStart);

    bool ok = true;
    if (*error == NPERR_NO_ERROR) {
        // Initialization steps for (e10s && !asyncInit) || !e10s
    }

    if (!ok) {
        return NS_ERROR_FAILURE;
    }

    if (*error != NPERR_NO_ERROR) {
        OnInitFailure();
        return NS_OK;
    }

    return NS_OK;
}

#endif

nsresult
PluginModuleParent::NP_Shutdown(NPError* error)
{
    PLUGIN_LOG_DEBUG_METHOD;

    if (mShutdown) {
        *error = NPERR_GENERIC_ERROR;
        return NS_ERROR_FAILURE;
    }

    if (!DoShutdown(error)) {
        return NS_ERROR_FAILURE;
    }

    return NS_OK;
}

bool
PluginModuleParent::DoShutdown(NPError* error)
{
    bool ok = true;
    if (IsChrome() && mHadLocalInstance) {
        // We synchronously call NP_Shutdown if the chrome process was using
        // plugins itself. That way we can service any requests the plugin
        // makes. If we're in e10s, though, the content processes will have
        // already shut down and there's no one to talk to. So we shut down
        // asynchronously in PluginModuleChild::ActorDestroy.
        ok = CallNP_Shutdown(error);
    }

    // if NP_Shutdown() is nested within another interrupt call, this will
    // break things.  but lord help us if we're doing that anyway; the
    // plugin dso will have been unloaded on the other side by the
    // CallNP_Shutdown() message
    Close();

    // mShutdown should either be initialized to false, or be transitiong from
    // false to true. It is never ok to go from true to false. Using OR for
    // the following assignment to ensure this.
    mShutdown |= ok;
    if (!ok) {
        *error = NPERR_GENERIC_ERROR;
    }
    return ok;
}

nsresult
PluginModuleParent::NP_GetMIMEDescription(const char** mimeDesc)
{
    PLUGIN_LOG_DEBUG_METHOD;

    *mimeDesc = "application/x-foobar";
    return NS_OK;
}

nsresult
PluginModuleParent::NP_GetValue(void *future, NPPVariable aVariable,
                                   void *aValue, NPError* error)
{
    MOZ_LOG(GetPluginLog(), LogLevel::Warning, ("%s Not implemented, requested variable %i", __FUNCTION__,
                                        (int) aVariable));

    //TODO: implement this correctly
    *error = NPERR_GENERIC_ERROR;
    return NS_OK;
}

nsresult
PluginModuleParent::NPP_New(NPMIMEType pluginType, NPP instance,
                            int16_t argc, char* argn[],
                            char* argv[], NPSavedData* saved,
                            NPError* error)
{
    PLUGIN_LOG_DEBUG_METHOD;

    if (mShutdown) {
        *error = NPERR_GENERIC_ERROR;
        return NS_ERROR_FAILURE;
    }

    // create the instance on the other side
    InfallibleTArray<nsCString> names;
    InfallibleTArray<nsCString> values;

    for (int i = 0; i < argc; ++i) {
        names.AppendElement(NullableString(argn[i]));
        values.AppendElement(NullableString(argv[i]));
    }

    return NPP_NewInternal(pluginType, instance, names, values, saved, error);
}

class nsCaseInsensitiveUTF8StringArrayComparator
{
public:
  template<class A, class B>
  bool Equals(const A& a, const B& b) const {
    return a.Equals(b.get(), nsCaseInsensitiveUTF8StringComparator());
  }
};

void
PluginModuleParent::AccumulateModuleInitBlockedTime()
{
    if (mPluginName.IsEmpty()) {
        GetPluginDetails();
    }
    Telemetry::Accumulate(Telemetry::BLOCKED_ON_PLUGIN_MODULE_INIT_MS,
                          GetHistogramKey(),
                          static_cast<uint32_t>(mTimeBlocked.ToMilliseconds()));
    mTimeBlocked = TimeDuration();
}

nsresult
PluginModuleParent::NPP_NewInternal(NPMIMEType pluginType, NPP instance,
                                    InfallibleTArray<nsCString>& names,
                                    InfallibleTArray<nsCString>& values,
                                    NPSavedData* saved, NPError* error)
{
    MOZ_ASSERT(names.Length() == values.Length());
    if (mPluginName.IsEmpty()) {
        GetPluginDetails();
        InitQuirksModes(nsDependentCString(pluginType));
        /** mTimeBlocked measures the time that the main thread has been blocked
         *  on plugin module initialization. As implemented, this is the sum of
         *  plugin-container launch + toolhelp32 snapshot + NP_Initialize.
         *  We don't accumulate its value until here because the plugin info
         *  for its histogram key is not available until *after* NP_Initialize.
         */
        AccumulateModuleInitBlockedTime();
    }

    nsCaseInsensitiveUTF8StringArrayComparator comparator;
    NS_NAMED_LITERAL_CSTRING(srcAttributeName, "src");
    auto srcAttributeIndex = names.IndexOf(srcAttributeName, 0, comparator);
    nsAutoCString srcAttribute;
    if (srcAttributeIndex != names.NoIndex) {
        srcAttribute = values[srcAttributeIndex];
    }

    nsDependentCString strPluginType(pluginType);
    PluginInstanceParent* parentInstance =
        new PluginInstanceParent(this, instance, strPluginType, mNPNIface);

    if (mIsFlashPlugin) {
        parentInstance->InitMetadata(strPluginType, srcAttribute);
    }

    instance->pdata = parentInstance;

    // Any IPC messages for the PluginInstance actor should be dispatched to the
    // DocGroup for the plugin's document.
    RefPtr<nsPluginInstanceOwner> owner = parentInstance->GetOwner();
    nsCOMPtr<nsIDOMElement> elt;
    owner->GetDOMElement(getter_AddRefs(elt));
    if (nsCOMPtr<nsINode> node = do_QueryInterface(elt)) {
        nsCOMPtr<nsIDocument> doc = node->OwnerDoc();
        if (doc) {
            nsCOMPtr<nsIEventTarget> eventTarget = doc->EventTargetFor(TaskCategory::Other);
            SetEventTargetForActor(parentInstance, eventTarget);
        }
    }

    if (!SendPPluginInstanceConstructor(parentInstance,
                                        nsDependentCString(pluginType),
                                        names, values)) {
        // |parentInstance| is automatically deleted.
        instance->pdata = nullptr;
        *error = NPERR_GENERIC_ERROR;
        return NS_ERROR_FAILURE;
    }

    {   // Scope for timer
        Telemetry::AutoTimer<Telemetry::BLOCKED_ON_PLUGIN_INSTANCE_INIT_MS>
            timer(GetHistogramKey());
        if (!CallSyncNPP_New(parentInstance, error)) {
            // if IPC is down, we'll get an immediate "failed" return, but
            // without *error being set.  So make sure that the error
            // condition is signaled to nsNPAPIPluginInstance
            if (NPERR_NO_ERROR == *error) {
                *error = NPERR_GENERIC_ERROR;
            }
            return NS_ERROR_FAILURE;
        }
    }

    if (*error != NPERR_NO_ERROR) {
        NPP_Destroy(instance, 0);
        return NS_ERROR_FAILURE;
    }

    UpdatePluginTimeout();

    return NS_OK;
}

void
PluginModuleChromeParent::UpdatePluginTimeout()
{
    TimeoutChanged(kParentTimeoutPref, this);
}

nsresult
PluginModuleParent::NPP_ClearSiteData(const char* site, uint64_t flags, uint64_t maxAge,
                                      nsCOMPtr<nsIClearSiteDataCallback> callback)
{
    if (!mClearSiteDataSupported)
        return NS_ERROR_NOT_AVAILABLE;

    static uint64_t callbackId = 0;
    callbackId++;
    mClearSiteDataCallbacks[callbackId] = callback;

    if (!SendNPP_ClearSiteData(NullableString(site), flags, maxAge, callbackId)) {
        return NS_ERROR_FAILURE;
    }
    return NS_OK;
}


nsresult
PluginModuleParent::NPP_GetSitesWithData(nsCOMPtr<nsIGetSitesWithDataCallback> callback)
{
    if (!mGetSitesWithDataSupported)
        return NS_ERROR_NOT_AVAILABLE;

    static uint64_t callbackId = 0;
    callbackId++;
    mSitesWithDataCallbacks[callbackId] = callback;

    if (!SendNPP_GetSitesWithData(callbackId))
        return NS_ERROR_FAILURE;

    return NS_OK;
}

#if !defined(MOZ_WIDGET_GTK)
mozilla::ipc::IPCResult
PluginModuleParent::AnswerProcessSomeEvents()
{
    NS_RUNTIMEABORT("unreached");
    return IPC_FAIL_NO_REASON(this);
}

#else
static const int kMaxChancesToProcessEvents = 20;

mozilla::ipc::IPCResult
PluginModuleParent::AnswerProcessSomeEvents()
{
    PLUGIN_LOG_DEBUG(("Spinning mini nested loop ..."));

    int i = 0;
    for (; i < kMaxChancesToProcessEvents; ++i)
        if (!g_main_context_iteration(nullptr, FALSE))
            break;

    PLUGIN_LOG_DEBUG(("... quitting mini nested loop; processed %i tasks", i));

    return IPC_OK();
}
#endif

mozilla::ipc::IPCResult
PluginModuleParent::RecvProcessNativeEventsInInterruptCall()
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));
#if defined(OS_WIN)
    ProcessNativeEventsInInterruptCall();
    return IPC_OK();
#else
    NS_NOTREACHED(
        "PluginModuleParent::RecvProcessNativeEventsInInterruptCall not implemented!");
    return IPC_FAIL_NO_REASON(this);
#endif
}

void
PluginModuleParent::ProcessRemoteNativeEventsInInterruptCall()
{
#if defined(OS_WIN)
    Unused << SendProcessNativeEventsInInterruptCall();
    return;
#endif
    NS_NOTREACHED(
        "PluginModuleParent::ProcessRemoteNativeEventsInInterruptCall not implemented!");
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvPluginShowWindow(const uint32_t& aWindowId, const bool& aModal,
                                         const int32_t& aX, const int32_t& aY,
                                         const size_t& aWidth, const size_t& aHeight)
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));
    NS_NOTREACHED(
        "PluginInstanceParent::RecvPluginShowWindow not implemented!");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvPluginHideWindow(const uint32_t& aWindowId)
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));
    NS_NOTREACHED(
        "PluginInstanceParent::RecvPluginHideWindow not implemented!");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvSetCursor(const NSCursorInfo& aCursorInfo)
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));
    NS_NOTREACHED(
        "PluginInstanceParent::RecvSetCursor not implemented!");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvShowCursor(const bool& aShow)
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));
    NS_NOTREACHED(
        "PluginInstanceParent::RecvShowCursor not implemented!");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvPushCursor(const NSCursorInfo& aCursorInfo)
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));
    NS_NOTREACHED(
        "PluginInstanceParent::RecvPushCursor not implemented!");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvPopCursor()
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));
    NS_NOTREACHED(
        "PluginInstanceParent::RecvPopCursor not implemented!");
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvNPN_SetException(const nsCString& aMessage)
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));

    // This function ignores its first argument.
    mozilla::plugins::parent::_setexception(nullptr, NullableStringGet(aMessage));
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvNPN_ReloadPlugins(const bool& aReloadPages)
{
    PLUGIN_LOG_DEBUG(("%s", FULLFUNCTION));

    mozilla::plugins::parent::_reloadplugins(aReloadPages);
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginModuleChromeParent::RecvNotifyContentModuleDestroyed()
{
    RefPtr<nsPluginHost> host = nsPluginHost::GetInst();
    if (host) {
        host->NotifyContentModuleDestroyed(mPluginId);
    }
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvReturnClearSiteData(const NPError& aRv,
                                            const uint64_t& aCallbackId)
{
    if (mClearSiteDataCallbacks.find(aCallbackId) == mClearSiteDataCallbacks.end()) {
        return IPC_OK();
    }
    if (!!mClearSiteDataCallbacks[aCallbackId]) {
        nsresult rv;
        switch (aRv) {
        case NPERR_NO_ERROR:
            rv = NS_OK;
            break;
        case NPERR_TIME_RANGE_NOT_SUPPORTED:
            rv = NS_ERROR_PLUGIN_TIME_RANGE_NOT_SUPPORTED;
            break;
        case NPERR_MALFORMED_SITE:
            rv = NS_ERROR_INVALID_ARG;
            break;
        default:
            rv = NS_ERROR_FAILURE;
        }
        mClearSiteDataCallbacks[aCallbackId]->Callback(rv);
    }
    mClearSiteDataCallbacks.erase(aCallbackId);
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginModuleParent::RecvReturnSitesWithData(nsTArray<nsCString>&& aSites,
                                            const uint64_t& aCallbackId)
{
    if (mSitesWithDataCallbacks.find(aCallbackId) == mSitesWithDataCallbacks.end()) {
        return IPC_OK();
    }

    if (!!mSitesWithDataCallbacks[aCallbackId]) {
        mSitesWithDataCallbacks[aCallbackId]->SitesWithData(aSites);
    }
    mSitesWithDataCallbacks.erase(aCallbackId);
    return IPC_OK();
}

layers::TextureClientRecycleAllocator*
PluginModuleParent::EnsureTextureAllocatorForDirectBitmap()
{
    if (!mTextureAllocatorForDirectBitmap) {
        mTextureAllocatorForDirectBitmap = new TextureClientRecycleAllocator(ImageBridgeChild::GetSingleton().get());
    }
    return mTextureAllocatorForDirectBitmap;
}

layers::TextureClientRecycleAllocator*
PluginModuleParent::EnsureTextureAllocatorForDXGISurface()
{
    if (!mTextureAllocatorForDXGISurface) {
        mTextureAllocatorForDXGISurface = new TextureClientRecycleAllocator(ImageBridgeChild::GetSingleton().get());
    }
    return mTextureAllocatorForDXGISurface;
}


mozilla::ipc::IPCResult
PluginModuleParent::AnswerNPN_SetValue_NPPVpluginRequiresAudioDeviceChanges(
                                        const bool& shouldRegister,
                                        NPError* result) {
    NS_RUNTIMEABORT("SetValue_NPPVpluginRequiresAudioDeviceChanges is only valid "
      "with PluginModuleChromeParent");
    *result = NPERR_GENERIC_ERROR;
    return IPC_OK();
}

mozilla::ipc::IPCResult
PluginModuleParent::AnswerGetKeyState(const int32_t& aVirtKey, int16_t* aRet)
{
    return IPC_FAIL_NO_REASON(this);
}

mozilla::ipc::IPCResult
PluginModuleChromeParent::AnswerGetKeyState(const int32_t& aVirtKey,
                                            int16_t* aRet)
{
    return PluginModuleParent::AnswerGetKeyState(aVirtKey, aRet);
}

mozilla::ipc::IPCResult
PluginModuleChromeParent::AnswerGetFileName(const GetFileNameFunc& aFunc,
                                            const OpenFileNameIPC& aOfnIn,
                                            OpenFileNameRetIPC* aOfnOut,
                                            bool* aResult)
{
    MOZ_ASSERT_UNREACHABLE("GetFileName IPC message is only available on "
                           "Windows builds with sandbox.");
    return IPC_FAIL_NO_REASON(this);
}

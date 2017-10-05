/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GeckoChildProcessHost.h"

#include "base/command_line.h"
#include "base/string_util.h"
#include "base/task.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/process_watcher.h"

#include "MainThreadUtils.h"
#include "mozilla/Sprintf.h"
#include "prenv.h"
#include "nsXPCOMPrivate.h"

#if defined(MOZ_CONTENT_SANDBOX)
#include "mozilla/SandboxSettings.h"
#if defined(XP_MACOSX)
#include "nsAppDirectoryServiceDefs.h"
#endif
#endif

#include "nsDirectoryServiceDefs.h"
#include "nsIFile.h"
#include "nsPrintfCString.h"

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/ipc/BrowserProcessSubThread.h"
#include "mozilla/Omnijar.h"
#include "mozilla/Telemetry.h"
#include "ProtocolUtils.h"
#include <sys/stat.h>

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
#include "mozilla/SandboxReporter.h"
#endif

#include "nsTArray.h"
#include "nsClassHashtable.h"
#include "nsHashKeys.h"
#include "nsNativeCharsetUtils.h"
#include "nscore.h" // for NS_FREE_PERMANENT_DATA

using mozilla::MonitorAutoLock;
using mozilla::ipc::GeckoChildProcessHost;

#ifdef ANDROID
// Like its predecessor in nsExceptionHandler.cpp, this is
// the magic number of a file descriptor remapping we must
// preserve for the child process.
static const int kMagicAndroidSystemPropFd = 5;
#endif

static bool
ShouldHaveDirectoryService()
{
  return GeckoProcessType_Default == XRE_GetProcessType();
}

GeckoChildProcessHost::GeckoChildProcessHost(GeckoProcessType aProcessType,
                                             bool aIsFileContent)
  : mProcessType(aProcessType),
    mIsFileContent(aIsFileContent),
    mMonitor("mozilla.ipc.GeckChildProcessHost.mMonitor"),
    mProcessState(CREATING_CHANNEL),
    mChildProcessHandle(0)
{
    MOZ_COUNT_CTOR(GeckoChildProcessHost);
}

GeckoChildProcessHost::~GeckoChildProcessHost()

{
  AssertIOThread();

  MOZ_COUNT_DTOR(GeckoChildProcessHost);

  if (mChildProcessHandle != 0) {
    ProcessWatcher::EnsureProcessTerminated(mChildProcessHandle
#ifdef NS_FREE_PERMANENT_DATA
    // If we're doing leak logging, shutdown can be slow.
                                            , false // don't "force"
#endif
    );
  }
}

//static
auto
GeckoChildProcessHost::GetPathToBinary(FilePath& exePath, GeckoProcessType processType) -> BinaryPathType
{
  if (sRunSelfAsContentProc &&
      (processType == GeckoProcessType_Content || processType == GeckoProcessType_GPU)) {
#if defined(OS_POSIX)
    exePath = FilePath(CommandLine::ForCurrentProcess()->argv()[0]);
#else
#  error Sorry; target OS not supported yet.
#endif
    return BinaryPathType::Self;
  }

  if (ShouldHaveDirectoryService()) {
    MOZ_ASSERT(gGREBinPath);
    nsCString path;
    NS_CopyUnicodeToNative(nsDependentString(gGREBinPath), path);
    exePath = FilePath(path.get());
  }

  if (exePath.empty()) {
    exePath = FilePath(CommandLine::ForCurrentProcess()->argv()[0]);
    exePath = exePath.DirName();
  }

  exePath = exePath.AppendASCII(MOZ_CHILD_PROCESS_NAME);

  return BinaryPathType::PluginContainer;
}

// We start the unique IDs at 1 so that 0 can be used to mean that
// a component has no unique ID assigned to it.
uint32_t GeckoChildProcessHost::sNextUniqueID = 1;

/* static */
uint32_t
GeckoChildProcessHost::GetUniqueID()
{
  return sNextUniqueID++;
}

void
GeckoChildProcessHost::PrepareLaunch()
{
}

bool
GeckoChildProcessHost::SyncLaunch(std::vector<std::string> aExtraOpts, int aTimeoutMs)
{
  PrepareLaunch();

  MessageLoop* ioLoop = XRE_GetIOMessageLoop();
  NS_ASSERTION(MessageLoop::current() != ioLoop, "sync launch from the IO thread NYI");

  ioLoop->PostTask(NewNonOwningRunnableMethod<std::vector<std::string>>(
    "ipc::GeckoChildProcessHost::RunPerformAsyncLaunch",
    this,
    &GeckoChildProcessHost::RunPerformAsyncLaunch,
    aExtraOpts));

  return WaitUntilConnected(aTimeoutMs);
}

bool
GeckoChildProcessHost::AsyncLaunch(std::vector<std::string> aExtraOpts)
{
  PrepareLaunch();

  MessageLoop* ioLoop = XRE_GetIOMessageLoop();

  ioLoop->PostTask(NewNonOwningRunnableMethod<std::vector<std::string>>(
    "ipc::GeckoChildProcessHost::RunPerformAsyncLaunch",
    this,
    &GeckoChildProcessHost::RunPerformAsyncLaunch,
    aExtraOpts));

  // This may look like the sync launch wait, but we only delay as
  // long as it takes to create the channel.
  MonitorAutoLock lock(mMonitor);
  while (mProcessState < CHANNEL_INITIALIZED) {
    lock.Wait();
  }

  return true;
}

bool
GeckoChildProcessHost::WaitUntilConnected(int32_t aTimeoutMs)
{
  PROFILER_LABEL_FUNC(js::ProfileEntry::Category::OTHER);

  // NB: this uses a different mechanism than the chromium parent
  // class.
  PRIntervalTime timeoutTicks = (aTimeoutMs > 0) ?
    PR_MillisecondsToInterval(aTimeoutMs) : PR_INTERVAL_NO_TIMEOUT;

  MonitorAutoLock lock(mMonitor);
  PRIntervalTime waitStart = PR_IntervalNow();
  PRIntervalTime current;

  // We'll receive several notifications, we need to exit when we
  // have either successfully launched or have timed out.
  while (mProcessState != PROCESS_CONNECTED) {
    // If there was an error then return it, don't wait out the timeout.
    if (mProcessState == PROCESS_ERROR) {
      break;
    }

    lock.Wait(timeoutTicks);

    if (timeoutTicks != PR_INTERVAL_NO_TIMEOUT) {
      current = PR_IntervalNow();
      PRIntervalTime elapsed = current - waitStart;
      if (elapsed > timeoutTicks) {
        break;
      }
      timeoutTicks = timeoutTicks - elapsed;
      waitStart = current;
    }
  }

  return mProcessState == PROCESS_CONNECTED;
}

bool
GeckoChildProcessHost::LaunchAndWaitForProcessHandle(StringVector aExtraOpts)
{
  PrepareLaunch();

  MessageLoop* ioLoop = XRE_GetIOMessageLoop();
  ioLoop->PostTask(NewNonOwningRunnableMethod<std::vector<std::string>>(
    "ipc::GeckoChildProcessHost::RunPerformAsyncLaunch",
    this,
    &GeckoChildProcessHost::RunPerformAsyncLaunch,
    aExtraOpts));

  MonitorAutoLock lock(mMonitor);
  while (mProcessState < PROCESS_CREATED) {
    lock.Wait();
  }
  MOZ_ASSERT(mProcessState == PROCESS_ERROR || mChildProcessHandle);

  return mProcessState < PROCESS_ERROR;
}

void
GeckoChildProcessHost::InitializeChannel()
{
  CreateChannel();

  MonitorAutoLock lock(mMonitor);
  mProcessState = CHANNEL_INITIALIZED;
  lock.Notify();
}

void
GeckoChildProcessHost::Join()
{
  AssertIOThread();

  if (!mChildProcessHandle) {
    return;
  }

  // If this fails, there's nothing we can do.
  base::KillProcess(mChildProcessHandle, 0, /*wait*/true);
  SetAlreadyDead();
}

void
GeckoChildProcessHost::SetAlreadyDead()
{
  if (mChildProcessHandle &&
      mChildProcessHandle != kInvalidProcessHandle) {
    base::CloseProcessHandle(mChildProcessHandle);
  }

  mChildProcessHandle = 0;
}

int32_t GeckoChildProcessHost::mChildCounter = 0;

void
GeckoChildProcessHost::SetChildLogName(const char* varName, const char* origLogName,
                                       nsACString &buffer)
{
  // We currently have no portable way to launch child with environment
  // different than parent.  So temporarily change NSPR_LOG_FILE so child
  // inherits value we want it to have. (NSPR only looks at NSPR_LOG_FILE at
  // startup, so it's 'safe' to play with the parent's environment this way.)
  buffer.Assign(varName);

  {
    buffer.Append(origLogName);
  }

  // Append child-specific postfix to name
  buffer.AppendLiteral(".child-");
  buffer.AppendInt(mChildCounter);

  // Passing temporary to PR_SetEnv is ok here if we keep the temporary
  // for the time we launch the sub-process.  It's copied to the new
  // environment.
  PR_SetEnv(buffer.BeginReading());
}

bool
GeckoChildProcessHost::PerformAsyncLaunch(std::vector<std::string> aExtraOpts)
{
#ifdef MOZ_GECKO_PROFILER
  AutoSetProfilerEnvVarsForChildProcess profilerEnvironment;
#endif

  const char* origNSPRLogName = PR_GetEnv("NSPR_LOG_FILE");
  const char* origMozLogName = PR_GetEnv("MOZ_LOG_FILE");

  // - Note: this code is not called re-entrantly, nor are restoreOrig*LogName
  //   or mChildCounter touched by any other thread, so this is safe.
  ++mChildCounter;

  // Must keep these on the same stack where from we call PerformAsyncLaunchInternal
  // so that PR_DuplicateEnvironment() still sees a valid memory.
  nsAutoCString nsprLogName;
  nsAutoCString mozLogName;

  if (origNSPRLogName) {
    if (mRestoreOrigNSPRLogName.IsEmpty()) {
      mRestoreOrigNSPRLogName.AssignLiteral("NSPR_LOG_FILE=");
      mRestoreOrigNSPRLogName.Append(origNSPRLogName);
    }
    SetChildLogName("NSPR_LOG_FILE=", origNSPRLogName, nsprLogName);
  }
  if (origMozLogName) {
    if (mRestoreOrigMozLogName.IsEmpty()) {
      mRestoreOrigMozLogName.AssignLiteral("MOZ_LOG_FILE=");
      mRestoreOrigMozLogName.Append(origMozLogName);
    }
    SetChildLogName("MOZ_LOG_FILE=", origMozLogName, mozLogName);
  }

  bool retval = PerformAsyncLaunchInternal(aExtraOpts);

  // Revert to original value
  if (origNSPRLogName) {
    PR_SetEnv(mRestoreOrigNSPRLogName.get());
  }
  if (origMozLogName) {
    PR_SetEnv(mRestoreOrigMozLogName.get());
  }

  return retval;
}

bool
GeckoChildProcessHost::RunPerformAsyncLaunch(std::vector<std::string> aExtraOpts)
{
  InitializeChannel();

  bool ok = PerformAsyncLaunch(aExtraOpts);
  if (!ok) {
    // WaitUntilConnected might be waiting for us to signal.
    // If something failed let's set the error state and notify.
    MonitorAutoLock lock(mMonitor);
    mProcessState = PROCESS_ERROR;
    lock.Notify();
    CHROMIUM_LOG(ERROR) << "Failed to launch " <<
      XRE_ChildProcessTypeToString(mProcessType) << " subprocess";
    Telemetry::Accumulate(Telemetry::SUBPROCESS_LAUNCH_FAILURE,
      nsDependentCString(XRE_ChildProcessTypeToString(mProcessType)));
  }
  return ok;
}

void
AddAppDirToCommandLine(std::vector<std::string>& aCmdLine)
{
  // Content processes need access to application resources, so pass
  // the full application directory path to the child process.
  if (ShouldHaveDirectoryService()) {
    nsCOMPtr<nsIProperties> directoryService(do_GetService(NS_DIRECTORY_SERVICE_CONTRACTID));
    NS_ASSERTION(directoryService, "Expected XPCOM to be available");
    if (directoryService) {
      nsCOMPtr<nsIFile> appDir;
      // NS_XPCOM_CURRENT_PROCESS_DIR really means the app dir, not the
      // current process dir.
      nsresult rv = directoryService->Get(NS_XPCOM_CURRENT_PROCESS_DIR,
                                          NS_GET_IID(nsIFile),
                                          getter_AddRefs(appDir));
      if (NS_SUCCEEDED(rv)) {
        nsAutoCString path;
        MOZ_ALWAYS_SUCCEEDS(appDir->GetNativePath(path));
        aCmdLine.push_back("-appdir");
        aCmdLine.push_back(path.get());
      }

#if defined(XP_MACOSX) && defined(MOZ_CONTENT_SANDBOX)
      // Full path to the profile dir
      nsCOMPtr<nsIFile> profileDir;
      rv = directoryService->Get(NS_APP_USER_PROFILE_50_DIR,
                                 NS_GET_IID(nsIFile),
                                 getter_AddRefs(profileDir));
      if (NS_SUCCEEDED(rv)) {
        nsAutoCString path;
        MOZ_ALWAYS_SUCCEEDS(profileDir->GetNativePath(path));
        aCmdLine.push_back("-profile");
        aCmdLine.push_back(path.get());
      }
#endif
    }
  }
}

bool
GeckoChildProcessHost::PerformAsyncLaunchInternal(std::vector<std::string>& aExtraOpts)
{
  // We rely on the fact that InitializeChannel() has already been processed
  // on the IO thread before this point is reached.
  if (!GetChannel()) {
    return false;
  }

  base::ProcessHandle process = 0;

  // send the child the PID so that it can open a ProcessHandle back to us.
  // probably don't want to do this in the long run
  char pidstring[32];
  SprintfLiteral(pidstring,"%d", base::Process::Current().pid());

  const char* const childProcessType =
      XRE_ChildProcessTypeToString(mProcessType);

//--------------------------------------------------
#if defined(OS_POSIX)
  // For POSIX, we have to be extremely anal about *not* using
  // std::wstring in code compiled with Mozilla's -fshort-wchar
  // configuration, because chromium is compiled with -fno-short-wchar
  // and passing wstrings from one config to the other is unsafe.  So
  // we split the logic here.

#if defined(OS_LINUX) || defined(OS_BSD) || defined(OS_SOLARIS)
  base::environment_map newEnvVars;

#if defined(MOZ_WIDGET_GTK)
  if (mProcessType == GeckoProcessType_Content) {
    // disable IM module to avoid sandbox violation
    newEnvVars["GTK_IM_MODULE"] = "gtk-im-context-simple";
  }
#endif

  // XPCOM may not be initialized in some subprocesses.  We don't want
  // to initialize XPCOM just for the directory service, especially
  // since LD_LIBRARY_PATH is already set correctly in subprocesses
  // (meaning that we don't need to set that up in the environment).
  if (ShouldHaveDirectoryService()) {
    MOZ_ASSERT(gGREBinPath);
    nsCString path;
    NS_CopyUnicodeToNative(nsDependentString(gGREBinPath), path);
# if defined(OS_LINUX) || defined(OS_BSD)
    const char *ld_library_path = PR_GetEnv("LD_LIBRARY_PATH");
    nsCString new_ld_lib_path(path.get());

#  if (MOZ_WIDGET_GTK == 3)
    if (mProcessType == GeckoProcessType_Plugin) {
      new_ld_lib_path.Append("/gtk2:");
      new_ld_lib_path.Append(path.get());
    }
#endif
    if (ld_library_path && *ld_library_path) {
      new_ld_lib_path.Append(':');
      new_ld_lib_path.Append(ld_library_path);
    }
    newEnvVars["LD_LIBRARY_PATH"] = new_ld_lib_path.get();

# endif  // OS_LINUX
  }
#endif  // OS_LINUX

  FilePath exePath;
  BinaryPathType pathType = GetPathToBinary(exePath, mProcessType);

#ifdef ANDROID
  // Remap the Android property workspace to a well-known int,
  // and update the environment to reflect the new value for the
  // child process.
  const char *apws = getenv("ANDROID_PROPERTY_WORKSPACE");
  if (apws) {
    int fd = atoi(apws);
    mFileMap.push_back(std::pair<int, int>(fd, kMagicAndroidSystemPropFd));

    char buf[32];
    char *szptr = strchr(apws, ',');

    snprintf(buf, sizeof(buf), "%d%s", kMagicAndroidSystemPropFd, szptr);
    newEnvVars["ANDROID_PROPERTY_WORKSPACE"] = buf;
  }
#endif  // ANDROID

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
  // Preload libmozsandbox.so so that sandbox-related interpositions
  // can be defined there instead of in the executable.
  // (This could be made conditional on intent to use sandboxing, but
  // it's harmless for non-sandboxed processes.)
  {
    nsAutoCString preload;
    // Prepend this, because people can and do preload libpthread.
    // (See bug 1222500.)
    preload.AssignLiteral("libmozsandbox.so");
    if (const char* oldPreload = PR_GetEnv("LD_PRELOAD")) {
      // Doesn't matter if oldPreload is ""; extra separators are ignored.
      preload.Append(' ');
      preload.Append(oldPreload);
    }
    // Explicitly construct the std::string to make it clear that this
    // isn't retaining a pointer to the nsCString's buffer.
    newEnvVars["LD_PRELOAD"] = std::string(preload.get());
  }
#endif

  // remap the IPC socket fd to a well-known int, as the OS does for
  // STDOUT_FILENO, for example
  int srcChannelFd, dstChannelFd;
  channel().GetClientFileDescriptorMapping(&srcChannelFd, &dstChannelFd);
  mFileMap.push_back(std::pair<int,int>(srcChannelFd, dstChannelFd));

  // no need for kProcessChannelID, the child process inherits the
  // other end of the socketpair() from us

  std::vector<std::string> childArgv;

  childArgv.push_back(exePath.value());

  if (pathType == BinaryPathType::Self) {
    childArgv.push_back("-contentproc");
  }

  childArgv.insert(childArgv.end(), aExtraOpts.begin(), aExtraOpts.end());

  if (Omnijar::IsInitialized()) {
    // Make sure that child processes can find the omnijar
    // See XRE_InitCommandLine in nsAppRunner.cpp
    nsAutoCString path;
    nsCOMPtr<nsIFile> file = Omnijar::GetPath(Omnijar::GRE);
    if (file && NS_SUCCEEDED(file->GetNativePath(path))) {
      childArgv.push_back("-greomni");
      childArgv.push_back(path.get());
    }
    file = Omnijar::GetPath(Omnijar::APP);
    if (file && NS_SUCCEEDED(file->GetNativePath(path))) {
      childArgv.push_back("-appomni");
      childArgv.push_back(path.get());
    }
  }

  // Add the application directory path (-appdir path)
  AddAppDirToCommandLine(childArgv);

  childArgv.push_back(pidstring);

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
  {
    int srcFd, dstFd;
    SandboxReporter::Singleton()
      ->GetClientFileDescriptorMapping(&srcFd, &dstFd);
    mFileMap.push_back(std::make_pair(srcFd, dstFd));
  }
#endif

  childArgv.push_back(childProcessType);

  base::LaunchApp(childArgv, mFileMap,
#if defined(OS_LINUX) || defined(OS_BSD)
                  newEnvVars,
#endif
                  false, &process);

  // We're in the parent and the child was launched. Close the child FD in the
  // parent as soon as possible, which will allow the parent to detect when the
  // child closes its FD (either due to normal exit or due to crash).
  GetChannel()->CloseClientFileDescriptor();

//--------------------------------------------------
#else
#  error Sorry
#endif

  if (!process) {
    return false;
  }

  if (!OpenPrivilegedHandle(base::GetProcId(process))
     ) {
    NS_RUNTIMEABORT("cannot open handle to child process");
  }
  MonitorAutoLock lock(mMonitor);
  mProcessState = PROCESS_CREATED;
  lock.Notify();

  return true;
}

bool
GeckoChildProcessHost::OpenPrivilegedHandle(base::ProcessId aPid)
{
  if (mChildProcessHandle) {
    MOZ_ASSERT(aPid == base::GetProcId(mChildProcessHandle));
    return true;
  }

  return base::OpenPrivilegedProcessHandle(aPid, &mChildProcessHandle);
}

void
GeckoChildProcessHost::OnChannelConnected(int32_t peer_pid)
{
  if (!OpenPrivilegedHandle(peer_pid)) {
    NS_RUNTIMEABORT("can't open handle to child process");
  }
  MonitorAutoLock lock(mMonitor);
  mProcessState = PROCESS_CONNECTED;
  lock.Notify();
}

void
GeckoChildProcessHost::OnMessageReceived(IPC::Message&& aMsg)
{
  // We never process messages ourself, just save them up for the next
  // listener.
  mQueue.push(Move(aMsg));
}

void
GeckoChildProcessHost::OnChannelError()
{
  // Update the process state to an error state if we have a channel
  // error before we're connected. This fixes certain failures,
  // but does not address the full range of possible issues described
  // in the FIXME comment below.
  MonitorAutoLock lock(mMonitor);
  if (mProcessState < PROCESS_CONNECTED) {
    mProcessState = PROCESS_ERROR;
    lock.Notify();
  }
  // FIXME/bug 773925: save up this error for the next listener.
}

void
GeckoChildProcessHost::GetQueuedMessages(std::queue<IPC::Message>& queue)
{
  // If this is called off the IO thread, bad things will happen.
  DCHECK(MessageLoopForIO::current());
  swap(queue, mQueue);
  // We expect the next listener to take over processing of our queue.
}

bool GeckoChildProcessHost::sRunSelfAsContentProc(false);

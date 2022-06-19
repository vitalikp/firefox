// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/debug/profiler.h"

#include <string>

#include "base/debug/debugging_flags.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"

// TODO(peria): Enable profiling on Windows.
#if BUILDFLAG(ENABLE_PROFILING) && !defined(NO_TCMALLOC)
#include "third_party/tcmalloc/chromium/src/gperftools/profiler.h"
#endif

namespace base {
namespace debug {

// TODO(peria): Enable profiling on Windows.
#if BUILDFLAG(ENABLE_PROFILING) && !defined(NO_TCMALLOC)

static int profile_count = 0;

void StartProfiling(const std::string& name) {
  ++profile_count;
  std::string full_name(name);
  std::string pid = IntToString(GetCurrentProcId());
  std::string count = IntToString(profile_count);
  ReplaceSubstringsAfterOffset(&full_name, 0, "{pid}", pid);
  ReplaceSubstringsAfterOffset(&full_name, 0, "{count}", count);
  ProfilerStart(full_name.c_str());
}

void StopProfiling() {
  ProfilerFlush();
  ProfilerStop();
}

void FlushProfiling() {
  ProfilerFlush();
}

bool BeingProfiled() {
  return ProfilingIsEnabledForAllThreads();
}

void RestartProfilingAfterFork() {
  ProfilerRegisterThread();
}

#else

void StartProfiling(const std::string& name) {
}

void StopProfiling() {
}

void FlushProfiling() {
}

bool BeingProfiled() {
  return false;
}

void RestartProfilingAfterFork() {
}

#endif

bool IsBinaryInstrumented() {
  return false;
}

ReturnAddressLocationResolver GetProfilerReturnAddrResolutionFunc() {
  return NULL;
}

DynamicFunctionEntryHook GetProfilerDynamicFunctionEntryHookFunc() {
  return NULL;
}

AddDynamicSymbol GetProfilerAddDynamicSymbolFunc() {
  return NULL;
}

MoveDynamicSymbol GetProfilerMoveDynamicSymbolFunc() {
  return NULL;
}

}  // namespace debug
}  // namespace base

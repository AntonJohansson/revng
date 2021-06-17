/// \file Statistics.cpp
/// \brief Implementation of the statistics collection framework

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Support/CommandLine.h"
#include "revng/Support/Statistics.h"

namespace cl = llvm::cl;

// Was: -stats
static cl::opt<bool> Statistics("statistics",
                                cl::desc("print statistics upon exit or "
                                         "SIGINT. Use "
                                         "this argument, ignore -stats."),
                                cl::cat(MainCategory));

static cl::alias A1("T",
                    cl::desc("Alias for -statistics"),
                    cl::aliasopt(Statistics),
                    cl::cat(MainCategory));

#ifdef _POSIX_C_SOURCE
struct Handler {
  int Signal;
  bool Restore;
  struct sigaction OldHandler;
  struct sigaction NewHandler;
};

// Print statistics on SIGINT (Ctrl + C), SIGABRT (assertions) and SIGUSR1.
// For SIGUSR1, don't terminate program execution.
static std::array<Handler, 3> Handlers = { { { SIGINT, true, {}, {} },
                                             { SIGABRT, true, {}, {} },
                                             { SIGUSR1, false, {}, {} } } };
#endif

llvm::ManagedStatic<OnQuitRegistry> OnQuitStatistics;

void installStatistics() {
  if (Statistics)
    OnQuitStatistics->install();
}

static void onQuit() {
  dbg << "\n";
  OnQuitStatistics->dump();
}

#ifdef _POSIX_C_SOURCE
static void onQuitSignalHandler(int Signal) {
  Handler *SignalHandler = nullptr;
  for (Handler &H : Handlers)
    if (H.Signal == Signal)
      SignalHandler = &H;

  // Assert we were notified of the signal we expected
  revng_assert(SignalHandler != nullptr);

  onQuit();

  if (not SignalHandler->Restore)
    return;

  int Result = sigaction(Signal, &SignalHandler->OldHandler, nullptr);
  revng_assert(Result == 0);
  raise(Signal);
}
#endif

void OnQuitRegistry::install() {
  // Dump on normal exit
  std::atexit(onQuit);

#ifdef _POSIX_C_SOURCE
  // Register signal handlers
  for (Handler &H : Handlers) {
    H.NewHandler.sa_handler = &onQuitSignalHandler;

    int Result = sigaction(H.Signal, &H.NewHandler, &H.OldHandler);
    revng_assert(Result == 0);
    revng_assert(H.OldHandler.sa_handler == nullptr);
  }
#endif
}

void RunningStatistics::onQuit() {
  dump();
  dbg << "\n";
}

OnQuitInteraface::~OnQuitInteraface() {
}

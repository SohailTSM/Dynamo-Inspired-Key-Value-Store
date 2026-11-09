#include "common/signals.h"

#include <pthread.h>
#include <signal.h>

#include <thread>

namespace dkv {

static sigset_t TermSet() {
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, SIGINT);
  sigaddset(&s, SIGTERM);
  return s;
}

void BlockTerminationSignals() {
  sigset_t s = TermSet();
  pthread_sigmask(SIG_BLOCK, &s, nullptr);
  signal(SIGPIPE, SIG_IGN);
}

int WaitForTerminationSignal() {
  sigset_t s = TermSet();
  int sig = 0;
  sigwait(&s, &sig);
  return sig;
}

void OnTerminationSignal(std::function<void()> on_signal) {
  std::thread([on_signal] {
    sigset_t s = TermSet();
    int sig = 0;
    sigwait(&s, &sig);
    on_signal();
  }).detach();
}

}  // namespace dkv

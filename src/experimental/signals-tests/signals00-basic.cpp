#include "sys.h"
#include "utils/Signals.h"
#include "utils/threading/Gate.h"
#include "cwds/signal_safe_printf.h"
#include "debug.h"
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <unistd.h>     // getpid
#include <signal.h>     // kill

// Gates used to make threads wait.
utils::threading::Gate until_SIGINT;            // Opened when SIGINT is received.
utils::threading::Gate until_SIGUSR1_or_S1;     // Opened when SIGUSR1 or S1 is received.

// Set if the signal handler of SIGUSR1 is received. This should not happen.
std::atomic<bool> failure{};

// Signal handlers.
extern "C" {

// Signal handler for SIGINT.
void my_SIGINT_callback(int n)
{
  signal_safe_printf("Calling my_SIGINT_callback(%d)\n", n);
  until_SIGINT.open();
}

// Signal handler for SIGUSR1.
void my_SIGUSR1_callback(int n)
{
  signal_safe_printf("Calling my_SIGUSR1_callback(%d)\n", n);
  failure = true;
  until_SIGUSR1_or_S1.open();
}

// Signal handler for RT signal 's1'.
void my_S1_callback(int n)
{
  signal_safe_printf("Calling my_S1_callback(%d)\n", n);
  until_SIGUSR1_or_S1.open();
}

} // extern "C"

#ifdef CWDEBUG
// Initialization code for new threads.
void init_debug(int thread)
{
  {
    std::ostringstream thread_name;
    thread_name << "thread" << std::dec << std::setfill('0') << std::setw(2) << thread;
    Debug(NAMESPACE_DEBUG::init_thread(thread_name.str()));
  }
}
#endif

void thread1()
{
  Debug(init_debug(1));
  DoutEntering(dc::notice, "thread1()");

  // This thread receives SIGINT signals.
  utils::Signal::unblock(SIGINT);
  // Wait until the signal handler of SIGINT was called (press ^C to make it happen).
  until_SIGINT.wait();

  // Try to block signal SIGUSR1 for thread2!
  utils::Signal::block_and_unregister(SIGUSR1);

  // Immediately after, send this signal to the process.
  Dout(dc::notice, "Calling 'kill(" << getpid() << ", SIGUSR1)'");
  kill(getpid(), SIGUSR1);
}

void thread2(int s1)
{
  Debug(init_debug(2));
  DoutEntering(dc::notice, "thread2()");

  // This thread receives SIGUSR1 signals.
  utils::Signal::unblock(SIGUSR1);

  // It also receives RT signal 's1'.
  utils::Signal::unblock(s1, my_S1_callback);

  // Wait until either SIGUSR1 or S1 was received.
  until_SIGUSR1_or_S1.wait();
}

void test1()
{
  DoutEntering(dc::notice, "test1()");

  // This process uses SIGINT, SIGUSR1 and 1 RT signals.
  utils::Signals signals({SIGINT, SIGUSR1}, 1);
  int s1 = utils::Signal::next_rt_signum();
  Dout(dc::notice, "Reserved RT signals: " << s1 << '.');

  // Set up the signal handlers of SIGINT and SIGUSR1 here.
  signals.register_callback(SIGINT, my_SIGINT_callback);
  signals.register_callback(SIGUSR1, my_SIGUSR1_callback);

  // Start two threads.
  std::thread t1(thread1);
  std::thread t2(thread2, s1);

  // Wait until thread1 finished - that is, until the user pressed control-C.
  t1.join();

  // Wait 500 ms - then send signal s1 to the process.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  kill(getpid(), s1);

  // Wait till thread2 received either SIGUSR1 (hopefully not) or the s1 signal.
  t2.join();

  // Print result.
  Dout(dc::notice, (failure ? "Failure" : "Success!"));
  ASSERT(!failure);
}

int main()
{
  Debug(NAMESPACE_DEBUG::init());
  test1();
  Dout(dc::notice, "Leaving main()...");
}

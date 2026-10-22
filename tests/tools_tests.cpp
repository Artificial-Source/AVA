#include "sys.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"

void run_tools_file_tests();
void run_tools_search_tests();
void run_tools_process_network_tests();

void run_tools_tests()
{
  ava::tests::HermeticTestEnvironment environment("ava-tools-");
  ScopedEnvVar const path("PATH", "/usr/bin:/bin");
  run_tools_file_tests();
  run_tools_search_tests();
  run_tools_process_network_tests();
}

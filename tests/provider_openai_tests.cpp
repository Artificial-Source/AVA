#include "sys.h"
#include "tests/provider_openai_test_suite.h"
#include "tests/support/test_env.h"

namespace ava::tests::provider_openai_suite {

void test_openai_provider_contract()
{
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto const request = exercise_contract_request_serialization(provider);
  exercise_contract_sse_validation(provider);
  exercise_contract_http_retry(provider);
  exercise_contract_terminal_parsing(provider);
  exercise_contract_final_transport(request);
}

}  // namespace ava::tests::provider_openai_suite

void run_provider_openai_tests()
{
  ava::tests::HermeticTestEnvironment environment("openai-suite-");
  using namespace ava::tests::provider_openai_suite;

  test_openai_provider_contract();
  test_openai_ordered_output_capture();
  test_v4_ordered_turn_persistence_replay_and_openai_serialization();
  test_openai_documented_message_reconciliation();
  test_openai_non_stream_output_order_and_strictness();
  test_openai_documented_function_completion_validation();
  test_openai_responses_refusal_and_unsupported_output();
  test_openai_stream_bridge_hides_internal_text_lifecycle_events();
  test_openai_incremental_sse_parser();
  test_openai_compatible_provider_contract();
  test_openai_compatible_parsing();
  test_builtin_openai_compatible_provider_contracts();
  test_closed_provider_finish_reason_catalog();
  test_openai_provider_parser_budgets();
  test_builtin_provider_registry();
}

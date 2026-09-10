#include "sys.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/app/Application.h"
#include "ava/app/app.h"
#include "ava/app/ava_debug.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include "debug.h"

int main(int argc, char** argv)
{
  Debug(ava::app::initialize_debug());
  ava::app::Application application;

  std::shared_ptr<ava::process::Supervisor> supervisor;
  std::optional<ava::process::ProcessScopeV1> application_process_scope;
  try
  {
    supervisor = std::make_shared<ava::process::Supervisor>();
    auto generated_scope = ava::process::ProcessScopeV1::application(supervisor);
    if (!generated_scope)
    {
      std::cerr << "AVA process supervision startup failed: " << generated_scope.error().format() << '\n';
      return 1;
    }
    application_process_scope = std::move(*generated_scope);
  }
  catch (...)
  {
    std::cerr << "AVA process supervision startup failed: application process authority is unavailable.\n";
    return 1;
  }

  int status = ava::app::run(argc, argv, *application_process_scope);

  supervisor->stop_accepting();
  auto const cleanup = supervisor->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(2));
  if (!cleanup.complete)
  {
    std::cerr << "AVA process cleanup did not complete.\n";
    if (status == 0)
      status = 1;
  }
  return status;
}

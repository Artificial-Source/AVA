#pragma once
#include "ava/http/transport.h"
#include "ava/provider/provider.h"

#include <vector>

namespace ava::tests {

class FakeTransport final : public ava::http::Transport
{
 public:
  explicit FakeTransport(std::vector<ava::http::HttpResponse> responses);
  using ava::http::Transport::send;
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override;
  [[nodiscard]] std::vector<ava::http::HttpRequest> const& requests() const noexcept;

 private:
  std::vector<ava::http::HttpResponse> responses_;
  std::vector<ava::http::HttpRequest> requests_;
};

}  // namespace ava::tests

#pragma once

#include <deque>
#include <filesystem>

#include "ai/client.h"

namespace patchwork {

class MockAiClient : public IAiClient {
  public:
    explicit MockAiClient(std::filesystem::path fixture_root);

    bool StartRequest(const AiRequest& request, std::string* error) override;
    std::vector<AiEvent> PollEvents() override;
    bool HasActiveRequest() const override;
    void Shutdown() override;

  private:
    AiResponse BuildResponse(const AiRequest& request) const;
    std::filesystem::path FixtureFor(const AiRequest& request) const;

    std::filesystem::path fixture_root_;
    std::deque<AiEvent> queued_events_;
};

}  // namespace patchwork

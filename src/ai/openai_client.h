#pragma once

#include <deque>
#include <future>
#include <string>

#include "ai/client.h"

namespace flowstate {

class OpenAiClient : public IAiClient {
  public:
    OpenAiClient();

    bool StartRequest(const AiRequest& request, std::string* error) override;
    std::vector<AiEvent> PollEvents() override;
    bool HasActiveRequest() const override;
    void Shutdown() override;

  private:
    AiResponse CompleteRequest(const AiRequest& request);

    std::string model_;
    std::future<AiResponse> pending_response_;
    std::deque<AiEvent> queued_events_;
    bool request_active_ = false;
};

}  // namespace flowstate

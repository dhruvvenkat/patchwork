#pragma once

#include "ai/client.h"

namespace flowstate {

class NoAiClient : public IAiClient {
  public:
    bool StartRequest(const AiRequest& request, std::string* error) override;
    std::vector<AiEvent> PollEvents() override;
    bool HasActiveRequest() const override;
    void Shutdown() override;
};

}  // namespace flowstate

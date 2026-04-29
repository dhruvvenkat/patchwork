#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "ai/client.h"
#include "ai/local_agent.h"

namespace patchwork {

class CodexClient : public IAiClient {
  public:
    CodexClient();
    explicit CodexClient(std::unique_ptr<ILocalAgentClient> local_agent);

    bool StartRequest(const AiRequest& request, std::string* error) override;
    std::vector<AiEvent> PollEvents() override;
    bool HasActiveRequest() const override;
    void Shutdown() override;

  private:
    void FinalizeRequest();

    std::string model_;
    std::string profile_;
    std::unique_ptr<ILocalAgentClient> local_agent_;
    std::optional<std::string> session_id_;
    std::filesystem::path session_root_;
    std::deque<AiEvent> queued_events_;
    std::string streamed_text_;
    std::string final_text_;
    bool request_active_ = false;
    bool streaming_state_emitted_ = false;
    bool request_started_ = false;
};

}  // namespace patchwork

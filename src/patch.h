#pragma once

#include <optional>
#include <string>
#include <vector>

#include "buffer.h"
#include "diff.h"

namespace flowstate {

enum class HunkDecision {
    Pending,
    Accepted,
    Rejected,
    Applied,
    Failed,
};

struct PatchHunkState {
    Hunk hunk;
    HunkDecision decision = HunkDecision::Pending;
};

struct PatchSession {
    PatchSet patch;
    std::vector<PatchHunkState> hunks;
    std::vector<std::string> original_lines;
    std::vector<size_t> preview_row_starts;
    size_t current_hunk = 0;
};

struct PatchApplyResult {
    bool success = false;
    std::string message;
};

bool PatchTargetsBuffer(const PatchSet& patch, const Buffer& buffer);
PatchSession CreatePatchSession(const PatchSet& patch, const Buffer& buffer);
std::vector<std::string> RenderPatchPreview(PatchSession& session);
PatchApplyResult AcceptCurrentHunk(Buffer& buffer, PatchSession& session);
void RejectCurrentHunk(PatchSession& session);
PatchApplyResult AcceptAllHunks(Buffer& buffer, PatchSession& session);
void RejectAllHunks(PatchSession& session);
size_t HunkIndexForPreviewRow(const PatchSession& session, size_t row);
std::string DecisionLabel(HunkDecision decision);

}  // namespace flowstate


#include "patch.h"

#include <algorithm>
#include <filesystem>

namespace patchwork {

namespace {

std::vector<std::string> OldSegment(const Hunk& hunk) {
    std::vector<std::string> segment;
    for (const DiffLine& line : hunk.lines) {
        if (line.type == DiffLineType::Context || line.type == DiffLineType::Remove) {
            segment.push_back(line.text);
        }
    }
    return segment;
}

std::vector<std::string> NewSegment(const Hunk& hunk) {
    std::vector<std::string> segment;
    for (const DiffLine& line : hunk.lines) {
        if (line.type == DiffLineType::Context || line.type == DiffLineType::Add) {
            segment.push_back(line.text);
        }
    }
    return segment;
}

bool SegmentMatches(const std::vector<std::string>& lines,
                    size_t start,
                    const std::vector<std::string>& expected) {
    if (start + expected.size() > lines.size()) {
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (lines[start + index] != expected[index]) {
            return false;
        }
    }
    return true;
}

std::optional<size_t> LocateHunk(const std::vector<std::string>& lines, const Hunk& hunk) {
    const std::vector<std::string> expected = OldSegment(hunk);
    const size_t preferred = hunk.old_start > 0 ? hunk.old_start - 1 : 0;

    if (expected.empty()) {
        return std::min(preferred, lines.size());
    }
    if (preferred <= lines.size() && SegmentMatches(lines, preferred, expected)) {
        return preferred;
    }

    std::optional<size_t> match;
    for (size_t start = 0; start + expected.size() <= lines.size(); ++start) {
        if (!SegmentMatches(lines, start, expected)) {
            continue;
        }
        if (match.has_value()) {
            return std::nullopt;
        }
        match = start;
    }
    return match;
}

PatchApplyResult ApplySingleHunk(Buffer& buffer, PatchHunkState& state) {
    if (state.decision == HunkDecision::Applied) {
        return {.success = true, .message = "Hunk already applied."};
    }
    if (state.decision == HunkDecision::Rejected) {
        return {.message = "Hunk rejected."};
    }

    std::vector<std::string> lines = buffer.lines();
    const std::optional<size_t> start = LocateHunk(lines, state.hunk);
    if (!start.has_value()) {
        state.decision = HunkDecision::Failed;
        return {.message = "Patch hunk does not match current buffer."};
    }

    const std::vector<std::string> old_segment = OldSegment(state.hunk);
    const std::vector<std::string> new_segment = NewSegment(state.hunk);
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(*start),
                lines.begin() + static_cast<std::ptrdiff_t>(*start + old_segment.size()));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*start), new_segment.begin(), new_segment.end());
    buffer.setLines(std::move(lines), true);
    state.decision = HunkDecision::Applied;
    return {.success = true, .message = "Hunk applied."};
}

}  // namespace

bool PatchTargetsBuffer(const PatchSet& patch, const Buffer& buffer) {
    if (!buffer.path().has_value()) {
        return false;
    }

    const std::filesystem::path target(patch.targetFile());
    const std::filesystem::path buffer_path = *buffer.path();
    return target == buffer_path || target.filename() == buffer_path.filename();
}

PatchSession CreatePatchSession(const PatchSet& patch, const Buffer& buffer) {
    PatchSession session;
    session.patch = patch;
    session.original_lines = buffer.lines();
    for (const Hunk& hunk : patch.hunks) {
        session.hunks.push_back({hunk, HunkDecision::Pending});
    }
    return session;
}

std::string DecisionLabel(HunkDecision decision) {
    switch (decision) {
        case HunkDecision::Pending:
            return "PENDING";
        case HunkDecision::Accepted:
            return "ACCEPTED";
        case HunkDecision::Rejected:
            return "REJECTED";
        case HunkDecision::Applied:
            return "APPLIED";
        case HunkDecision::Failed:
            return "FAILED";
    }
    return "UNKNOWN";
}

std::vector<std::string> RenderPatchPreview(PatchSession& session) {
    std::vector<std::string> preview;
    session.preview_row_starts.clear();

    for (size_t index = 0; index < session.hunks.size(); ++index) {
        session.preview_row_starts.push_back(preview.size());
        const PatchHunkState& state = session.hunks[index];
        preview.push_back("@@ -" + std::to_string(state.hunk.old_start) + "," +
                          std::to_string(state.hunk.old_count) + " +" +
                          std::to_string(state.hunk.new_start) + "," +
                          std::to_string(state.hunk.new_count) + " @@ [" +
                          DecisionLabel(state.decision) + "]");

        for (const DiffLine& line : state.hunk.lines) {
            char prefix = ' ';
            if (line.type == DiffLineType::Add) {
                prefix = '+';
            } else if (line.type == DiffLineType::Remove) {
                prefix = '-';
            }
            preview.push_back(std::string(1, prefix) + line.text);
        }

        if (index + 1 < session.hunks.size()) {
            preview.emplace_back();
        }
    }

    if (preview.empty()) {
        preview.emplace_back("No patch hunks.");
    }
    return preview;
}

PatchApplyResult AcceptCurrentHunk(Buffer& buffer, PatchSession& session) {
    if (session.hunks.empty()) {
        return {.message = "No patch session."};
    }

    session.current_hunk = std::min(session.current_hunk, session.hunks.size() - 1);
    PatchHunkState& hunk = session.hunks[session.current_hunk];
    hunk.decision = HunkDecision::Accepted;
    return ApplySingleHunk(buffer, hunk);
}

void RejectCurrentHunk(PatchSession& session) {
    if (session.hunks.empty()) {
        return;
    }
    session.current_hunk = std::min(session.current_hunk, session.hunks.size() - 1);
    session.hunks[session.current_hunk].decision = HunkDecision::Rejected;
}

PatchApplyResult AcceptAllHunks(Buffer& buffer, PatchSession& session) {
    if (session.hunks.empty()) {
        return {.message = "No patch session."};
    }

    const std::vector<std::string> snapshot = buffer.lines();
    std::vector<HunkDecision> decisions_before;
    decisions_before.reserve(session.hunks.size());
    for (const PatchHunkState& hunk : session.hunks) {
        decisions_before.push_back(hunk.decision);
    }
    const bool was_dirty = buffer.dirty();

    for (size_t reverse_index = session.hunks.size(); reverse_index-- > 0;) {
        PatchHunkState& hunk = session.hunks[reverse_index];
        if (hunk.decision == HunkDecision::Rejected || hunk.decision == HunkDecision::Applied) {
            continue;
        }
        hunk.decision = HunkDecision::Accepted;
        const PatchApplyResult result = ApplySingleHunk(buffer, hunk);
        if (!result.success) {
            buffer.setLines(snapshot, false);
            if (!was_dirty) {
                buffer.clearDirty();
            }
            for (size_t index = 0; index < session.hunks.size(); ++index) {
                session.hunks[index].decision = decisions_before[index];
            }
            session.hunks[reverse_index].decision = HunkDecision::Failed;
            return result;
        }
    }

    return {.success = true, .message = "Accepted patch hunks applied."};
}

void RejectAllHunks(PatchSession& session) {
    for (PatchHunkState& hunk : session.hunks) {
        if (hunk.decision != HunkDecision::Applied) {
            hunk.decision = HunkDecision::Rejected;
        }
    }
}

size_t HunkIndexForPreviewRow(const PatchSession& session, size_t row) {
    if (session.preview_row_starts.empty()) {
        return 0;
    }

    size_t current = 0;
    for (size_t index = 0; index < session.preview_row_starts.size(); ++index) {
        if (session.preview_row_starts[index] > row) {
            break;
        }
        current = index;
    }
    return current;
}

}  // namespace patchwork

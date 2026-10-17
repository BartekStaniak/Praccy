#include "undo_manager.h"
#include "../audio/graph_engine.h"
#include "../plugins/plugin_window.h"

namespace praccy::state {

namespace {

void applyScenePresetToGraph(const ScenePreset& preset, audio::GraphEngine& graph) {
    SceneManager::applyGraphSnapshot(preset, graph);
}

} // anonymous namespace

bool UndoManager::shouldCoalesce(UndoActionType actionType,
                                 const std::string& targetKey,
                                 bool force) const noexcept {
    if (force || !m_inContinuousGroup || m_undoStack.empty()) {
        return false;
    }
    const bool isContinuous = (actionType == UndoActionType::ParameterTweak ||
                               actionType == UndoActionType::MidiCcAutomation);
    if (!isContinuous) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto& top = m_undoStack.back();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - top.timestamp).count();
    const bool sameTarget = (targetKey.empty() || top.targetIdentifier == targetKey);
    return (elapsedMs < kCoalesceIdleThresholdMs) && sameTarget;
}

void UndoManager::pushSnapshot(ScenePreset snapshot,
                               std::string description,
                               UndoActionType actionType,
                               const std::string& targetKey,
                               bool force)
{
    const auto now = std::chrono::steady_clock::now();

    const bool isContinuous = (actionType == UndoActionType::ParameterTweak ||
                               actionType == UndoActionType::MidiCcAutomation);

    if (!force && isContinuous && m_inContinuousGroup && !m_undoStack.empty()) {
        auto& top = m_undoStack.back();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - top.timestamp).count();
        const bool sameTarget = (targetKey.empty() || top.targetIdentifier == targetKey);

        if (elapsedMs < kCoalesceIdleThresholdMs && sameTarget) {
            // Coalesce: update current transaction state in place
            top.snapshot = std::move(snapshot);
            top.timestamp = now;
            if (!description.empty()) {
                top.description = description;
                m_lastActionDesc = description;
            }
            return;
        }
    }

    // New discrete transaction
    if (m_undoStack.size() >= kMaxHistoryLevels) {
        m_undoStack.pop_front(); // Evict oldest history entry
    }

    UndoTransaction tx{
        .snapshot = std::move(snapshot),
        .description = description,
        .actionType = actionType,
        .targetIdentifier = targetKey,
        .timestamp = now
    };

    m_undoStack.push_back(std::move(tx));
    m_redoStack.clear();
    m_lastActionDesc = description;
    m_inContinuousGroup = isContinuous;
}

void UndoManager::recordSnapshot(const ScenePreset& snapshot, bool force) {
    pushSnapshot(snapshot, "Parameter Tweak", UndoActionType::ParameterTweak, "", force);
}

void UndoManager::commitActiveTransaction() noexcept {
    m_inContinuousGroup = false;
}

bool UndoManager::canUndo() const noexcept {
    return !m_undoStack.empty();
}

bool UndoManager::canRedo() const noexcept {
    return !m_redoStack.empty();
}

bool UndoManager::undo(ScenePreset& outPreset) {
    if (m_undoStack.empty()) {
        return false;
    }

    m_inContinuousGroup = false;

    if (m_undoStack.size() > 1) {
        auto current = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        outPreset = m_undoStack.back().snapshot;
        m_redoStack.push_back(std::move(current));
        m_lastActionDesc = "Undo: " + m_redoStack.back().description;
        return true;
    }

    // Only 1 item remaining
    outPreset = m_undoStack.back().snapshot;
    auto current = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    m_redoStack.push_back(std::move(current));
    m_lastActionDesc = "Undo: " + m_redoStack.back().description;
    return true;
}

bool UndoManager::redo(ScenePreset& outPreset) {
    if (m_redoStack.empty()) {
        return false;
    }

    m_inContinuousGroup = false;

    auto next = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    outPreset = next.snapshot;
    m_lastActionDesc = "Redo: " + next.description;
    m_undoStack.push_back(std::move(next));
    return true;
}

bool UndoManager::undo() {
    ScenePreset dummy;
    return undo(dummy);
}

bool UndoManager::redo() {
    ScenePreset dummy;
    return redo(dummy);
}

bool UndoManager::undo(audio::GraphEngine& graph, SceneManager& /*scenes*/) {
    if (m_undoStack.empty()) {
        return false;
    }
    if (m_undoStack.size() == 1) {
        // Preserve the baseline snapshot at the bottom of the undo stack so subsequent
        // parameter or rack edits always have a baseline state to undo back to.
        m_inContinuousGroup = false;
        applyScenePresetToGraph(m_undoStack.back().snapshot, graph);
        m_lastActionDesc = "Undo: " + m_undoStack.back().description;
        return true;
    }
    ScenePreset restored;
    if (!undo(restored)) {
        return false;
    }
    applyScenePresetToGraph(restored, graph);
    return true;
}

bool UndoManager::redo(audio::GraphEngine& graph, SceneManager& /*scenes*/) {
    ScenePreset restored;
    if (!redo(restored)) {
        return false;
    }
    applyScenePresetToGraph(restored, graph);
    return true;
}

void UndoManager::clear() noexcept {
    m_undoStack.clear();
    m_redoStack.clear();
    m_lastActionDesc.clear();
    m_inContinuousGroup = false;
}

} // namespace praccy::state

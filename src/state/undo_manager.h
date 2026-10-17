#pragma once

#include "scene_manager.h"
#include <deque>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>

namespace praccy::audio { class GraphEngine; }

namespace praccy::state {

enum class UndoActionType : uint8_t {
    ParameterTweak,     // Continuous knob/slider drag
    StructuralMutation, // Add slot, delete slot, reorder, split, dissolve
    SceneSwitch,        // Preset recalled or scene button pressed
    MidiCcAutomation    // External MIDI CC streaming
};

struct UndoTransaction {
    ScenePreset snapshot;
    std::string description;
    UndoActionType actionType{UndoActionType::ParameterTweak};
    std::string targetIdentifier; // Slot/Param key for continuous coalescing
    std::chrono::steady_clock::time_point timestamp;
};

// Value equality operators for ScenePreset hierarchy
inline bool operator==(const PluginSlotPreset& a, const PluginSlotPreset& b) {
    return a.name == b.name && a.path == b.path && a.type == b.type &&
           a.bypassed == b.bypassed && a.dryWet == b.dryWet &&
           a.inputGainDb == b.inputGainDb && a.outputGainDb == b.outputGainDb &&
           a.state == b.state && a.macros == b.macros && a.paramValues == b.paramValues;
}

inline bool operator==(const BranchPreset& a, const BranchPreset& b) {
    return a.name == b.name && a.gainDb == b.gainDb && a.pan == b.pan &&
           a.muted == b.muted && a.solo == b.solo && a.phaseInvert == b.phaseInvert &&
           a.slots == b.slots;
}

inline bool operator==(const NodePreset& a, const NodePreset& b) {
    return a.kind == b.kind && a.slot == b.slot && a.branches == b.branches &&
           a.branchBlend == b.branchBlend;
}

inline bool operator==(const ScenePreset& a, const ScenePreset& b) {
    return a.name == b.name && a.nodes == b.nodes;
}

class UndoManager {
public:
    static constexpr size_t kMaxHistoryLevels = 64;
    static constexpr int64_t kCoalesceIdleThresholdMs = 400;

    UndoManager() = default;

    /// Check if an incoming continuous transaction would coalesce into the active top transaction
    [[nodiscard]] bool shouldCoalesce(UndoActionType actionType,
                                      const std::string& targetKey,
                                      bool force = false) const noexcept;

    /// Record a state snapshot with coalescing logic
    void pushSnapshot(ScenePreset snapshot,
                      std::string description = "Parameter Tweak",
                      UndoActionType actionType = UndoActionType::ParameterTweak,
                      const std::string& targetKey = "",
                      bool force = false);

    /// Convenience wrapper for pushSnapshot
    void recordSnapshot(const ScenePreset& snapshot, bool force = false);

    /// Finalize pending coalescing group (e.g. on mouse release)
    void commitActiveTransaction() noexcept;

    /// Check if undo/redo stacks are available
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;

    /// Restore previous state to outPreset
    bool undo(ScenePreset& outPreset);

    /// Re-apply next state to outPreset
    bool redo(ScenePreset& outPreset);

    /// Parameterless undo / redo
    bool undo();
    bool redo();

    /// Restore previous state applying to GraphEngine with 10ms crossfade
    bool undo(audio::GraphEngine& graph, SceneManager& scenes);

    /// Re-apply next state applying to GraphEngine with 10ms crossfade
    bool redo(audio::GraphEngine& graph, SceneManager& scenes);

    /// Clear history
    void clear() noexcept;

    [[nodiscard]] size_t undoCount() const noexcept { return m_undoStack.size(); }
    [[nodiscard]] size_t redoCount() const noexcept { return m_redoStack.size(); }
    [[nodiscard]] const std::string& lastActionDescription() const noexcept { return m_lastActionDesc; }

private:
    std::deque<UndoTransaction> m_undoStack;
    std::vector<UndoTransaction> m_redoStack;
    std::string m_lastActionDesc;
    bool m_inContinuousGroup{false};
};

} // namespace praccy::state

#pragma once

#include <chrono>

class DoubleTapDetector
{
public:
    DoubleTapDetector(float thresholdSeconds = 0.4f)
        : m_threshold(thresholdSeconds)
    {
    }

    // Returns true if this is a double-tap on the same target
    // Target can be any pointer (e.g., NPC reference, rigid body, etc.)
    bool Detect(void* target)
    {
        auto now = std::chrono::steady_clock::now();
        float currentTime = std::chrono::duration<float>(now - m_startTime).count();

        bool isDoubleTap = (m_lastTarget == target) &&
                           (currentTime - m_lastTime) < m_threshold;

        // Update state for next detection
        m_lastTime = currentTime;
        m_lastTarget = target;

        return isDoubleTap;
    }

    // Reset the detector (e.g., after handling a double-tap)
    void Reset()
    {
        m_lastTime = 0.0f;
        m_lastTarget = nullptr;
    }

    void SetThreshold(float seconds) { m_threshold = seconds; }
    float GetThreshold() const { return m_threshold; }

private:
    float m_threshold;
    float m_lastTime = 0.0f;
    void* m_lastTarget = nullptr;
    std::chrono::steady_clock::time_point m_startTime = std::chrono::steady_clock::now();
};

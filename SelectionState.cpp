#include "SelectionState.h"

SelectionState* SelectionState::s_instance = nullptr;

SelectionState* SelectionState::instance()
{
    if (!s_instance) {
        s_instance = new SelectionState();
    }
    return s_instance;
}

void SelectionState::setRange(qint64 startMs, qint64 endMs)
{
    if (endMs <= startMs) {
        return;
    }
    const bool changed = (startMs != m_startMs) || (endMs != m_endMs) || !m_hasRange;
    m_startMs = startMs;
    m_endMs = endMs;
    m_hasRange = true;
    if (changed) {
        emit selectionChanged(m_startMs, m_endMs, static_cast<int>(m_mode));
    }
}

void SelectionState::setRangeAndMode(qint64 startMs, qint64 endMs, Mode mode)
{
    if (endMs <= startMs) {
        return;
    }
    const bool changed = (startMs != m_startMs) || (endMs != m_endMs) || (mode != m_mode) || !m_hasRange;
    m_startMs = startMs;
    m_endMs = endMs;
    m_mode = mode;
    m_hasRange = true;
    if (changed) {
        emit selectionChanged(m_startMs, m_endMs, static_cast<int>(m_mode));
    }
}

void SelectionState::setMode(Mode mode)
{
    if (mode == m_mode) {
        return;
    }
    m_mode = mode;
    if (m_hasRange) {
        emit selectionChanged(m_startMs, m_endMs, static_cast<int>(m_mode));
    }
}

#include "SelectionState.h"

#include <QDebug>

SelectionState* SelectionState::s_instance = nullptr;

namespace {
const char* selectionModeName(SelectionState::Mode mode)
{
    return (mode == SelectionState::Review) ? "Review" : "Live";
}
} // namespace

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
        if (m_mode == Review) {
            qInfo() << "[SelectionState] setRange Review startMs=" << m_startMs
                    << "endMs=" << m_endMs << "spanMs=" << (m_endMs - m_startMs);
        }
        emit selectionChanged(m_startMs, m_endMs, static_cast<int>(m_mode));
    }
}

void SelectionState::setRangeAndMode(qint64 startMs, qint64 endMs, Mode mode)
{
    if (endMs <= startMs) {
        return;
    }
    const Mode prevMode = m_mode;
    const bool changed = (startMs != m_startMs) || (endMs != m_endMs) || (mode != m_mode) || !m_hasRange;
    m_startMs = startMs;
    m_endMs = endMs;
    m_mode = mode;
    m_hasRange = true;
    if (changed) {
        if (mode == Review || prevMode == Review || mode != prevMode) {
            qInfo() << "[SelectionState] setRangeAndMode prevMode=" << selectionModeName(prevMode)
                    << "mode=" << selectionModeName(mode)
                    << "startMs=" << m_startMs << "endMs=" << m_endMs
                    << "spanMs=" << (m_endMs - m_startMs);
        }
        emit selectionChanged(m_startMs, m_endMs, static_cast<int>(m_mode));
    }
}

void SelectionState::setMode(Mode mode)
{
    if (mode == m_mode) {
        return;
    }
    const Mode prevMode = m_mode;
    m_mode = mode;
    qInfo() << "[SelectionState] setMode" << selectionModeName(prevMode)
            << "->" << selectionModeName(mode)
            << "hasRange=" << m_hasRange
            << "startMs=" << m_startMs << "endMs=" << m_endMs;
    if (m_hasRange) {
        emit selectionChanged(m_startMs, m_endMs, static_cast<int>(m_mode));
    }
}

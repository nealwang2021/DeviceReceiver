#pragma once

#include <QString>

namespace CrashHandlerWin {

using LogCallback = void (*)(const char* utf8Message);

void setLogCallback(LogCallback callback);
void setSessionTag(const QString& sessionTag);
void installHandlers();
QString crashDirectoryPath();

} // namespace CrashHandlerWin

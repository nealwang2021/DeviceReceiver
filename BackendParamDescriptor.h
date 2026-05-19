#ifndef BACKENDPARAMDESCRIPTOR_H
#define BACKENDPARAMDESCRIPTOR_H

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

enum ParamWidgetType {
    ParamInt,        // → QSpinBox
    ParamDouble,     // → QDoubleSpinBox
    ParamEnum,       // → QComboBox
    ParamIntList,    // → QLineEdit (comma-separated ints)
};

struct BackendParamDescriptor {
    QString key;              // AppConfig key, e.g. "MultiFreq/BaseFrequencyHz"
    QString label;            // UI label, e.g. "基频(Hz)"
    ParamWidgetType type;
    QVariant defaultValue;
    double minVal = 0;
    double maxVal = 100;
    double stepVal = 1;
    QStringList enumOptions;  // for ParamEnum
};

#endif // BACKENDPARAMDESCRIPTOR_H

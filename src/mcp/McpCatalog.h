#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace TonDron::mcp {

QStringList toolboxNames();
QJsonObject catalogPayload();
QJsonObject toolboxPayload(const QString &name);
QJsonArray homepageTools();
QJsonArray toolboxDirectTools(const QString &name);
bool isHomepageTool(const QString &name);
bool isKnownOp(const QString &name);
QString toolboxForOp(const QString &name);
QString homepageHtml();
QString agentGuideText();

} // namespace TonDron::mcp

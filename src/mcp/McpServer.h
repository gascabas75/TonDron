#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <memory>

class AppController;
class QThread;

namespace TonDron::mcp {
class McpHttp;
class McpDispatcher;

class McpServer : public QObject
{
    Q_OBJECT

public:
    explicit McpServer(AppController *controller, QObject *parent = nullptr);
    ~McpServer() override;

    bool running() const { return m_running; }
    quint16 port() const { return m_port; }
    QString token() const { return m_token; }
    QString url() const;
    QString error() const { return m_error; }

    QString cursorSnippet() const;
    QString claudeCommand() const;

public slots:
    bool start();
    void stop();
    QJsonValue handleRpc(const QString &toolbox, const QJsonValue &body);

signals:
    void runningChanged();
    void errorChanged();

private:
    QString makeToken() const;
    QJsonObject dispatchTool(const QString &name, const QJsonObject &args);

    AppController *m_controller = nullptr;
    std::unique_ptr<McpDispatcher> m_dispatcher;
    QThread *m_thread = nullptr;
    McpHttp *m_http = nullptr;
    QString m_token;
    QString m_error;
    quint16 m_port = 0;
    bool m_running = false;
};

} // namespace TonDron::mcp

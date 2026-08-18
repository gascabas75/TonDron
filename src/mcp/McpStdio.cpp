#include "mcp/McpStdio.h"
#include "mcp/McpSession.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>

#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace TonDron::mcp {
namespace {

void writeStdout(const QByteArray &payload)
{
    const QByteArray header = "Content-Length: " + QByteArray::number(payload.size()) + "\r\n\r\n";
    fwrite(header.constData(), 1, static_cast<size_t>(header.size()), stdout);
    fwrite(payload.constData(), 1, static_cast<size_t>(payload.size()), stdout);
    fflush(stdout);
}

void writeRpcError(const QJsonValue &id, int code, const QString &message)
{
    const QJsonObject body{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}},
    };
    writeStdout(QJsonDocument(body).toJson(QJsonDocument::Compact));
}

QByteArray postJson(quint16 port, const QString &token, const QByteArray &body, QString *error)
{
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), port);
    if (!socket.waitForConnected(2000)) {
        if (error)
            *error = QStringLiteral(
                "Could not connect to TonDron. Is the editor open with Agent access enabled?");
        return {};
    }

    QByteArray req;
    req += "POST /mcp HTTP/1.1\r\n";
    req += "Host: 127.0.0.1\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Authorization: Bearer ";
    req += token.toUtf8();
    req += "\r\nContent-Length: ";
    req += QByteArray::number(body.size());
    req += "\r\nConnection: close\r\n\r\n";
    req += body;
    socket.write(req);
    socket.waitForBytesWritten(2000);

    QByteArray response;
    while (socket.waitForReadyRead(15000) || socket.bytesAvailable() > 0)
        response += socket.readAll();

    const int sep = response.indexOf("\r\n\r\n");
    if (sep < 0) {
        if (error)
            *error = QStringLiteral("Empty response from TonDron MCP.");
        return {};
    }
    return response.mid(sep + 4);
}

QByteArray readStdinMessage()
{
    QByteArray header;
    char ch;
    int newlines = 0;
    while (fread(&ch, 1, 1, stdin) == 1) {
        header.append(ch);
        if (ch == '\n') {
            ++newlines;
            if (header.endsWith("\r\n\r\n") || newlines >= 2)
                break;
        } else if (ch != '\r') {
            newlines = 0;
        }
        if (header.size() > 64 * 1024)
            return {};
    }
    if (header.isEmpty())
        return {};

    if (header.startsWith('{')) {
        QByteArray rest = header;
        while (!rest.contains('\n') && fread(&ch, 1, 1, stdin) == 1)
            rest.append(ch);
        return rest.trimmed();
    }

    int length = 0;
    const QByteArray lower = header.toLower();
    const int at = lower.indexOf("content-length:");
    if (at >= 0) {
        int start = at + 15;
        while (start < header.size() && (header.at(start) == ' ' || header.at(start) == '\t'))
            ++start;
        length = header.mid(start).toInt();
    }
    QByteArray body;
    body.resize(length);
    int got = 0;
    while (got < length) {
        const int n = static_cast<int>(
            fread(body.data() + got, 1, static_cast<size_t>(length - got), stdin));
        if (n <= 0)
            break;
        got += n;
    }
    body.truncate(got);
    return body;
}

} // namespace

int runStdioAttach()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    quint16 port = 0;
    QString token;
    QString error;
    if (!readSessionFile(&port, &token, &error)) {
        fprintf(stderr, "%s\n", qPrintable(error));
        writeRpcError(QJsonValue::Null, -32000, error);
        return 2;
    }

    while (!feof(stdin)) {
        const QByteArray message = readStdinMessage();
        if (message.isEmpty()) {
            if (feof(stdin))
                break;
            continue;
        }
        QString postError;
        const QByteArray reply = postJson(port, token, message, &postError);
        if (reply.isEmpty()) {
            fprintf(stderr, "%s\n", qPrintable(postError));
            writeRpcError(QJsonValue::Null, -32000, postError);
            return 3;
        }
        writeStdout(reply.trimmed());
    }
    return 0;
}

} // namespace TonDron::mcp

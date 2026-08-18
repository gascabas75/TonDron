#include <QtTest>

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include "mcp/McpCatalog.h"
#include "mcp/McpDispatcher.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpSession.h"
#include "models/AppController.h"
#include "models/AssetLibrary.h"

class McpTest : public QObject
{
    Q_OBJECT

private slots:
    void catalogListsToolboxes();
    void catalogOpsIncludeWhen();
    void toolboxDescriptionsIncludeWhen();
    void toolboxAnnotationsPresent();
    void toolboxUnknownIsError();
    void toolboxReturnsSchemas();
    void protocolInitializeAndToolsList();
    void protocolUnknownOp();
    void sessionFileRoundTrip();
    void sessionFileMissing();
    void serverRequiresBearerToken();
    void serverInitializeWithToken();
    void applyUnknownOp();
    void applyBatchStopsAndUndoRevertsPrefix();
    void inspectIsCompact();
    void inspectIncludesProjectFields();
    void placeHonorsOverlapToggle();
    void workAreaRoundTrip();
    void exportOptionsAndSettings();
    void exportVideoRequiresPath();
    void projectSetupRoundTrip();
    void captureDoesNotInsertClip();
    void inspectRevisionUnchanged();
    void listEffectsIncludesParams();
    void audioToolboxReturnsSchemas();
    void waveformReturnsPeaksOnFirstCall();
    void waveformReportsSilenceAsZero();
    void detectBeatsRejectsShortRange();
    void detectBeatsFindsClickTempoAndPublishes();
    void splitOnBeatsCutsAndUndoesAsOneStep();
    void snapClipsToBeatsRespectsMaxDistance();
    void setVolumeRoundTrips();
    void audioReadOpsAreNotUndoable();
    void armedBeatGridMakesMoveClipSnap();
};

static QJsonObject rpc(const QString &method, const QJsonObject &params = {}, int id = 1)
{
    QJsonObject o{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
    };
    if (!params.isEmpty())
        o.insert(QStringLiteral("params"), params);
    return o;
}

static QByteArray httpPost(quint16 port, const QByteArray &auth, const QByteArray &body,
                           int *statusOut = nullptr)
{
    QTcpSocket socket;
    QEventLoop loop;
    QObject::connect(&socket, &QTcpSocket::connected, &loop, &QEventLoop::quit);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    socket.connectToHost(QStringLiteral("127.0.0.1"), port);
    if (socket.state() != QAbstractSocket::ConnectedState)
        loop.exec();
    if (socket.state() != QAbstractSocket::ConnectedState)
        return {};

    QByteArray req = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n";
    if (!auth.isEmpty()) {
        req += "Authorization: ";
        req += auth;
        req += "\r\n";
    }
    req += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
    req += body;
    socket.write(req);

    QByteArray response;
    QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&]() {
        response += socket.readAll();
        if (response.contains("\r\n\r\n")) {
            const int sep = response.indexOf("\r\n\r\n");
            const QByteArray header = response.left(sep);
            const int length = [&] {
                const QByteArray lower = header.toLower();
                const int at = lower.indexOf("content-length:");
                if (at < 0)
                    return 0;
                return header.mid(at + 15).trimmed().toInt();
            }();
            if (response.size() >= sep + 4 + length)
                loop.quit();
        }
    });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    response += socket.readAll();

    const int sep = response.indexOf("\r\n\r\n");
    if (sep < 0)
        return {};
    if (statusOut) {
        const QByteArray line = response.left(response.indexOf('\n'));
        const auto parts = line.split(' ');
        *statusOut = parts.size() >= 2 ? parts.at(1).toInt() : 0;
    }
    return response.mid(sep + 4);
}

void McpTest::catalogListsToolboxes()
{
    const QJsonObject cat = TonDron::mcp::catalogPayload();
    QVERIFY(cat.value(QStringLiteral("ok")).toBool());
    const QJsonArray boxes = cat.value(QStringLiteral("toolboxes")).toArray();
    QCOMPARE(boxes.size(), 15);
    QStringList names;
    for (const QJsonValue &v : boxes)
        names.append(v.toObject().value(QStringLiteral("name")).toString());
    QVERIFY(names.contains(QStringLiteral("media")));
    QVERIFY(names.contains(QStringLiteral("timeline")));
    QVERIFY(names.contains(QStringLiteral("canvas")));
    QVERIFY(names.contains(QStringLiteral("project")));
    QVERIFY(names.contains(QStringLiteral("audio")));
}

void McpTest::catalogOpsIncludeWhen()
{
    const QJsonObject cat = TonDron::mcp::catalogPayload();
    const QJsonArray boxes = cat.value(QStringLiteral("toolboxes")).toArray();
    QVERIFY(cat.contains(QStringLiteral("endpoints")));
    QVERIFY(cat.contains(QStringLiteral("units")));
    QVERIFY(cat.contains(QStringLiteral("workflow")));
    QVERIFY(cat.contains(QStringLiteral("guide")));
    for (const QJsonValue &v : boxes) {
        const QJsonArray ops = v.toObject().value(QStringLiteral("ops")).toArray();
        QVERIFY(!ops.isEmpty());
        const QJsonObject first = ops.at(0).toObject();
        QVERIFY(first.contains(QStringLiteral("name")));
        QVERIFY(first.contains(QStringLiteral("when")));
    }
}

void McpTest::toolboxDescriptionsIncludeWhen()
{
    const QJsonObject payload = TonDron::mcp::toolboxPayload(QStringLiteral("timeline"));
    const QJsonArray tools = payload.value(QStringLiteral("tools")).toArray();
    bool sawAddTrack = false;
    for (const QJsonValue &v : tools) {
        const QJsonObject tool = v.toObject();
        if (tool.value(QStringLiteral("name")).toString() == QLatin1String("add_track")) {
            sawAddTrack = true;
            QVERIFY(tool.value(QStringLiteral("description")).toString().startsWith(QStringLiteral("When:")));
            const QJsonObject type =
                tool.value(QStringLiteral("inputSchema")).toObject()
                    .value(QStringLiteral("properties")).toObject()
                    .value(QStringLiteral("type")).toObject();
            QVERIFY(type.contains(QStringLiteral("enum")));
        }
    }
    QVERIFY(sawAddTrack);
}

void McpTest::toolboxAnnotationsPresent()
{
    const QJsonObject payload = TonDron::mcp::toolboxPayload(QStringLiteral("media"));
    const QJsonArray tools = payload.value(QStringLiteral("tools")).toArray();
    bool sawList = false;
    bool sawRename = false;
    for (const QJsonValue &v : tools) {
        const QJsonObject tool = v.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString();
        if (name == QLatin1String("list_assets")) {
            sawList = true;
            const QJsonObject ann = tool.value(QStringLiteral("annotations")).toObject();
            QVERIFY(ann.value(QStringLiteral("readOnlyHint")).toBool());
            QVERIFY(ann.value(QStringLiteral("idempotentHint")).toBool());
        }
        if (name == QLatin1String("rename_asset"))
            sawRename = true;
    }
    QVERIFY(sawList);
    QVERIFY(sawRename);
    QVERIFY(TonDron::mcp::toolboxNames().contains(QStringLiteral("project")));
}

void McpTest::toolboxUnknownIsError()
{
    const QJsonObject payload = TonDron::mcp::toolboxPayload(QStringLiteral("nope"));
    QCOMPARE(payload.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(payload.value(QStringLiteral("error")).toString(), QStringLiteral("unknown_toolbox"));
}

void McpTest::toolboxReturnsSchemas()
{
    const QJsonObject payload = TonDron::mcp::toolboxPayload(QStringLiteral("timeline"));
    QVERIFY(payload.value(QStringLiteral("ok")).toBool());
    const QJsonArray tools = payload.value(QStringLiteral("tools")).toArray();
    QVERIFY(tools.size() >= 8);
    bool sawPlace = false;
    for (const QJsonValue &v : tools) {
        if (v.toObject().value(QStringLiteral("name")).toString() == QLatin1String("place_clip")) {
            sawPlace = true;
            QVERIFY(v.toObject().contains(QStringLiteral("inputSchema")));
        }
    }
    QVERIFY(sawPlace);
}

void McpTest::protocolInitializeAndToolsList()
{
    const QJsonValue init = TonDron::mcp::handleJsonRpc(rpc(QStringLiteral("initialize")), {}, {});
    QVERIFY(init.isObject());
    QCOMPARE(init.toObject().value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("serverInfo")).toObject()
                 .value(QStringLiteral("name")).toString(),
             QStringLiteral("drift"));

    const QJsonValue listed = TonDron::mcp::handleJsonRpc(rpc(QStringLiteral("tools/list")), {}, {});
    const QJsonArray tools =
        listed.toObject().value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    QCOMPARE(tools.size(), 5);
}

void McpTest::protocolUnknownOp()
{
    bool called = false;
    const QJsonValue reply = TonDron::mcp::handleJsonRpc(
        rpc(QStringLiteral("tools/call"),
            {{QStringLiteral("name"), QStringLiteral("not_a_tool")},
             {QStringLiteral("arguments"), QJsonObject{}}}),
        {},
        [&](const QString &, const QJsonObject &) {
            called = true;
            return QJsonObject{};
        });
    QVERIFY(!called);
    const QJsonArray content = reply.toObject()
                                   .value(QStringLiteral("result"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    QVERIFY(!content.isEmpty());
    const auto payload = QJsonDocument::fromJson(
                             content.at(0).toObject().value(QStringLiteral("text")).toString().toUtf8())
                             .object();
    QCOMPARE(payload.value(QStringLiteral("error")).toString(), QStringLiteral("unknown_op"));
}

void McpTest::sessionFileRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("session.json"));
    qputenv("DRIFT_MCP_SESSION_PATH", path.toUtf8());
    QVERIFY(TonDron::mcp::writeSessionFile(4731, QStringLiteral("abc123")));
    quint16 port = 0;
    QString token;
    QString error;
    QVERIFY(TonDron::mcp::readSessionFile(&port, &token, &error));
    QCOMPARE(port, quint16(4731));
    QCOMPARE(token, QStringLiteral("abc123"));
    TonDron::mcp::removeSessionFile();
    QVERIFY(!QFile::exists(path));
    qunsetenv("DRIFT_MCP_SESSION_PATH");
}

void McpTest::sessionFileMissing()
{
    qputenv("DRIFT_MCP_SESSION_PATH", "/tmp/drift-mcp-does-not-exist-test.json");
    QString error;
    QVERIFY(!TonDron::mcp::readSessionFile(nullptr, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("Agent access")));
    qunsetenv("DRIFT_MCP_SESSION_PATH");
}

void McpTest::serverRequiresBearerToken()
{
    QTemporaryDir dir;
    qputenv("DRIFT_MCP_SESSION_PATH", dir.filePath(QStringLiteral("s.json")).toUtf8());
    AssetLibrary library;
    AppController state(&library);
    state.setMcpEnabled(true);
    QVERIFY2(state.mcpRunning(), qPrintable(state.mcpError()));
    int status = 0;
    httpPost(quint16(state.mcpPort()), {},
             QJsonDocument(rpc(QStringLiteral("ping"))).toJson(QJsonDocument::Compact), &status);
    QCOMPARE(status, 401);
    state.setMcpEnabled(false);
    QVERIFY(!state.mcpRunning());
    qunsetenv("DRIFT_MCP_SESSION_PATH");
}

void McpTest::serverInitializeWithToken()
{
    QTemporaryDir dir;
    qputenv("DRIFT_MCP_SESSION_PATH", dir.filePath(QStringLiteral("s.json")).toUtf8());
    AssetLibrary library;
    AppController state(&library);
    state.setMcpEnabled(true);
    QVERIFY(state.mcpRunning());
    int status = 0;
    const QByteArray auth = "Bearer " + state.mcpToken().toUtf8();
    const QByteArray body = httpPost(
        quint16(state.mcpPort()), auth,
        QJsonDocument(rpc(QStringLiteral("initialize"))).toJson(QJsonDocument::Compact), &status);
    QCOMPARE(status, 200);
    const auto doc = QJsonDocument::fromJson(body);
    QCOMPARE(doc.object().value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("serverInfo")).toObject()
                 .value(QStringLiteral("name")).toString(),
             QStringLiteral("drift"));
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("s.json"))));
    state.setMcpEnabled(false);
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("s.json"))));
    qunsetenv("DRIFT_MCP_SESSION_PATH");
}

void McpTest::applyUnknownOp()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    const QJsonObject result =
        dispatcher.applyOne(QStringLiteral("not_real"), {});
    QCOMPARE(result.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(result.value(QStringLiteral("error")).toString(), QStringLiteral("unknown_op"));
}

void McpTest::applyBatchStopsAndUndoRevertsPrefix()
{
    AssetLibrary library;
    AppController state(&library);
    state.addTextClip(QStringLiteral("Hello"), 0.0);
    const int track = state.selectedTrack();
    const int clip = state.selectedClip();
    QVERIFY(track >= 0);
    const QString id = state.mcpCompactClip(track, clip).value(QStringLiteral("id")).toString();
    QVERIFY(!id.isEmpty());

    TonDron::mcp::McpDispatcher dispatcher(&state);
    const QJsonObject batch = dispatcher.apply(QJsonObject{
        {QStringLiteral("ops"),
         QJsonArray{
             QJsonObject{{QStringLiteral("tool"), QStringLiteral("set_duration")},
                         {QStringLiteral("args"),
                          QJsonObject{{QStringLiteral("clip"), id},
                                      {QStringLiteral("duration"), 2.0}}}},
             QJsonObject{{QStringLiteral("tool"), QStringLiteral("not_real")},
                         {QStringLiteral("args"), QJsonObject{}}},
         }},
    });
    QCOMPARE(batch.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(batch.value(QStringLiteral("error")).toString(), QStringLiteral("apply_failed"));
    QCOMPARE(batch.value(QStringLiteral("stopped")).toInt(), 1);
    QCOMPARE(state.mcpCompactClip(track, clip).value(QStringLiteral("duration")).toDouble(), 2.0);

    QVERIFY(state.undoAvailable());
    state.undo();
    QVERIFY(state.mcpCompactClip(track, clip).value(QStringLiteral("duration")).toDouble() > 2.0);
}

void McpTest::inspectIsCompact()
{
    AssetLibrary library;
    AppController state(&library);
    state.addTextClip(QStringLiteral("Hello"), 0.0);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    const QJsonObject summary = dispatcher.inspect({});
    QVERIFY(summary.value(QStringLiteral("ok")).toBool());
    QVERIFY(summary.contains(QStringLiteral("tracks")));
    QVERIFY(summary.contains(QStringLiteral("overlap")));
    QCOMPARE(summary.value(QStringLiteral("overlap")).toBool(), false);
    QVERIFY(!summary.contains(QStringLiteral("work_in")));
    QVERIFY(!summary.toVariantMap().contains(QStringLiteral("effects")));
    QCOMPARE(summary.value(QStringLiteral("path")).toString(), QString());
    QCOMPARE(summary.value(QStringLiteral("dirty")).toBool(), true);
    QVERIFY(summary.contains(QStringLiteral("background")));
    QVERIFY(summary.contains(QStringLiteral("export")));
    const QJsonObject withClips = dispatcher.inspect({{QStringLiteral("clips"), true}});
    const QJsonArray tracks = withClips.value(QStringLiteral("tracks")).toArray();
    QVERIFY(!tracks.isEmpty());
    QVERIFY(tracks.at(0).toObject().contains(QStringLiteral("items")));
}

void McpTest::inspectIncludesProjectFields()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    const QJsonObject inspect = dispatcher.inspect({});
    QVERIFY(inspect.contains(QStringLiteral("path")));
    QVERIFY(inspect.contains(QStringLiteral("dirty")));
    QVERIFY(inspect.contains(QStringLiteral("background")));
    const QJsonObject exportState = inspect.value(QStringLiteral("export")).toObject();
    QVERIFY(exportState.contains(QStringLiteral("active")));
    QVERIFY(exportState.contains(QStringLiteral("progress")));
}

void McpTest::placeHonorsOverlapToggle()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QJsonObject first = dispatcher.applyOne(
        QStringLiteral("add_text"),
        {{QStringLiteral("text"), QStringLiteral("A")}, {QStringLiteral("at"), 0.0}});
    QVERIFY(first.value(QStringLiteral("ok")).toBool());

    const QJsonObject gapped = dispatcher.applyOne(
        QStringLiteral("add_text"),
        {{QStringLiteral("text"), QStringLiteral("B")}, {QStringLiteral("at"), 1.0}});
    QVERIFY(gapped.value(QStringLiteral("ok")).toBool());
    QVERIFY(gapped.value(QStringLiteral("start")).toDouble() > 4.0);

    QVERIFY(state.undoAvailable());
    state.undo();

    const QJsonObject overlapOn = dispatcher.applyOne(
        QStringLiteral("set_overlap"), {{QStringLiteral("enabled"), true}});
    QVERIFY(overlapOn.value(QStringLiteral("ok")).toBool());
    QCOMPARE(overlapOn.value(QStringLiteral("overlap")).toBool(), true);

    const QJsonObject overlapping = dispatcher.applyOne(
        QStringLiteral("add_text"),
        {{QStringLiteral("text"), QStringLiteral("B")}, {QStringLiteral("at"), 1.0}});
    QVERIFY(overlapping.value(QStringLiteral("ok")).toBool());
    QCOMPARE(overlapping.value(QStringLiteral("start")).toDouble(), 1.0);
    QVERIFY(!overlapping.contains(QStringLiteral("reason")));
    QCOMPARE(dispatcher.inspect({}).value(QStringLiteral("overlap")).toBool(), true);
}

void McpTest::workAreaRoundTrip()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QJsonObject bad = dispatcher.applyOne(
        QStringLiteral("set_work_area"),
        {{QStringLiteral("in"), 5.0}, {QStringLiteral("out"), 1.0}});
    QCOMPARE(bad.value(QStringLiteral("ok")).toBool(), false);

    const QJsonObject set = dispatcher.applyOne(
        QStringLiteral("set_work_area"),
        {{QStringLiteral("in"), 1.5}, {QStringLiteral("out"), 4.0}});
    QVERIFY(set.value(QStringLiteral("ok")).toBool());
    QCOMPARE(set.value(QStringLiteral("work_in")).toDouble(), 1.5);
    QCOMPARE(set.value(QStringLiteral("work_out")).toDouble(), 4.0);

    const QJsonObject inspect = dispatcher.inspect({});
    QCOMPARE(inspect.value(QStringLiteral("work_in")).toDouble(), 1.5);
    QCOMPARE(inspect.value(QStringLiteral("work_out")).toDouble(), 4.0);

    const QJsonObject cleared = dispatcher.applyOne(QStringLiteral("clear_work_area"), {});
    QVERIFY(cleared.value(QStringLiteral("ok")).toBool());
    QVERIFY(!dispatcher.inspect({}).contains(QStringLiteral("work_in")));
}

void McpTest::exportOptionsAndSettings()
{
    QStandardPaths::setTestModeEnabled(true);
    const QString org = QCoreApplication::organizationName();
    const QString app = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("DriftMcpTest"));
    QCoreApplication::setApplicationName(QStringLiteral("DriftMcpTest"));
    const auto restore = qScopeGuard([&] {
        QSettings().remove(QStringLiteral("export"));
        QCoreApplication::setOrganizationName(org);
        QCoreApplication::setApplicationName(app);
        QStandardPaths::setTestModeEnabled(false);
    });
    QSettings().remove(QStringLiteral("export"));

    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QJsonObject options = dispatcher.applyOne(QStringLiteral("list_export_options"), {});
    QVERIFY(options.value(QStringLiteral("ok")).toBool());
    QVERIFY(!options.value(QStringLiteral("video")).toArray().isEmpty());
    QVERIFY(!options.value(QStringLiteral("fps")).toArray().isEmpty());
    QVERIFY(options.contains(QStringLiteral("gif")));
}

void McpTest::exportVideoRequiresPath()
{
    QStandardPaths::setTestModeEnabled(true);
    const QString org = QCoreApplication::organizationName();
    const QString app = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("DriftMcpTest"));
    QCoreApplication::setApplicationName(QStringLiteral("DriftMcpTest"));
    const auto restore = qScopeGuard([&] {
        QSettings().remove(QStringLiteral("export"));
        QCoreApplication::setOrganizationName(org);
        QCoreApplication::setApplicationName(app);
        QStandardPaths::setTestModeEnabled(false);
    });
    QSettings().remove(QStringLiteral("export"));

    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QJsonObject missing = dispatcher.applyOne(QStringLiteral("export_video"), {});
    QCOMPARE(missing.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(missing.value(QStringLiteral("error")).toString(), QStringLiteral("bad_args"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString outPath = dir.filePath(QStringLiteral("out.mp4"));
    const QJsonObject started = dispatcher.applyOne(
        QStringLiteral("export_video"), {{QStringLiteral("path"), outPath}});
    if (started.value(QStringLiteral("ok")).toBool()) {
        QVERIFY(started.value(QStringLiteral("started")).toBool());
        const QJsonObject status = dispatcher.applyOne(QStringLiteral("export_status"), {});
        QVERIFY(status.contains(QStringLiteral("busy")));
        QVERIFY(status.contains(QStringLiteral("progress")));
    }
}

void McpTest::projectSetupRoundTrip()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QJsonObject setup = dispatcher.applyOne(
        QStringLiteral("set_project_setup"),
        {{QStringLiteral("width"), 1280}, {QStringLiteral("height"), 720}, {QStringLiteral("fps"), 30}});
    QVERIFY(setup.value(QStringLiteral("ok")).toBool());
    QCOMPARE(setup.value(QStringLiteral("w")).toInt(), 1280);
    QCOMPARE(setup.value(QStringLiteral("h")).toInt(), 720);
    QCOMPARE(setup.value(QStringLiteral("fps")).toInt(), 30);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("test.dcut"));
    const QJsonObject saved = dispatcher.applyOne(
        QStringLiteral("save_project"), {{QStringLiteral("path"), path}});
    QVERIFY(saved.value(QStringLiteral("ok")).toBool());
    QVERIFY(QFile::exists(path));
}

void McpTest::captureDoesNotInsertClip()
{
    AssetLibrary library;
    AppController state(&library);
    state.addTextClip(QStringLiteral("Hello"), 0.0);
    auto clipCount = [](AppController &s) {
        int n = 0;
        for (const QVariant &t : s.tracks())
            n += t.toMap().value(QStringLiteral("clips")).toList().size();
        return n;
    };
    const int before = clipCount(state);
    const QJsonObject result = state.mcpCaptureFrame(-1.0, false);
    QCOMPARE(clipCount(state), before);
    if (result.value(QStringLiteral("isError")).toBool())
        QSKIP("Compositor could not produce a frame in this environment");
    QVERIFY(result.value(QStringLiteral("content")).toArray().size() >= 1);
}

void McpTest::inspectRevisionUnchanged()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    const QJsonObject first = dispatcher.inspect({});
    QVERIFY(first.value(QStringLiteral("ok")).toBool());
    QVERIFY(first.contains(QStringLiteral("revision")));
    const int revision = first.value(QStringLiteral("revision")).toInt();
    const QJsonObject unchanged = dispatcher.inspect({{QStringLiteral("since"), revision}});
    QVERIFY(unchanged.value(QStringLiteral("ok")).toBool());
    QCOMPARE(unchanged.value(QStringLiteral("unchanged")).toBool(), true);
    QCOMPARE(unchanged.value(QStringLiteral("revision")).toInt(), revision);
}

void McpTest::listEffectsIncludesParams()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    const QJsonObject result = dispatcher.applyOne(QStringLiteral("list_effects"), {});
    QVERIFY(result.value(QStringLiteral("ok")).toBool());
    const QJsonArray effects = result.value(QStringLiteral("effects")).toArray();
    QVERIFY(!effects.isEmpty());
    bool sawParams = false;
    for (const QJsonValue &v : effects) {
        const QJsonArray params = v.toObject().value(QStringLiteral("params")).toArray();
        if (!params.isEmpty()) {
            sawParams = true;
            QVERIFY(params.at(0).toObject().contains(QStringLiteral("key")));
            break;
        }
    }
    QVERIFY(sawParams);
}

// --- audio -------------------------------------------------------------------------------
//
// The audio ops need real PCM, so these generate fixtures with the ffmpeg CLI the same way
// tst_editorstate does, and skip when it is not installed.

namespace {

QString ffmpegPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

bool runFfmpeg(const QStringList &args)
{
    QProcess proc;
    proc.start(ffmpegPath(), QStringList{QStringLiteral("-y")} + args);
    return proc.waitForFinished(60000) && proc.exitCode() == 0;
}

// Broadband noise bursts every 0.5 s — 120 BPM, and broadband so every FFT bin jumps at once,
// which is what the spectral-flux detector keys on.
bool writeClickTrack(const QString &path, int seconds)
{
    return runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                      QStringLiteral("aevalsrc=0.9*random(0)*exp(-mod(t\\,0.5)*80):s=48000:d=%1")
                          .arg(seconds),
                      QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), path});
}

// Two seconds of tone then two of digital silence, concatenated rather than gated so the tail
// is genuinely zero. The gain is there because ffmpeg's sine filter emits at -18 dBFS (0.125
// linear); these peaks are linear, so without it "loud" and "quiet" would be a factor of 8
// apart instead of the full scale the assertions read as.
bool writeHalfSilentTone(const QString &path)
{
    return runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                      QStringLiteral("sine=frequency=440:sample_rate=48000:duration=2"),
                      QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                      QStringLiteral("anullsrc=r=48000:cl=mono:d=2"),
                      QStringLiteral("-filter_complex"),
                      QStringLiteral("[0:a]volume=8[loud];[loud][1:a]concat=n=2:v=0:a=1[out]"),
                      QStringLiteral("-map"), QStringLiteral("[out]"), QStringLiteral("-c:a"),
                      QStringLiteral("pcm_s16le"), path});
}

// Imports `path` and drops it on the timeline at `at`, returning the new clip's UUID.
QString importAndPlace(TonDron::mcp::McpDispatcher &dispatcher, const QString &path, double at)
{
    const QJsonObject imported = dispatcher.applyOne(
        QStringLiteral("import_media"),
        {{QStringLiteral("paths"), QJsonArray{path}}});
    if (!imported.value(QStringLiteral("ok")).toBool())
        return {};

    const QJsonArray assets = imported.value(QStringLiteral("assets")).toArray();
    if (assets.isEmpty())
        return {};
    const QString assetId = assets.at(0).toObject().value(QStringLiteral("id")).toString();

    const QJsonObject placed = dispatcher.applyOne(
        QStringLiteral("place_clip"),
        {{QStringLiteral("asset"), assetId}, {QStringLiteral("at"), at}});
    if (!placed.value(QStringLiteral("ok")).toBool())
        return {};
    return placed.value(QStringLiteral("id")).toString();
}

} // namespace

void McpTest::audioToolboxReturnsSchemas()
{
    const QJsonObject payload = TonDron::mcp::toolboxPayload(QStringLiteral("audio"));
    QVERIFY(payload.value(QStringLiteral("ok")).toBool());
    const QJsonArray tools = payload.value(QStringLiteral("tools")).toArray();
    QCOMPARE(tools.size(), 8);

    QStringList names;
    for (const QJsonValue &v : tools) {
        const QJsonObject tool = v.toObject();
        names.append(tool.value(QStringLiteral("name")).toString());
        QVERIFY(tool.contains(QStringLiteral("inputSchema")));
    }
    QVERIFY(names.contains(QStringLiteral("get_waveform")));
    QVERIFY(names.contains(QStringLiteral("detect_beats")));
    QVERIFY(names.contains(QStringLiteral("split_on_beats")));
    QVERIFY(names.contains(QStringLiteral("snap_clips_to_beats")));
    QVERIFY(names.contains(QStringLiteral("set_volume")));

    // detect_beats is on /mcp/audio, not the homepage.
    QCOMPARE(TonDron::mcp::toolboxForOp(QStringLiteral("detect_beats")), QStringLiteral("audio"));
}

// The whole reason these ops call the engine directly instead of the QML getters: those come
// back empty the first time and repaint on a signal, which an agent never sees.
void McpTest::waveformReturnsPeaksOnFirstCall()
{
    if (ffmpegPath().isEmpty())
        QSKIP("ffmpeg not available to generate a test clip");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("tone.wav"));
    QVERIFY(writeHalfSilentTone(source));

    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QString clip = importAndPlace(dispatcher, source, 0.0);
    QVERIFY(!clip.isEmpty());

    const QJsonObject result = dispatcher.applyOne(
        QStringLiteral("get_waveform"),
        {{QStringLiteral("clip"), clip}, {QStringLiteral("buckets"), 64}});
    QVERIFY2(result.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    QCOMPARE(result.value(QStringLiteral("source")).toString(), QStringLiteral("clip"));

    const QJsonArray peaks = result.value(QStringLiteral("peaks")).toArray();
    QCOMPARE(peaks.size(), 64);
    for (const QJsonValue &v : peaks)
        QVERIFY(v.toDouble() >= 0.0 && v.toDouble() <= 1.0);
    QVERIFY2(result.value(QStringLiteral("max")).toDouble() > 0.2,
             qPrintable(QString::number(result.value(QStringLiteral("max")).toDouble())));
}

// reduceDensePeaks floors silence at 0.05 so a quiet lane still draws; these peaks must not,
// or "is this stretch empty" becomes unanswerable.
void McpTest::waveformReportsSilenceAsZero()
{
    if (ffmpegPath().isEmpty())
        QSKIP("ffmpeg not available to generate a test clip");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("half-tone.wav"));
    QVERIFY(writeHalfSilentTone(source));

    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    QVERIFY(!importAndPlace(dispatcher, source, 0.0).isEmpty());

    const QString assetId = state.mcpInspect(false)
                                .value(QStringLiteral("assets")).toArray().at(0).toObject()
                                .value(QStringLiteral("id")).toString();
    QVERIFY(!assetId.isEmpty());

    const QJsonObject loud = dispatcher.applyOne(
        QStringLiteral("get_waveform"),
        {{QStringLiteral("asset"), assetId}, {QStringLiteral("start"), 0.0},
         {QStringLiteral("duration"), 1.5}, {QStringLiteral("buckets"), 16}});
    QVERIFY2(loud.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(loud).toJson(QJsonDocument::Compact)));
    QVERIFY2(loud.value(QStringLiteral("max")).toDouble() > 0.2,
             qPrintable(QJsonDocument(loud).toJson(QJsonDocument::Compact)));

    const QJsonObject quiet = dispatcher.applyOne(
        QStringLiteral("get_waveform"),
        {{QStringLiteral("asset"), assetId}, {QStringLiteral("start"), 2.5},
         {QStringLiteral("duration"), 1.0}, {QStringLiteral("buckets"), 16}});
    QVERIFY(quiet.value(QStringLiteral("ok")).toBool());
    QCOMPARE(quiet.value(QStringLiteral("max")).toDouble(), 0.0);
}

void McpTest::detectBeatsRejectsShortRange()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QJsonObject tooShort = dispatcher.applyOne(
        QStringLiteral("detect_beats"), {{QStringLiteral("duration"), 1.0}});
    QCOMPARE(tooShort.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(tooShort.value(QStringLiteral("error")).toString(), QStringLiteral("bad_args"));

    const QJsonObject tooLong = dispatcher.applyOne(
        QStringLiteral("detect_beats"), {{QStringLiteral("duration"), 5000.0}});
    QCOMPARE(tooLong.value(QStringLiteral("error")).toString(), QStringLiteral("bad_args"));

    const QJsonObject missing = dispatcher.applyOne(QStringLiteral("detect_beats"), {});
    QCOMPARE(missing.value(QStringLiteral("error")).toString(), QStringLiteral("bad_args"));
}

void McpTest::detectBeatsFindsClickTempoAndPublishes()
{
    if (ffmpegPath().isEmpty())
        QSKIP("ffmpeg not available to generate a test clip");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("clicks.wav"));
    QVERIFY(writeClickTrack(source, 12));

    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    QVERIFY(!importAndPlace(dispatcher, source, 0.0).isEmpty());

    const QJsonObject beats = dispatcher.applyOne(
        QStringLiteral("detect_beats"),
        {{QStringLiteral("start"), 0.0}, {QStringLiteral("duration"), 10.0}});
    QVERIFY2(beats.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(beats).toJson(QJsonDocument::Compact)));

    const double bpm = beats.value(QStringLiteral("bpm")).toDouble();
    QVERIFY2(std::abs(bpm - 120.0) < 3.0, qPrintable(QStringLiteral("bpm %1").arg(bpm)));
    QVERIFY(!beats.value(QStringLiteral("beats")).toArray().isEmpty());
    QVERIFY(!beats.value(QStringLiteral("onsets")).toArray().isEmpty());
    QCOMPARE(beats.value(QStringLiteral("cached")).toBool(), false);

    // The same range comes back from cache rather than re-mixing.
    const QJsonObject again = dispatcher.applyOne(
        QStringLiteral("detect_beats"),
        {{QStringLiteral("start"), 0.0}, {QStringLiteral("duration"), 10.0}});
    QCOMPARE(again.value(QStringLiteral("cached")).toBool(), true);

    // And the editor sees the same analysis the agent got.
    const QJsonObject detail = dispatcher.inspect({{QStringLiteral("detail"), true}});
    const QJsonObject state_ = detail.value(QStringLiteral("beats")).toObject();
    QCOMPARE(state_.value(QStringLiteral("analysed")).toBool(), true);
    QCOMPARE(state_.value(QStringLiteral("stale")).toBool(), false);
    QVERIFY(std::abs(state_.value(QStringLiteral("bpm")).toDouble() - bpm) < 0.5);

    // Arming the layers is what turns beats into snap targets.
    const QJsonObject layers = dispatcher.applyOne(
        QStringLiteral("set_beat_layers"), {{QStringLiteral("grid"), true}});
    QVERIFY(layers.value(QStringLiteral("ok")).toBool());
    QCOMPARE(layers.value(QStringLiteral("gridVisible")).toBool(), true);
    QVERIFY(layers.value(QStringLiteral("snapTargets")).toInt() > 0);
}

void McpTest::splitOnBeatsCutsAndUndoesAsOneStep()
{
    if (ffmpegPath().isEmpty())
        QSKIP("ffmpeg not available to generate a test clip");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("clicks.wav"));
    QVERIFY(writeClickTrack(source, 12));

    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    const QString clip = importAndPlace(dispatcher, source, 0.0);
    QVERIFY(!clip.isEmpty());

    const QJsonObject beats = dispatcher.applyOne(
        QStringLiteral("detect_beats"),
        {{QStringLiteral("start"), 0.0}, {QStringLiteral("duration"), 10.0}});
    QVERIFY(beats.value(QStringLiteral("ok")).toBool());
    if (beats.value(QStringLiteral("beats")).toArray().isEmpty())
        QSKIP("no tempo found in the generated click track");

    const int clipsBefore = dispatcher.inspect({}).value(QStringLiteral("clips")).toInt();

    const QJsonObject split = dispatcher.applyOne(
        QStringLiteral("split_on_beats"),
        {{QStringLiteral("clip"), clip}, {QStringLiteral("unit"), QStringLiteral("bar")}});
    QVERIFY2(split.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(split).toJson(QJsonDocument::Compact)));

    const QJsonArray produced = split.value(QStringLiteral("clips")).toArray();
    QVERIFY(produced.size() > 1);
    QCOMPARE(produced.at(0).toString(), clip); // the original id names the first piece
    for (const QJsonValue &v : produced)
        QVERIFY(state.mcpLocateClip(v.toString()).first >= 0);
    QCOMPARE(dispatcher.inspect({}).value(QStringLiteral("clips")).toInt(),
             clipsBefore + produced.size() - 1);

    // However many cuts it made, it is one step.
    QVERIFY(state.undoAvailable());
    state.undo();
    QCOMPARE(dispatcher.inspect({}).value(QStringLiteral("clips")).toInt(), clipsBefore);
}

void McpTest::snapClipsToBeatsRespectsMaxDistance()
{
    if (ffmpegPath().isEmpty())
        QSKIP("ffmpeg not available to generate a test clip");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("clicks.wav"));
    QVERIFY(writeClickTrack(source, 12));

    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    QVERIFY(!importAndPlace(dispatcher, source, 0.0).isEmpty());

    const QJsonObject beats = dispatcher.applyOne(
        QStringLiteral("detect_beats"),
        {{QStringLiteral("start"), 0.0}, {QStringLiteral("duration"), 10.0}});
    QVERIFY(beats.value(QStringLiteral("ok")).toBool());
    if (beats.value(QStringLiteral("beats")).toArray().isEmpty())
        QSKIP("no tempo found in the generated click track");

    // A title on its own lane, deliberately nowhere near a beat.
    const QJsonObject text = dispatcher.applyOne(
        QStringLiteral("add_text"),
        {{QStringLiteral("text"), QStringLiteral("A")}, {QStringLiteral("at"), 20.0}});
    QVERIFY(text.value(QStringLiteral("ok")).toBool());
    const QString title = text.value(QStringLiteral("id")).toString();

    const QJsonObject snapped = dispatcher.applyOne(
        QStringLiteral("snap_clips_to_beats"),
        {{QStringLiteral("clips"), QJsonArray{title}}, {QStringLiteral("max_distance"), 0.25}});
    QVERIFY(snapped.value(QStringLiteral("ok")).toBool());
    QCOMPARE(snapped.value(QStringLiteral("moved")).toArray().size(), 0);

    const QJsonArray skipped = snapped.value(QStringLiteral("skipped")).toArray();
    QCOMPARE(skipped.size(), 1);
    QCOMPARE(skipped.at(0).toObject().value(QStringLiteral("reason")).toString(),
             QStringLiteral("too_far"));

    // With a wide enough window it does move, and reports where it actually landed.
    const QJsonObject wide = dispatcher.applyOne(
        QStringLiteral("snap_clips_to_beats"),
        {{QStringLiteral("clips"), QJsonArray{title}}, {QStringLiteral("max_distance"), 60.0}});
    QVERIFY(wide.value(QStringLiteral("ok")).toBool());
    const QJsonArray moved = wide.value(QStringLiteral("moved")).toArray();
    QCOMPARE(moved.size(), 1);
    QVERIFY(moved.at(0).toObject().contains(QStringLiteral("to")));
}

void McpTest::setVolumeRoundTrips()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QJsonObject text = dispatcher.applyOne(
        QStringLiteral("add_text"),
        {{QStringLiteral("text"), QStringLiteral("A")}, {QStringLiteral("at"), 0.0}});
    QVERIFY(text.value(QStringLiteral("ok")).toBool());
    const QString clip = text.value(QStringLiteral("id")).toString();

    const QJsonObject set = dispatcher.applyOne(
        QStringLiteral("set_volume"),
        {{QStringLiteral("clip"), clip}, {QStringLiteral("value"), 0.5}});
    QVERIFY2(set.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(set).toJson(QJsonDocument::Compact)));
    QCOMPARE(set.value(QStringLiteral("value")).toDouble(), 0.5);
    QCOMPARE(set.value(QStringLiteral("volumeKeys")).toInt(), 1);

    const QJsonObject keys = dispatcher.applyOne(
        QStringLiteral("list_keyframes"),
        {{QStringLiteral("clip"), clip}, {QStringLiteral("prop"), QStringLiteral("volume")}});
    QVERIFY(keys.value(QStringLiteral("ok")).toBool());
    const QJsonArray points = keys.value(QStringLiteral("keys")).toArray();
    QCOMPARE(points.size(), 1);
    QCOMPARE(points.at(0).toObject().value(QStringLiteral("value")).toDouble(), 0.5);

    // Out of range is clamped to what the mixer will actually honour, and says so.
    const QJsonObject loud = dispatcher.applyOne(
        QStringLiteral("set_volume"),
        {{QStringLiteral("clip"), clip}, {QStringLiteral("value"), 9.0}});
    QCOMPARE(loud.value(QStringLiteral("value")).toDouble(), 2.0);
    QCOMPARE(loud.value(QStringLiteral("clamped")).toBool(), true);
}

void McpTest::audioReadOpsAreNotUndoable()
{
    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);

    const QJsonObject result = dispatcher.apply(
        {{QStringLiteral("ops"),
          QJsonArray{QJsonObject{{QStringLiteral("tool"), QStringLiteral("audio_summary")}},
                     QJsonObject{{QStringLiteral("tool"), QStringLiteral("set_beat_layers")},
                                 {QStringLiteral("args"),
                                  QJsonObject{{QStringLiteral("grid"), true}}}}}}});
    QVERIFY2(result.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    QVERIFY(!state.undoAvailable());
}

// The payoff of the whole toolbox: extraSnapTargets() already feeds TonDron::snapTime, so arming
// the grid makes every ordinary placement op quantise without asking for it. Nothing in the
// audio ops themselves would fail if this stopped working, so it needs its own test.
void McpTest::armedBeatGridMakesMoveClipSnap()
{
    if (ffmpegPath().isEmpty())
        QSKIP("ffmpeg not available to generate a test clip");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("clicks.wav"));
    QVERIFY(writeClickTrack(source, 12));

    AssetLibrary library;
    AppController state(&library);
    TonDron::mcp::McpDispatcher dispatcher(&state);
    QVERIFY(!importAndPlace(dispatcher, source, 0.0).isEmpty());

    const QJsonObject beats = dispatcher.applyOne(
        QStringLiteral("detect_beats"),
        {{QStringLiteral("start"), 0.0}, {QStringLiteral("duration"), 10.0}});
    QVERIFY(beats.value(QStringLiteral("ok")).toBool());
    const QJsonArray grid = beats.value(QStringLiteral("beats")).toArray();
    if (grid.isEmpty())
        QSKIP("no tempo found in the generated click track");

    // Two beats, seconds apart. They have to be different targets: snapTime does not exclude the
    // clip being moved, so aiming the armed move at where the unarmed one already parked the
    // clip would snap it to itself at distance 0 and prove nothing.
    //
    // Mid-range, so neither is near the targets snapTime always has (0, the playhead, and every
    // clip edge). The title goes on its own text track, so the music clip is not in its way.
    double unarmedBeat = 0.0;
    double beat = 0.0;
    for (const QJsonValue &v : grid) {
        const double t = v.toDouble();
        if (unarmedBeat <= 0.0 && t > 3.0)
            unarmedBeat = t;
        else if (unarmedBeat > 0.0 && t > unarmedBeat + 2.0) {
            beat = t;
            break;
        }
    }
    if (unarmedBeat <= 0.0 || beat <= 0.0)
        QSKIP("no usable pair of beats in the analysed range");

    const QJsonObject text = dispatcher.applyOne(
        QStringLiteral("add_text"),
        {{QStringLiteral("text"), QStringLiteral("A")}, {QStringLiteral("at"), 30.0}});
    QVERIFY(text.value(QStringLiteral("ok")).toBool());
    const QString title = text.value(QStringLiteral("id")).toString();

    // Grid off: the clip lands exactly where it was told, 40 ms off the beat.
    const QJsonObject unarmed = dispatcher.applyOne(
        QStringLiteral("move_clip"),
        {{QStringLiteral("clip"), title}, {QStringLiteral("at"), unarmedBeat + 0.04}});
    QVERIFY(unarmed.value(QStringLiteral("ok")).toBool());
    QVERIFY2(std::abs(unarmed.value(QStringLiteral("placed")).toDouble() - (unarmedBeat + 0.04))
                 < 0.005,
             qPrintable(QJsonDocument(unarmed).toJson(QJsonDocument::Compact)));

    const QJsonObject layers = dispatcher.applyOne(QStringLiteral("set_beat_layers"),
                                                   {{QStringLiteral("grid"), true}});
    QVERIFY2(layers.value(QStringLiteral("snapTargets")).toInt() > 0,
             qPrintable(QJsonDocument(layers).toJson(QJsonDocument::Compact)));

    // Grid on: the same 40 ms miss is now pulled onto the beat.
    const QJsonObject armed = dispatcher.applyOne(
        QStringLiteral("move_clip"),
        {{QStringLiteral("clip"), title}, {QStringLiteral("at"), beat + 0.04}});
    QVERIFY(armed.value(QStringLiteral("ok")).toBool());
    QVERIFY2(std::abs(armed.value(QStringLiteral("placed")).toDouble() - beat) < 0.005,
             qPrintable(QStringLiteral("beat=%1 targets=%2 reply=%3")
                            .arg(beat)
                            .arg(layers.value(QStringLiteral("snapTargets")).toInt())
                            .arg(QString::fromUtf8(
                                QJsonDocument(armed).toJson(QJsonDocument::Compact)))));
}

QTEST_MAIN(McpTest)
#include "tst_mcp.moc"

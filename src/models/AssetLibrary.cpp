#include "AssetLibrary.h"

#include "core/Project.h"

#include "engine/MediaProbe.h"
#include "engine/MediaThumbnail.h"

#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QJsonObject>
#include <QMetaObject>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <optional>

namespace {

bool isImagePath(const QString &path)
{
    static const QStringList extensions = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"),  QStringLiteral("webp"), QStringLiteral("bmp"),
        QStringLiteral("tiff"), QStringLiteral("tif"),  QStringLiteral("svg"),
    };
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

bool isAudioPath(const QString &path)
{
    static const QStringList extensions = {
        QStringLiteral("mp3"),  QStringLiteral("wav"),  QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("ogg"),  QStringLiteral("m4a"),
        QStringLiteral("wma"),  QStringLiteral("aiff"), QStringLiteral("aif"),
    };
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

TonDron::MediaKind kindFrom(const MediaInfo &info, const QString &path)
{
    if (isImagePath(path))
        return TonDron::MediaKind::Image;

    for (const StreamInfo &stream : info.streams) {
        if (stream.type == StreamInfo::Type::Video && !stream.attachedPicture)
            return TonDron::MediaKind::Video;
    }
    for (const StreamInfo &stream : info.streams) {
        if (stream.type == StreamInfo::Type::Audio)
            return TonDron::MediaKind::Audio;
    }
    return TonDron::MediaKind::Other;
}

TonDron::MediaKind provisionalKind(const QString &path)
{
    if (isImagePath(path))
        return TonDron::MediaKind::Image;
    if (isAudioPath(path))
        return TonDron::MediaKind::Audio;
    return TonDron::MediaKind::Video;
}

QString formatDuration(TonDron::TimeUs durationUs)
{
    if (durationUs <= 0)
        return {};

    const int totalSeconds = static_cast<int>(durationUs / TonDron::kUsPerSecond);
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }

    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

void fillAudioPresence(TonDron::MediaAsset &asset, const MediaInfo &info)
{
    bool hasAudio = false;
    for (const StreamInfo &stream : info.streams) {
        if (stream.type == StreamInfo::Type::Audio) {
            hasAudio = true;
            asset.sampleRate = stream.sampleRate;
            asset.channels = stream.channels;
            if (asset.codecName.isEmpty())
                asset.codecName = stream.codecName;
        }
    }
    asset.hasAudio = hasAudio;
    asset.hasAudioKnown = true;
}

TonDron::MediaAsset buildProbedAsset(const QString &absolutePath, const QString &name, const MediaInfo &info)
{
    TonDron::MediaAsset asset;
    asset.name = name;
    asset.path = absolutePath;
    asset.kind = kindFrom(info, absolutePath);
    asset.durationUs = info.durationUs;
    asset.durationLabel =
        asset.kind == TonDron::MediaKind::Image ? QString() : formatDuration(info.durationUs);

    for (const StreamInfo &stream : info.streams) {
        if (stream.type == StreamInfo::Type::Video && !stream.attachedPicture) {
            asset.width = stream.width;
            asset.height = stream.height;
            asset.fps = stream.fps;
            asset.rotationDegrees = stream.rotationDegrees;
            asset.codecName = stream.codecName;
        }
    }
    fillAudioPresence(asset, info);

    const QString kindString = TonDron::mediaKindToString(asset.kind);
    asset.thumbnailPath = MediaThumbnail::generate(absolutePath, kindString);
    asset.filmstripPath = asset.kind == TonDron::MediaKind::Video
                              ? MediaThumbnail::generateFilmstrip(absolutePath, kindString)
                              : asset.thumbnailPath;
    return asset;
}

TonDron::MediaAsset buildImageAsset(const QString &absolutePath, const QString &name)
{
    const QString kindString = TonDron::mediaKindToString(TonDron::MediaKind::Image);
    const QString thumb = MediaThumbnail::generate(absolutePath, kindString);
    QImageReader reader(absolutePath);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (reader.transformation() & QImageIOHandler::TransformationRotate90)
        size.transpose();

    TonDron::MediaAsset asset;
    asset.name = name;
    asset.path = absolutePath;
    asset.kind = TonDron::MediaKind::Image;
    asset.width = size.width();
    asset.height = size.height();
    asset.thumbnailPath = thumb;
    asset.filmstripPath = thumb;
    asset.hasAudio = false;
    asset.hasAudioKnown = true;
    return asset;
}

// Reads everything the bin needs about a file. Blocking, so it only ever runs on a worker
// thread — shared by the import path and the replace path.
std::optional<TonDron::MediaAsset> probeAsset(const QString &absolutePath, bool imageOnly)
{
    const QString name = QFileInfo(absolutePath).fileName();
    if (imageOnly)
        return buildImageAsset(absolutePath, name);

    const MediaInfo info = MediaProbe::probe(absolutePath);
    if (!info.ok)
        return std::nullopt;
    return buildProbedAsset(absolutePath, name, info);
}

} // namespace

AssetLibrary::AssetLibrary(QObject *parent)
    : QAbstractListModel(parent)
{
    // Re-broadcast every row-count change as countChanged so QML bindings on
    // `count` stay live without each mutation site having to remember to emit.
    connect(this, &QAbstractItemModel::rowsInserted, this, &AssetLibrary::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &AssetLibrary::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &AssetLibrary::countChanged);

    connect(this, &QAbstractItemModel::rowsInserted, this, &AssetLibrary::snapshotAssets);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &AssetLibrary::snapshotAssets);
    connect(this, &QAbstractItemModel::modelReset, this, &AssetLibrary::snapshotAssets);
}

QList<QString> AssetLibrary::currentPaths() const
{
    if (!m_project)
        return {};

    QList<QString> paths;
    paths.reserve(m_project->assetOrder().size());
    for (const QString &id : m_project->assetOrder()) {
        const TonDron::MediaAsset *asset = m_project->asset(id);
        paths.append(asset ? asset->path : QString{});
    }
    return paths;
}

void AssetLibrary::snapshotAssets()
{
    m_syncedOrder = m_project ? m_project->assetOrder() : QList<QString>{};
    m_syncedPaths = currentPaths();
}

void AssetLibrary::syncToProject()
{
    if (!m_project)
        return;

    // Undo/redo assigns the whole project behind this model's back. Resetting
    // unconditionally would rebuild every card on every unrelated timeline
    // undo, so only an actual order change is worth the churn.
    if (m_syncedOrder != m_project->assetOrder()) {
        beginResetModel();
        endResetModel();
        return;
    }

    // An undone source replace leaves the order untouched — same row, same id, different file —
    // so the paths have to be compared too or the card keeps showing the media it no longer
    // points at. Only the rows that actually moved are re-read.
    const QList<QString> paths = currentPaths();
    if (paths == m_syncedPaths)
        return;

    for (int i = 0; i < paths.size(); ++i) {
        if (i < m_syncedPaths.size() && m_syncedPaths.at(i) == paths.at(i))
            continue;
        emitAssetRowChanged(i, {}); // empty roles: every role may have moved with the file
        emit assetMetadataChanged(m_project->assetIdAt(i));
    }
    m_syncedPaths = paths;
}

void AssetLibrary::setProject(TonDron::Project *project)
{
    beginResetModel();
    m_project = project;
    m_importPending.clear();
    m_thumbPending.clear();
    m_audioProbePending.clear();
    endResetModel();
}

int AssetLibrary::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_project)
        return 0;
    return m_project->assetOrder().size();
}

const TonDron::MediaAsset *AssetLibrary::assetAtIndex(int index) const
{
    if (!m_project || index < 0 || index >= m_project->assetOrder().size())
        return nullptr;
    return m_project->asset(m_project->assetIdAt(index));
}

TonDron::MediaAsset *AssetLibrary::assetAtIndex(int index)
{
    if (!m_project || index < 0 || index >= m_project->assetOrder().size())
        return nullptr;
    return m_project->asset(m_project->assetIdAt(index));
}

QVariant AssetLibrary::data(const QModelIndex &index, int role) const
{
    const TonDron::MediaAsset *asset = assetAtIndex(index.row());
    if (!index.isValid() || !asset)
        return {};

    switch (role) {
    case IdRole:
        return asset->id;
    case NameRole:
        return asset->name;
    case KindRole:
        return TonDron::mediaKindToString(asset->kind);
    case DurationRole:
        return asset->durationLabel;
    case DurationSecondsRole:
        return TonDron::usToSeconds(asset->durationUs);
    case PathRole:
        return asset->path;
    case ThumbnailPathRole:
        return asset->thumbnailPath;
    case FilmstripPathRole:
        return asset->filmstripPath;
    default:
        return {};
    }
}

QHash<int, QByteArray> AssetLibrary::roleNames() const
{
    return {
        {IdRole, "id"},
        {NameRole, "name"},
        {KindRole, "kind"},
        {DurationRole, "duration"},
        {DurationSecondsRole, "durationSeconds"},
        {PathRole, "path"},
        {ThumbnailPathRole, "thumbnailPath"},
        {FilmstripPathRole, "filmstripPath"},
    };
}

bool AssetLibrary::containsPath(const QString &path) const
{
    return indexOfPath(path) >= 0;
}

int AssetLibrary::indexOfPath(const QString &path) const
{
    if (!m_project)
        return -1;

    const QString normalized = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_project->assetOrder().size(); ++i) {
        const TonDron::MediaAsset *asset = assetAtIndex(i);
        if (asset && asset->path == normalized)
            return i;
    }
    return -1;
}

int AssetLibrary::indexOfId(const QString &id) const
{
    if (!m_project)
        return -1;
    return m_project->assetIndex(id);
}

QString AssetLibrary::assetIdAt(int index) const
{
    if (!m_project)
        return {};
    return m_project->assetIdAt(index);
}

void AssetLibrary::emitAssetRowChanged(int index, const QList<int> &roles)
{
    if (index < 0)
        return;
    const QModelIndex modelIndex = createIndex(index, 0);
    emit dataChanged(modelIndex, modelIndex, roles);
}

void AssetLibrary::startThumbJob(const QString &assetId)
{
    if (!m_project || assetId.isEmpty() || m_thumbPending.contains(assetId))
        return;

    TonDron::MediaAsset *asset = m_project->asset(assetId);
    if (!asset)
        return;

    const bool needThumb = asset->thumbnailPath.isEmpty() || !QFileInfo::exists(asset->thumbnailPath);
    const bool needStrip = asset->kind == TonDron::MediaKind::Video
                           && (asset->filmstripPath.isEmpty() || !QFileInfo::exists(asset->filmstripPath));
    if (!needThumb && !needStrip) {
        if (asset->kind != TonDron::MediaKind::Video && !asset->thumbnailPath.isEmpty()
            && asset->filmstripPath != asset->thumbnailPath) {
            asset->filmstripPath = asset->thumbnailPath;
            emitAssetRowChanged(indexOfId(assetId), {FilmstripPathRole});
        }
        return;
    }

    m_thumbPending.insert(assetId);
    const QString path = asset->path;
    const TonDron::MediaKind kind = asset->kind;

    (void)QtConcurrent::run([this, assetId, path, kind, needThumb, needStrip]() {
        const QString kindString = TonDron::mediaKindToString(kind);
        QString thumb;
        QString strip;
        if (needThumb)
            thumb = MediaThumbnail::generate(path, kindString);
        if (needStrip)
            strip = MediaThumbnail::generateFilmstrip(path, kindString);
        else if (!thumb.isEmpty() && kind != TonDron::MediaKind::Video)
            strip = thumb;

        QMetaObject::invokeMethod(
            this,
            [this, assetId, path, thumb, strip]() { applyThumbResult(assetId, path, thumb, strip); },
            Qt::QueuedConnection);
    });
}

void AssetLibrary::applyThumbResult(const QString &assetId, const QString &sourcePath,
                                    const QString &thumb, const QString &strip)
{
    m_thumbPending.remove(assetId);
    if (!m_project)
        return;

    TonDron::MediaAsset *asset = m_project->asset(assetId);
    // The source was replaced while this job ran, so these frames are of a file the row no
    // longer points at.
    if (!asset || asset->path != sourcePath)
        return;

    bool changed = false;
    if (!thumb.isEmpty() && asset->thumbnailPath != thumb) {
        asset->thumbnailPath = thumb;
        changed = true;
    }
    if (!strip.isEmpty() && asset->filmstripPath != strip) {
        asset->filmstripPath = strip;
        changed = true;
    } else if (asset->kind != TonDron::MediaKind::Video && !asset->thumbnailPath.isEmpty()
               && asset->filmstripPath != asset->thumbnailPath) {
        asset->filmstripPath = asset->thumbnailPath;
        changed = true;
    }

    if (!changed)
        return;

    emitAssetRowChanged(indexOfId(assetId), {ThumbnailPathRole, FilmstripPathRole});
    emit assetMetadataChanged(assetId);
}

void AssetLibrary::refreshMediaAt(int index)
{
    TonDron::MediaAsset *asset = assetAtIndex(index);
    if (!asset)
        return;
    startThumbJob(asset->id);
}

void AssetLibrary::startImportJob(const QString &assetId, const QString &absolutePath, bool imageOnly)
{
    if (assetId.isEmpty() || m_importPending.contains(assetId))
        return;

    m_importPending.insert(assetId);

    (void)QtConcurrent::run([this, assetId, absolutePath, imageOnly]() {
        const std::optional<TonDron::MediaAsset> probed = probeAsset(absolutePath, imageOnly);
        const TonDron::MediaAsset filled = probed.value_or(TonDron::MediaAsset{});
        const bool ok = probed.has_value();

        QMetaObject::invokeMethod(
            this,
            [this, assetId, filled, ok]() { applyImportResult(assetId, filled, ok); },
            Qt::QueuedConnection);
    });
}

bool AssetLibrary::startReplaceProbe(int index, const QString &absolutePath)
{
    const TonDron::MediaAsset *asset = assetAtIndex(index);
    if (!asset || absolutePath.isEmpty())
        return false;

    const QString assetId = asset->id;
    if (m_importPending.contains(assetId))
        return false;

    m_importPending.insert(assetId);
    const bool imageOnly = isImagePath(absolutePath);

    (void)QtConcurrent::run([this, assetId, absolutePath, imageOnly]() {
        const std::optional<TonDron::MediaAsset> probed = probeAsset(absolutePath, imageOnly);
        const TonDron::MediaAsset filled = probed.value_or(TonDron::MediaAsset{});
        const bool ok = probed.has_value();

        QMetaObject::invokeMethod(
            this,
            [this, assetId, filled, ok]() {
                m_importPending.remove(assetId);
                emit assetSourceProbed(assetId, filled, ok);
            },
            Qt::QueuedConnection);
    });
    return true;
}

bool AssetLibrary::applyProbedSource(const QString &assetId, const TonDron::MediaAsset &filled)
{
    if (!m_project)
        return false;

    const int index = indexOfId(assetId);
    TonDron::MediaAsset *asset = index < 0 ? nullptr : m_project->asset(assetId);
    if (!asset)
        return false;

    const QString id = asset->id;
    *asset = filled;
    asset->id = id;

    // Jobs still in flight were started against the old file. They drop themselves on landing
    // because the path they probed no longer matches; clearing the pending flags is what lets
    // the replacement start its own.
    m_thumbPending.remove(assetId);
    m_audioProbePending.remove(assetId);

    snapshotAssets();
    emitAssetRowChanged(index,
                        {NameRole, KindRole, DurationRole, DurationSecondsRole, PathRole,
                         ThumbnailPathRole, FilmstripPathRole});
    emit assetMetadataChanged(assetId);
    return true;
}

void AssetLibrary::applyImportResult(const QString &assetId, const TonDron::MediaAsset &filled, bool ok)
{
    m_importPending.remove(assetId);
    if (!m_project)
        return;

    const int index = indexOfId(assetId);
    if (index < 0)
        return;

    if (!ok) {
        beginRemoveRows({}, index, index);
        m_project->assets().remove(assetId);
        m_project->assetOrder().removeAll(assetId);
        endRemoveRows();
        return;
    }

    TonDron::MediaAsset *asset = m_project->asset(assetId);
    if (!asset)
        return;

    asset->name = filled.name;
    asset->kind = filled.kind;
    asset->durationUs = filled.durationUs;
    asset->durationLabel = filled.durationLabel;
    asset->path = filled.path;
    asset->width = filled.width;
    asset->height = filled.height;
    asset->fps = filled.fps;
    asset->rotationDegrees = filled.rotationDegrees;
    asset->sampleRate = filled.sampleRate;
    asset->channels = filled.channels;
    asset->codecName = filled.codecName;
    asset->hasAudio = filled.hasAudio;
    asset->hasAudioKnown = filled.hasAudioKnown;
    asset->thumbnailPath = filled.thumbnailPath;
    asset->filmstripPath = filled.filmstripPath;

    emitAssetRowChanged(index,
                        {NameRole, KindRole, DurationRole, DurationSecondsRole, PathRole,
                         ThumbnailPathRole, FilmstripPathRole});
    emit assetMetadataChanged(assetId);
}

QVariantMap AssetLibrary::assetAt(int index) const
{
    const TonDron::MediaAsset *asset = assetAtIndex(index);
    if (!asset)
        return {};

    return {
        {QStringLiteral("id"), asset->id},
        {QStringLiteral("name"), asset->name},
        {QStringLiteral("kind"), TonDron::mediaKindToString(asset->kind)},
        {QStringLiteral("duration"), asset->durationLabel},
        {QStringLiteral("durationSeconds"), TonDron::usToSeconds(asset->durationUs)},
        {QStringLiteral("path"), asset->path},
        {QStringLiteral("width"), asset->width},
        {QStringLiteral("height"), asset->height},
        {QStringLiteral("fps"), asset->fps},
        {QStringLiteral("rotationDegrees"), asset->rotationDegrees},
        {QStringLiteral("thumbnailPath"), asset->thumbnailPath},
        {QStringLiteral("filmstripPath"), asset->filmstripPath},
        {QStringLiteral("assetIndex"), index},
    };
}

QString AssetLibrary::thumbnailAt(int index) const
{
    const TonDron::MediaAsset *asset = assetAtIndex(index);
    return asset ? asset->thumbnailPath : QString{};
}

QString AssetLibrary::filmstripAt(int index) const
{
    const TonDron::MediaAsset *asset = assetAtIndex(index);
    return asset ? asset->filmstripPath : QString{};
}

void AssetLibrary::ensureMedia(int index)
{
    refreshMediaAt(index);
}

void AssetLibrary::ensureAllMedia()
{
    if (!m_project)
        return;
    for (int i = 0; i < m_project->assetOrder().size(); ++i)
        refreshMediaAt(i);
}

void AssetLibrary::ensureAudioPresence(const QString &assetId)
{
    if (!m_project || assetId.isEmpty() || m_audioProbePending.contains(assetId))
        return;

    TonDron::MediaAsset *asset = m_project->asset(assetId);
    if (!asset || asset->hasAudioKnown)
        return;

    if (asset->channels > 0 || asset->sampleRate > 0) {
        asset->hasAudio = true;
        asset->hasAudioKnown = true;
        emit assetMetadataChanged(assetId);
        return;
    }

    m_audioProbePending.insert(assetId);
    const QString path = asset->path;

    (void)QtConcurrent::run([this, assetId, path]() {
        const MediaInfo info = MediaProbe::probe(path);
        bool hasAudio = false;
        int sampleRate = 0;
        int channels = 0;
        if (info.ok) {
            for (const StreamInfo &stream : info.streams) {
                if (stream.type == StreamInfo::Type::Audio) {
                    hasAudio = true;
                    sampleRate = stream.sampleRate;
                    channels = stream.channels;
                    break;
                }
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, assetId, path, hasAudio, sampleRate, channels]() {
                applyAudioPresence(assetId, path, hasAudio, sampleRate, channels);
            },
            Qt::QueuedConnection);
    });
}

void AssetLibrary::applyAudioPresence(const QString &assetId, const QString &sourcePath,
                                      bool hasAudio, int sampleRate, int channels)
{
    m_audioProbePending.remove(assetId);
    if (!m_project)
        return;

    TonDron::MediaAsset *asset = m_project->asset(assetId);
    // Answered for a file the row no longer points at; the replacement brought its own.
    if (!asset || asset->path != sourcePath)
        return;

    asset->hasAudio = hasAudio;
    asset->hasAudioKnown = true;
    if (hasAudio) {
        if (sampleRate > 0)
            asset->sampleRate = sampleRate;
        if (channels > 0)
            asset->channels = channels;
    }
    emit assetMetadataChanged(assetId);
}

void AssetLibrary::sortByName()
{
    if (!m_project || m_project->assetOrder().size() < 2)
        return;

    beginResetModel();
    QList<QString> order = m_project->assetOrder();
    std::sort(order.begin(), order.end(), [this](const QString &a, const QString &b) {
        const TonDron::MediaAsset *assetA = m_project->asset(a);
        const TonDron::MediaAsset *assetB = m_project->asset(b);
        if (!assetA || !assetB)
            return a < b;
        return assetA->name.compare(assetB->name, Qt::CaseInsensitive) < 0;
    });
    m_project->assetOrder() = order;
    endResetModel();
}

bool AssetLibrary::setAssetName(int index, const QString &name)
{
    TonDron::MediaAsset *asset = assetAtIndex(index);
    if (!asset)
        return false;

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || asset->name == trimmed)
        return false;

    asset->name = trimmed;
    emitAssetRowChanged(index, {NameRole});
    snapshotAssets();
    return true;
}

void AssetLibrary::sortByKind()
{
    if (!m_project || m_project->assetOrder().size() < 2)
        return;

    beginResetModel();
    QList<QString> order = m_project->assetOrder();
    std::sort(order.begin(), order.end(), [this](const QString &a, const QString &b) {
        const TonDron::MediaAsset *assetA = m_project->asset(a);
        const TonDron::MediaAsset *assetB = m_project->asset(b);
        if (!assetA || !assetB)
            return a < b;
        const int cmp = TonDron::mediaKindToString(assetA->kind)
                            .compare(TonDron::mediaKindToString(assetB->kind), Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : assetA->name.compare(assetB->name, Qt::CaseInsensitive) < 0;
    });
    m_project->assetOrder() = order;
    endResetModel();
}

bool AssetLibrary::removeAssetAt(int index)
{
    if (!m_project || index < 0 || index >= m_project->assetOrder().size())
        return false;

    const QString assetId = m_project->assetIdAt(index);
    beginRemoveRows({}, index, index);
    m_project->assets().remove(assetId);
    m_project->assetOrder().removeAll(assetId);
    endRemoveRows();

    // In-flight probe/thumb jobs already no-op when the id is gone; this just
    // keeps the pending sets from retaining ids nothing will ever clear.
    m_importPending.remove(assetId);
    m_thumbPending.remove(assetId);
    m_audioProbePending.remove(assetId);
    return true;
}

void AssetLibrary::clear()
{
    if (!m_project || m_project->assetOrder().isEmpty())
        return;

    beginResetModel();
    m_project->assets().clear();
    m_project->assetOrder().clear();
    m_importPending.clear();
    m_thumbPending.clear();
    m_audioProbePending.clear();
    endResetModel();
}

QJsonArray AssetLibrary::toJsonArray() const
{
    if (!m_project)
        return {};

    QJsonArray assets;
    for (const QString &id : m_project->assetOrder()) {
        const TonDron::MediaAsset *asset = m_project->asset(id);
        if (!asset)
            continue;
        QJsonObject object{
            {QStringLiteral("id"), asset->id},
            {QStringLiteral("name"), asset->name},
            {QStringLiteral("kind"), TonDron::mediaKindToString(asset->kind)},
            {QStringLiteral("durationUs"), static_cast<double>(asset->durationUs)},
            {QStringLiteral("duration"), asset->durationLabel},
            {QStringLiteral("path"), asset->path},
            {QStringLiteral("width"), asset->width},
            {QStringLiteral("height"), asset->height},
            {QStringLiteral("fps"), asset->fps},
            {QStringLiteral("rotationDegrees"), asset->rotationDegrees},
            {QStringLiteral("sampleRate"), asset->sampleRate},
            {QStringLiteral("channels"), asset->channels},
            {QStringLiteral("codecName"), asset->codecName},
            {QStringLiteral("thumbnailPath"), asset->thumbnailPath},
            {QStringLiteral("filmstripPath"), asset->filmstripPath},
        };
        if (asset->hasAudioKnown)
            object.insert(QStringLiteral("hasAudio"), asset->hasAudio);
        assets.append(object);
    }
    return assets;
}

void AssetLibrary::loadFromJsonArray(const QJsonArray &assets)
{
    if (!m_project)
        return;

    beginResetModel();
    m_project->assets().clear();
    m_project->assetOrder().clear();
    m_importPending.clear();
    m_thumbPending.clear();
    m_audioProbePending.clear();

    for (const QJsonValue &value : assets) {
        const QJsonObject object = value.toObject();
        TonDron::MediaAsset asset;
        asset.id = object.value(QStringLiteral("id")).toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
        asset.name = object.value(QStringLiteral("name")).toString();
        asset.kind = TonDron::mediaKindFromString(object.value(QStringLiteral("kind")).toString());
        asset.durationLabel = object.value(QStringLiteral("duration")).toString();
        if (object.contains(QStringLiteral("durationUs"))) {
            asset.durationUs = static_cast<TonDron::TimeUs>(object.value(QStringLiteral("durationUs")).toDouble());
        } else {
            asset.durationUs = TonDron::secondsToUs(object.value(QStringLiteral("durationSeconds")).toDouble());
        }
        asset.path = object.value(QStringLiteral("path")).toString();
        asset.width = object.value(QStringLiteral("width")).toInt();
        asset.height = object.value(QStringLiteral("height")).toInt();
        asset.fps = object.value(QStringLiteral("fps")).toDouble();
        asset.rotationDegrees = object.value(QStringLiteral("rotationDegrees")).toInt();
        asset.sampleRate = object.value(QStringLiteral("sampleRate")).toInt();
        asset.channels = object.value(QStringLiteral("channels")).toInt();
        asset.codecName = object.value(QStringLiteral("codecName")).toString();
        asset.thumbnailPath = object.value(QStringLiteral("thumbnailPath")).toString();
        asset.filmstripPath = object.value(QStringLiteral("filmstripPath")).toString();
        if (object.contains(QStringLiteral("hasAudio"))) {
            asset.hasAudioKnown = true;
            asset.hasAudio = object.value(QStringLiteral("hasAudio")).toBool();
        } else if (asset.channels > 0 || asset.sampleRate > 0) {
            asset.hasAudioKnown = true;
            asset.hasAudio = true;
        }
        m_project->addAsset(asset);
    }

    endResetModel();

    for (int i = 0; i < m_project->assetOrder().size(); ++i)
        refreshMediaAt(i);
}

void AssetLibrary::importUrls(const QList<QUrl> &urls)
{
    QStringList paths;
    paths.reserve(urls.size());
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        } else if (!url.isEmpty()) {
            const QString asString = url.toString();
            if (asString.startsWith(QLatin1String("file://"), Qt::CaseInsensitive))
                paths.append(QUrl(asString).toLocalFile());
            else
                paths.append(asString);
        }
    }
    importFiles(paths);
}

QStringList AssetLibrary::importLocalPaths(const QStringList &paths)
{
    return importFilesReturningIds(paths);
}

bool AssetLibrary::isImportPending(const QString &assetId) const
{
    return m_importPending.contains(assetId);
}

QString AssetLibrary::addGeneratedAsset(TonDron::MediaAsset asset)
{
    if (!m_project || asset.path.isEmpty())
        return {};

    if (asset.id.isEmpty())
        asset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    asset.path = QFileInfo(asset.path).absoluteFilePath();

    const int row = m_project->assetOrder().size();
    beginInsertRows({}, row, row);
    const QString id = m_project->addAsset(asset);
    endInsertRows();
    return id;
}

void AssetLibrary::importFiles(const QStringList &paths)
{
    importFilesReturningIds(paths);
}

QStringList AssetLibrary::importFilesReturningIds(const QStringList &paths)
{
    QStringList ids;
    if (!m_project)
        return ids;

    for (const QString &path : paths) {
        const QFileInfo fileInfo(path);
        const QString absolutePath = fileInfo.absoluteFilePath();
        if (!fileInfo.isFile())
            continue;

        const int existingIndex = indexOfPath(absolutePath);
        if (existingIndex >= 0) {
            refreshMediaAt(existingIndex);
            ids.append(assetIdAt(existingIndex));
            continue;
        }

        TonDron::MediaAsset placeholder;
        placeholder.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        placeholder.name = fileInfo.fileName();
        placeholder.path = absolutePath;
        placeholder.kind = provisionalKind(absolutePath);

        const int row = m_project->assetOrder().size();
        beginInsertRows({}, row, row);
        m_project->addAsset(placeholder);
        endInsertRows();

        startImportJob(placeholder.id, absolutePath, isImagePath(path));
        ids.append(placeholder.id);
    }
    return ids;
}

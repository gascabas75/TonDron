#include "FrameCompositor.h"

#include "ClipReaderPool.h"
#include "CompositorFrameHistory.h"
#include "EffectCatalog.h"
#include "EffectProcessor.h"
#include "FaceTrack.h"
#include "GpuCompositor.h"
#include "GpuEffectExecutor.h"
#include "MaskApplier.h"
#include "ReverseProxyCache.h"
#include "TextRaster.h"
#include "TransitionCatalog.h"
#include "core/Clip.h"
#include "core/ClipAnimation.h"
#include "core/ShapePath.h"
#include "core/SubtitleCue.h"
#include "core/Time.h"
#include "core/Transition.h"

#include <QBrush>
#include <QColor>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImageReader>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <cmath>
#include <QtMath>

#include <unordered_map>

namespace {

void collectActivePaths(const TonDron::Project *project, TonDron::TimeUs timelineUs, QSet<QString> &videoPaths,
                        QSet<QString> &audioPaths)
{
    if (!project)
        return;

    for (const TonDron::Track &track : project->tracks()) {
        if (track.hidden)
            continue;

        for (const TonDron::Clip &clip : track.clips) {
            if (!clip.containsTime(timelineUs))
                continue;

            // Retained separately from clip.path: the matte has its own reader, and dropping it
            // here would tear the worker down and re-open the file every frame.
            if (clip.mask.shape == TonDron::MaskShape::Matte && !clip.mask.mattePath.isEmpty())
                videoPaths.insert(clip.mask.mattePath);

            if (clip.path.isEmpty())
                continue;
            if (clip.type == TonDron::ClipType::Shape)
                continue;

            if ((track.type == TonDron::TrackType::Video || track.type == TonDron::TrackType::Shape)
                && clip.type != TonDron::ClipType::Text) {
                // The reversed proxy, when there is one, is what the composite actually reads —
                // retaining clip.path instead would tear down the proxy's worker every frame.
                videoPaths.insert(TonDron::videoReadPath(clip));
            }
            if (track.type == TonDron::TrackType::Audio
                || (track.type == TonDron::TrackType::Video && clip.type == TonDron::ClipType::Video)) {
                audioPaths.insert(clip.path);
            }
        }
    }
}

// Every video frame this composite will need, so the readers can decode them
// concurrently on their own threads instead of one clip at a time on ours.
QList<ClipReaderPool::VideoRequest> collectVideoRequests(const TonDron::Project *project,
                                                         TonDron::TimeUs timelineUs, int maxWidth,
                                                         int maxHeight)
{
    QList<ClipReaderPool::VideoRequest> requests;
    if (!project)
        return requests;

    for (const TonDron::Track &track : project->tracks()) {
        if (track.hidden || track.type == TonDron::TrackType::Audio)
            continue;

        for (const TonDron::Clip &clip : track.clips) {
            if (!clip.containsTime(timelineUs))
                continue;

            // Mattes decode like any other video, so warm them alongside the sources rather
            // than stalling the composite on a serial read later.
            if (clip.mask.shape == TonDron::MaskShape::Matte && !clip.mask.mattePath.isEmpty()) {
                requests.append(ClipReaderPool::VideoRequest{
                    clip.mask.mattePath,
                    qMax<TonDron::TimeUs>(0, clip.timelineToSourceUs(timelineUs)
                                               - clip.mask.matteSrcOffsetUs),
                    maxWidth, maxHeight});
            }

            if (clip.type != TonDron::ClipType::Video || clip.path.isEmpty())
                continue;

            const TonDron::VideoRead read = TonDron::resolveVideoRead(clip, timelineUs);
            requests.append(
                ClipReaderPool::VideoRequest{read.path, read.sourceUs, maxWidth, maxHeight});
        }
    }
    return requests;
}

const TonDron::Effect *findTimeEchoEffect(const QList<TonDron::Effect> &effects)
{
    for (const TonDron::Effect &effect : effects) {
        if (!effect.enabled)
            continue;
        if (effect.catalogId == QStringLiteral("time_echo"))
            return &effect;
    }
    return nullptr;
}

// Only worth touching the face track when something in the chain actually consumes it, so a clip
// that has been detected but is running ordinary effects pays nothing.
bool chainNeedsFace(const QList<TonDron::Effect> &effects)
{
    for (const TonDron::Effect &effect : effects) {
        if (!effect.enabled)
            continue;
        const EffectPresetEntry *def =
            effect.catalogId.isEmpty() ? nullptr : effectDefForId(effect.catalogId);
        if (def && def->needsFace)
            return true;
    }
    return false;
}

// This frame's anchors for a clip, or an empty list when nothing in the chain wants them. Shared by
// the CPU and GPU compositing paths so a face warp cannot come out differently between preview and
// export depending on which one ran.
QList<TonDron::FaceAnchors> faceSlotsForClip(const TonDron::Clip &clip,
                                           const QList<TonDron::Effect> &effects,
                                           TonDron::TimeUs timelineUs)
{
    if (clip.faceTrackPath.isEmpty() || !chainNeedsFace(effects))
        return {};
    const auto track = TonDron::loadFaceTrackCached(clip.faceTrackPath);
    if (!track)
        return {};
    return track->sampleAll(clip.timelineToSourceUs(timelineUs) - clip.faceTrackSrcOffsetUs);
}

// The clip's chain as it should render *this* frame: time_echo dropped (its trail is assembled
// before the chain runs) and every keyframed parameter baked down to its value at clipTimeUs.
// Both the CPU and GPU paths go through here, so an animated parameter cannot come out different
// between preview and export.
QList<TonDron::Effect> resolvedClipEffects(const TonDron::Clip &clip, TonDron::TimeUs clipTimeUs)
{
    QList<TonDron::Effect> filtered;
    filtered.reserve(clip.effects.size());
    for (const TonDron::Effect &effect : clip.effects) {
        if (!effect.enabled)
            continue;
        if (effect.catalogId != QStringLiteral("time_echo"))
            filtered.append(effect.resolvedAt(clipTimeUs));
    }
    return filtered;
}

// Still images never change frame to frame, but decodeClipMediaFrame used to
// re-read and re-decode the file on every composited frame. Cache the scaled
// result per (path, size).
QImage decodedStillImage(const QString &path, int maxWidth, int maxHeight)
{
    // Keyed on mtime and size as well as path: the same path can hold different
    // pixels over time, and serving a stale decode would silently render the old
    // image.
    struct Key
    {
        QString path;
        qint64 mtimeMs = 0;
        qint64 fileSize = 0;
        int w = 0;
        int h = 0;
        bool operator==(const Key &other) const
        {
            return path == other.path && mtimeMs == other.mtimeMs && fileSize == other.fileSize
                   && w == other.w && h == other.h;
        }
    };
    struct KeyHash
    {
        size_t operator()(const Key &k) const
        {
            return qHash(k.path) ^ size_t(k.mtimeMs) ^ (size_t(k.fileSize) << 7)
                   ^ (size_t(k.w) << 1) ^ (size_t(k.h) << 17);
        }
    };

    static QMutex mutex;
    static std::unordered_map<Key, QImage, KeyHash> cache;

    const QFileInfo info(path);
    const Key key{path, info.lastModified().toMSecsSinceEpoch(), info.size(), maxWidth, maxHeight};
    {
        QMutexLocker lock(&mutex);
        const auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
    }

    QImageReader reader(path);
    QImage image = reader.read();
    if (image.isNull())
        return {};
    image = image.convertToFormat(QImage::Format_RGBA8888)
                .scaled(maxWidth, maxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QMutexLocker lock(&mutex);
    if (cache.size() > 32)
        cache.clear();
    cache.emplace(key, image);
    return image;
}

// maxWidth/maxHeight bound the decode buffer. They are deliberately *not* the
// clip's layout rect: the layout rect moves every frame under a scale keyframe,
// and a changing decode size invalidates the decoder's frame cache and forces a
// keyframe seek per frame. Decoding to a stable, canvas-bounded size and letting
// the draw step scale is both stable and cheaper.
QImage decodeClipMediaFrame(const TonDron::Clip &clip, TonDron::TimeUs timelineUs, int maxWidth, int maxHeight)
{
    if (clip.path.isEmpty())
        return {};

    if (clip.type == TonDron::ClipType::Image)
        return decodedStillImage(clip.path, maxWidth, maxHeight);

    if (clip.type == TonDron::ClipType::Video) {
        const TonDron::VideoRead read = TonDron::resolveVideoRead(clip, timelineUs);
        return ClipReaderPool::instance().readVideoFrame(read.path, read.sourceUs, maxWidth, maxHeight);
    }

    return {};
}

QImage shapeImageForClip(const TonDron::Clip &clip, int width, int height, double renderScale);

// maxWidth/maxHeight bound the decoded frame; the returned image may be smaller
// (source-limited) and is scaled to the clip's layout rect at draw time.
QImage imageForClip(const TonDron::Clip &clip, TonDron::TimeUs timelineUs, int maxWidth, int maxHeight,
                    int projectFps, int maxTimeEchoHistoryFrames)
{
    if (clip.type == TonDron::ClipType::Shape)
        return shapeImageForClip(clip, maxWidth, maxHeight, 1.0);

    if (clip.path.isEmpty())
        return {};

    const TonDron::TimeUs clipTimeUs = timelineUs - clip.timelineStart;
    const TonDron::Effect *timeEcho = findTimeEchoEffect(clip.effects);
    const QList<TonDron::Effect> otherEffects = resolvedClipEffects(clip, clipTimeUs);

    QImage image;
    if (timeEcho) {
        const EffectPresetEntry *def = effectDefForId(timeEcho->catalogId);
        if (!def)
            return {};

        const QMap<QString, QVariant> params =
            resolvedEffectParameters(timeEcho->resolvedAt(clipTimeUs), *def);
        int frameCount = qBound(1, params.value(QStringLiteral("frames"), 4).toInt(), 10);
        if (maxTimeEchoHistoryFrames >= 0)
            frameCount = qMin(frameCount, maxTimeEchoHistoryFrames);
        const double decay = qBound(0.0, params.value(QStringLiteral("decay"), 0.55).toDouble(), 1.0);
        const auto blendMode =
            CompositorFrameHistory::parseEchoBlendMode(params.value(QStringLiteral("blendMode")).toString());

        const TonDron::TimeUs frameStepUs = TonDron::frameDurationUs(projectFps);
        QList<QImage> samples;
        samples.reserve(frameCount + 1);

        const QImage current = decodeClipMediaFrame(clip, timelineUs, maxWidth, maxHeight);
        if (current.isNull())
            return {};
        samples.append(current);

        for (int i = 1; i <= frameCount; ++i) {
            const TonDron::TimeUs pastClipUs = clipTimeUs - static_cast<TonDron::TimeUs>(i) * frameStepUs;
            if (pastClipUs < 0)
                break;
            const TonDron::TimeUs pastTimelineUs = clip.timelineStart + pastClipUs;
            const QImage past = decodeClipMediaFrame(clip, pastTimelineUs, maxWidth, maxHeight);
            if (!past.isNull())
                samples.append(past);
        }

        image = CompositorFrameHistory::applyTimeEcho(samples, decay, blendMode);
    } else {
        image = decodeClipMediaFrame(clip, timelineUs, maxWidth, maxHeight);
    }

    if (image.isNull())
        return image;

    // Mask geometry is normalized, so it applies at whatever size the decode
    // actually produced.
    if (!otherEffects.isEmpty()) {
        // Baked anchors, so this is a lookup rather than an inference: no ONNX ever runs on the
        // compositor thread, and preview and export read the same numbers.
        image = EffectProcessor::applyEffects(image, otherEffects, clipTimeUs,
                                              faceSlotsForClip(clip, otherEffects, timelineUs));
    }
    if (clip.mask.shape != TonDron::MaskShape::None)
        image = TonDron::applyMask(image, clip.mask, image.width(), image.height());
    return image;
}

Qt::PenStyle penStyleFor(TonDron::ShapeStrokeStyle style)
{
    switch (style) {
    case TonDron::ShapeStrokeStyle::None:
        return Qt::NoPen;
    case TonDron::ShapeStrokeStyle::Solid:
        return Qt::SolidLine;
    case TonDron::ShapeStrokeStyle::Dash:
        return Qt::DashLine;
    case TonDron::ShapeStrokeStyle::Dot:
        return Qt::DotLine;
    case TonDron::ShapeStrokeStyle::DashDot:
        return Qt::DashDotLine;
    }
    return Qt::SolidLine;
}

QBrush shapeBrush(const TonDron::ShapeStyle &style, const QRectF &bounds)
{
    switch (style.fillKind) {
    case TonDron::ShapeFillKind::None:
        return Qt::NoBrush;
    case TonDron::ShapeFillKind::Solid:
        return style.fill;
    case TonDron::ShapeFillKind::LinearGradient: {
        // Angle sweeps the gradient axis across the shape's own bounding box, so the same angle
        // reads the same whatever the clip is scaled to.
        const double radians = qDegreesToRadians(style.gradientAngle);
        const QPointF centre = bounds.center();
        const QPointF half(qCos(radians) * bounds.width() / 2.0,
                           qSin(radians) * bounds.height() / 2.0);
        QLinearGradient gradient(centre - half, centre + half);
        gradient.setColorAt(0.0, style.fill);
        gradient.setColorAt(1.0, style.fillSecondary);
        return gradient;
    }
    case TonDron::ShapeFillKind::RadialGradient: {
        QRadialGradient gradient(bounds.center(),
                                 qMax(bounds.width(), bounds.height()) / 2.0);
        gradient.setColorAt(0.0, style.fill);
        gradient.setColorAt(1.0, style.fillSecondary);
        return gradient;
    }
    }
    return style.fill;
}

// Rasterized at exactly the destination size: the GPU quad is the layout rect and samples this
// texture 0..1, so anything smaller is upscaled and the stroke stretches with it.
QImage shapeImageForClip(const TonDron::Clip &clip, int width, int height, double renderScale)
{
    if (clip.type != TonDron::ClipType::Shape)
        return {};

    const int w = qMax(1, width);
    const int h = qMax(1, height);

    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);

    const TonDron::ShapeStyle &style = clip.shapeStyle;
    // Stroke width and corner radius are authored in project pixels, so they scale with the render
    // just like the layout rect does — otherwise preview and export disagree.
    const double strokeWidth =
        style.strokeStyle == TonDron::ShapeStrokeStyle::None ? 0.0 : style.strokeWidth * renderScale;
    const double inset = strokeWidth / 2.0;
    const QRectF bounds =
        QRectF(0, 0, w, h).adjusted(inset, inset, -inset, -inset).normalized();

    TonDron::ShapeStyle scaled = style;
    scaled.cornerRadius = style.cornerRadius * renderScale;

    p.setBrush(shapeBrush(scaled, bounds));
    p.setPen(strokeWidth <= 0.0
                 ? QPen(Qt::NoPen)
                 : QPen(style.stroke, strokeWidth, penStyleFor(style.strokeStyle), Qt::RoundCap,
                        Qt::RoundJoin));
    p.drawPath(TonDron::shapePath(scaled, bounds));

    p.end();
    return image;
}

double opacityForClip(const TonDron::Clip &clip, TonDron::TimeUs timelineUs)
{
    double value = 1.0;
    if (!clip.opacity.isEmpty()) {
        const TonDron::TimeUs relative = timelineUs - clip.timelineStart;
        value = qBound(0.0, clip.opacity.evaluateAt(relative), 1.0);
    }
    // Edge-relative fades ride on top of any opacity keyframes.
    return value * clip.fadeMultiplier(timelineUs);
}

double transformValue(const TonDron::KeyframeTrack<double> &track, TonDron::TimeUs relative, double defaultValue)
{
    if (track.isEmpty())
        return defaultValue;
    return track.evaluateAt(relative);
}

// Layout is stored in project pixels. Preview/export canvases may be scaled
// via renderScale — always map project → canvas here so WYSIWYG handles match.
void layoutRectForClip(const TonDron::Clip &clip, TonDron::TimeUs timelineUs, int projectWidth, int projectHeight,
                       double renderScale, double extraScale, double *xOut, double *yOut, double *wOut, double *hOut,
                       double *rotationOut = nullptr)
{
    const TonDron::TimeUs relative = timelineUs - clip.timelineStart;
    const double scale = renderScale * extraScale;
    *xOut = transformValue(clip.transformX, relative, 0.0) * renderScale;
    *yOut = transformValue(clip.transformY, relative, 0.0) * renderScale;
    *wOut = transformValue(clip.transformW, relative, static_cast<double>(projectWidth)) * scale;
    *hOut = transformValue(clip.transformH, relative, static_cast<double>(projectHeight)) * scale;
    if (rotationOut)
        *rotationOut = transformValue(clip.rotation, relative, 0.0);
}

// The bottommost active video/image frame at this time, used to derive a blur fill.
// Track 0 is topmost, so walk tracks back-to-front and take the first hit.
QImage bottommostVisualFrame(const TonDron::Project &project, TonDron::TimeUs timelineUs, int width, int height)
{
    const QList<TonDron::Track> &tracks = project.tracks();
    for (int ti = tracks.size() - 1; ti >= 0; --ti) {
        const TonDron::Track &track = tracks.at(ti);
        if (track.hidden || track.type == TonDron::TrackType::Audio)
            continue;
        for (const TonDron::Clip &clip : track.clips) {
            if (!clip.containsTime(timelineUs))
                continue;
            if (clip.type != TonDron::ClipType::Video && clip.type != TonDron::ClipType::Image)
                continue;
            QImage frame = imageForClip(clip, timelineUs, width, height, project.fps(), -1);
            if (!frame.isNull())
                return frame;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// GPU scene building
//
// The pixels a clip contributes before the GPU takes over: decode, plus the
// time_echo trail (which needs several decoded frames). Effects and the mask are
// deliberately left to the GPU.
QImage gpuSourceForClip(const TonDron::Clip &clip, TonDron::TimeUs timelineUs, int maxWidth, int maxHeight,
                        int projectFps, int maxTimeEchoHistoryFrames)
{
    if (clip.path.isEmpty())
        return {};

    const TonDron::Effect *timeEcho = findTimeEchoEffect(clip.effects);
    if (!timeEcho)
        return decodeClipMediaFrame(clip, timelineUs, maxWidth, maxHeight);

    const EffectPresetEntry *def = effectDefForId(timeEcho->catalogId);
    if (!def)
        return {};

    const TonDron::TimeUs clipTimeUs = timelineUs - clip.timelineStart;
    const QMap<QString, QVariant> params =
        resolvedEffectParameters(timeEcho->resolvedAt(clipTimeUs), *def);
    int frameCount = qBound(1, params.value(QStringLiteral("frames"), 4).toInt(), 10);
    if (maxTimeEchoHistoryFrames >= 0)
        frameCount = qMin(frameCount, maxTimeEchoHistoryFrames);
    const double decay = qBound(0.0, params.value(QStringLiteral("decay"), 0.55).toDouble(), 1.0);
    const auto blendMode =
        CompositorFrameHistory::parseEchoBlendMode(params.value(QStringLiteral("blendMode")).toString());

    const TonDron::TimeUs frameStepUs = TonDron::frameDurationUs(projectFps);
    QList<QImage> samples;
    samples.reserve(frameCount + 1);

    const QImage current = decodeClipMediaFrame(clip, timelineUs, maxWidth, maxHeight);
    if (current.isNull())
        return {};
    samples.append(current);

    for (int i = 1; i <= frameCount; ++i) {
        const TonDron::TimeUs pastClipUs = clipTimeUs - static_cast<TonDron::TimeUs>(i) * frameStepUs;
        if (pastClipUs < 0)
            break;
        const QImage past =
            decodeClipMediaFrame(clip, clip.timelineStart + pastClipUs, maxWidth, maxHeight);
        if (!past.isNull())
            samples.append(past);
    }

    return CompositorFrameHistory::applyTimeEcho(samples, decay, blendMode);
}

// Prefer NV12 for plain video (preview upload path); fall back to RGBA QImage
// when time_echo needs CPU blending or NV12 decode fails.
void fillGpuLayerPixels(GpuLayer &layer, const TonDron::Clip &clip, TonDron::TimeUs timelineUs, int maxWidth,
                        int maxHeight, int projectFps, int maxTimeEchoHistoryFrames)
{
    if (clip.path.isEmpty())
        return;

    const TonDron::Effect *timeEcho = findTimeEchoEffect(clip.effects);
    if (!timeEcho && clip.type == TonDron::ClipType::Video) {
        const TonDron::VideoRead read = TonDron::resolveVideoRead(clip, timelineUs);
        const Nv12Frame nv12 =
            ClipReaderPool::instance().readVideoFrameNv12(read.path, read.sourceUs, maxWidth, maxHeight);
        if (nv12.isValid()) {
            layer.nv12 = nv12.data;
            layer.nv12Width = nv12.width;
            layer.nv12Height = nv12.height;
            return;
        }
    }

    layer.source = gpuSourceForClip(clip, timelineUs, maxWidth, maxHeight, projectFps,
                                    maxTimeEchoHistoryFrames);
}

// The word the playhead sits on, for styles whose accent rule follows the speech. -1 for every
// other rule, which keeps their raster time-independent and therefore cached across the clip.
int karaokeWordIndex(const TonDron::Clip &clip, TonDron::TimeUs timelineUs)
{
    if (clip.textStyle.accent.rule != TonDron::WordAccentRule::Karaoke)
        return -1;
    const QString text = clip.textContent.isEmpty() ? clip.name : clip.textContent;
    return TonDron::activeWordIndexAt(text, clip.timelineStart,
                                    clip.timelineStart + clip.timelineDuration, timelineUs);
}

int karaokeWordIndex(const TonDron::Clip &clip, const TonDron::SubtitleCue &cue, TonDron::TimeUs localUs)
{
    if (clip.textStyle.accent.rule != TonDron::WordAccentRule::Karaoke)
        return -1;
    return TonDron::activeWordIndexAt(cue.text, cue.startUs, cue.endUs, localUs);
}

// CapCut-style body intro/outro: opacity/offset/scale/rotation on top of fades and text anims.
void applyClipBodyAnimation(const TonDron::Clip &clip, TonDron::TimeUs timelineUs, double layoutW,
                            double layoutH, QRectF *destRect, double *opacity, double *rotation)
{
    if (!destRect || !opacity || !rotation)
        return;
    if (clip.type == TonDron::ClipType::Audio || clip.type == TonDron::ClipType::Subtitle)
        return;
    if (clip.animIn.kind == TonDron::ClipAnimKind::None && clip.animOut.kind == TonDron::ClipAnimKind::None)
        return;

    const TonDron::ClipAnimSample body =
        TonDron::evaluateClipAnimation(clip.timelineStart, clip.timelineDuration, clip.animIn,
                                     clip.animOut, timelineUs, layoutW, layoutH);
    *opacity *= body.opacity;
    destRect->translate(body.dx, body.dy);
    if (!qFuzzyCompare(body.scale, 1.0)) {
        const QPointF centre = destRect->center();
        destRect->setSize(destRect->size() * body.scale);
        destRect->moveCenter(centre);
    }
    *rotation += body.rotationDeg;
}

GpuLayer buildGpuLayer(const TonDron::Clip &clip, TonDron::TimeUs timelineUs, int projectWidth,
                       int projectHeight, double renderScale, int canvasWidth, int canvasHeight,
                       int projectFps, int maxTimeEchoHistoryFrames)
{
    GpuLayer layer;

    const TonDron::TimeUs clipTimeUs = timelineUs - clip.timelineStart;

    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
    double rotation = 0.0;
    layoutRectForClip(clip, timelineUs, projectWidth, projectHeight, renderScale, 1.0, &x, &y, &w, &h,
                      &rotation);
    if (w <= 0.5 || h <= 0.5)
        return layer;

    const int layoutW = qMax(1, qRound(w));
    const int layoutH = qMax(1, qRound(h));

    const QRectF layoutRect(x, y, w, h);
    QRectF destRect = layoutRect;
    double opacity = opacityForClip(clip, timelineUs);

    if (clip.type == TonDron::ClipType::Text) {
        // The raster carries a bleed margin for the stroke, shadow and box, so its destination rect
        // is wider than the layout rect. Entrance/exit motion rides on the layer, not the pixels.
        const TextRasterResult raster =
            rasterizeText(clip, layoutRect, renderScale, karaokeWordIndex(clip, timelineUs));
        const TextAnimSample anim = sampleTextAnimation(clip, timelineUs, layoutRect, renderScale);

        layer.source = raster.image;
        layer.effects = resolvedClipEffects(clip, clipTimeUs);

        destRect = raster.rect.translated(anim.dx, anim.dy);
        if (!qFuzzyCompare(anim.scale, 1.0)) {
            const QPointF centre = destRect.center();
            destRect.setSize(destRect.size() * anim.scale);
            destRect.moveCenter(centre);
        }
        opacity *= anim.opacity;

        if (anim.blurPx > 0.5) {
            TonDron::Effect blur;
            blur.catalogId = QStringLiteral("builtin.effects.gaussian_blur");
            blur.parameters.insert(QStringLiteral("u_blurRadius"), anim.blurPx);
            layer.effects.append(blur);
        }
    } else if (clip.type == TonDron::ClipType::Subtitle) {
        const TonDron::TimeUs localUs = timelineUs - clip.timelineStart;
        const TonDron::SubtitleCue *cue = activeSubtitleCueAt(clip.subtitleCues, localUs);
        if (!cue || cue->text.trimmed().isEmpty())
            return layer;

        const TextRasterResult raster =
            rasterizeText(clip, cue->text, layoutRect, renderScale,
                          karaokeWordIndex(clip, *cue, localUs));
        if (raster.image.isNull())
            return layer;

        // Each cue animates in and out on its own window, so cues play one after another.
        const TextAnimSample anim = sampleSubtitleCueAnimation(clip, *cue, timelineUs, layoutRect,
                                                               renderScale);

        layer.source = raster.image;
        layer.effects = resolvedClipEffects(clip, clipTimeUs);

        destRect = raster.rect.translated(anim.dx, anim.dy);
        if (!qFuzzyCompare(anim.scale, 1.0)) {
            const QPointF centre = destRect.center();
            destRect.setSize(destRect.size() * anim.scale);
            destRect.moveCenter(centre);
        }
        opacity *= anim.opacity;

        if (anim.blurPx > 0.5) {
            TonDron::Effect blur;
            blur.catalogId = QStringLiteral("builtin.effects.gaussian_blur");
            blur.parameters.insert(QStringLiteral("u_blurRadius"), anim.blurPx);
            layer.effects.append(blur);
        }
    } else if (clip.type == TonDron::ClipType::Shape) {
        layer.source = shapeImageForClip(clip, layoutW, layoutH, renderScale);
        layer.effects = resolvedClipEffects(clip, clipTimeUs);
    } else {
        // Bounded by the canvas, not the layout rect — see decodeClipMediaFrame.
        fillGpuLayerPixels(layer, clip, timelineUs, canvasWidth, canvasHeight, projectFps,
                           maxTimeEchoHistoryFrames);
        layer.effects = resolvedClipEffects(clip, clipTimeUs);
    }

    if (!layer.hasPixels())
        return layer;

    applyClipBodyAnimation(clip, timelineUs, w, h, &destRect, &opacity, &rotation);

    layer.mask = clip.mask;
    if (clip.mask.shape == TonDron::MaskShape::Matte && !clip.mask.mattePath.isEmpty()) {
        // The matte covers the segmented source range, so it starts at matteSrcOffsetUs.
        const TonDron::TimeUs matteUs =
            clip.timelineToSourceUs(timelineUs) - clip.mask.matteSrcOffsetUs;
        const QImage matte = ClipReaderPool::instance().readVideoFrame(
            clip.mask.mattePath, qMax<TonDron::TimeUs>(0, matteUs), canvasWidth, canvasHeight);
        // A missing matte frame must not silently blank the clip — leave the layer unmasked.
        if (!matte.isNull())
            layer.matte = matte;
    }
    layer.rect = destRect;
    layer.rotation = rotation;
    layer.flipH = clip.flipH;
    layer.flipV = clip.flipV;
    layer.opacity = opacity;
    layer.clipTimeUs = timelineUs - clip.timelineStart;
    layer.faceSlots = faceSlotsForClip(clip, layer.effects, timelineUs);
    layer.valid = true;
    return layer;
}

// The reveal granularity in effect for a text clip: the entrance's unit, or the exit's if the
// entrance is whole-block. TextAnimUnit::Block means the whole-layer path (buildGpuLayer) is used.
TonDron::TextAnimUnit activeSpanUnit(const TonDron::TextStyle &style)
{
    if (style.animIn.kind != TonDron::TextAnimKind::None && style.animIn.unit != TonDron::TextAnimUnit::Block)
        return style.animIn.unit;
    if (style.animOut.kind != TonDron::TextAnimKind::None && style.animOut.unit != TonDron::TextAnimUnit::Block)
        return style.animOut.unit;
    return TonDron::TextAnimUnit::Block;
}

// Build one GpuItem per reveal span (character / word / line) of a text clip, so the entrance/exit
// staggers across the block. Mirrors the text branch of buildGpuLayer, but each span is its own
// layer carrying its own sampled transform. Returns empty for whole-block text (use buildGpuLayer).
QList<GpuItem> buildTextSpanItems(const TonDron::Clip &clip, TonDron::TimeUs timelineUs, int projectWidth,
                                  int projectHeight, double renderScale, TonDron::TextAnimUnit unit)
{
    QList<GpuItem> items;

    const TonDron::TimeUs clipTimeUs = timelineUs - clip.timelineStart;

    double x = 0.0, y = 0.0, w = 0.0, h = 0.0, rotation = 0.0;
    layoutRectForClip(clip, timelineUs, projectWidth, projectHeight, renderScale, 1.0, &x, &y, &w, &h,
                      &rotation);
    if (w <= 0.5 || h <= 0.5)
        return items;
    const QRectF layoutRect(x, y, w, h);

    const QString text = clip.textContent.isEmpty() ? clip.name : clip.textContent;
    const QList<TextSpanRaster> spans = rasterizeTextSpans(clip, text, layoutRect, renderScale, unit,
                                                           karaokeWordIndex(clip, timelineUs));
    if (spans.isEmpty())
        return items;

    const double clipOpacity = opacityForClip(clip, timelineUs);
    const QList<TonDron::Effect> baseEffects = resolvedClipEffects(clip, clipTimeUs);
    int spanCount = 0;
    for (const TextSpanRaster &s : spans)
        spanCount = qMax(spanCount, s.count);

    for (const TextSpanRaster &span : spans) {
        if (span.image.isNull())
            continue;

        GpuItem item;
        item.blend = clip.blendMode;
        GpuLayer &layer = item.layer;
        layer.source = span.image;
        layer.effects = baseEffects;

        QRectF destRect = span.rect;
        double opacity = clipOpacity;

        // index == -1 is the static box background: no per-span motion, always visible behind glyphs.
        if (span.index >= 0) {
            const TextAnimSample anim =
                sampleTextSpanAnimation(clip, timelineUs, span.index, spanCount, layoutRect, renderScale);
            destRect.translate(anim.dx, anim.dy);
            if (!qFuzzyCompare(anim.scale, 1.0)) {
                const QPointF centre = destRect.center();
                destRect.setSize(destRect.size() * anim.scale);
                destRect.moveCenter(centre);
            }
            opacity *= anim.opacity;
            if (anim.blurPx > 0.5) {
                TonDron::Effect blur;
                blur.catalogId = QStringLiteral("builtin.effects.gaussian_blur");
                blur.parameters.insert(QStringLiteral("u_blurRadius"), anim.blurPx);
                layer.effects.append(blur);
            }
            if (opacity <= 0.001)
                continue; // a span that has not entered (or has fully exited) draws nothing
        }

        double spanRotation = rotation;
        applyClipBodyAnimation(clip, timelineUs, w, h, &destRect, &opacity, &spanRotation);
        if (opacity <= 0.001)
            continue;

        // Clip masks are layer-relative, so applying one here would stamp the whole shape onto every
        // span. Kinetic text + mask is rare; spans are left unmasked rather than mask each glyph.
        layer.rect = destRect;
        layer.rotation = spanRotation;
        layer.flipH = clip.flipH;
        layer.flipV = clip.flipV;
        layer.opacity = opacity;
        layer.clipTimeUs = clipTimeUs;
        layer.faceSlots = faceSlotsForClip(clip, layer.effects, timelineUs);
        layer.valid = true;
        items.append(item);
    }
    return items;
}

GpuScene buildGpuScene(const TonDron::Project &project, TonDron::TimeUs timelineUs, int width, int height,
                       double renderScale, const FrameCompositor::RenderOptions &options)
{
    GpuScene scene;
    scene.canvasSize = QSize(width, height);

    const int projectWidth = project.width();
    const int projectHeight = project.height();
    const int fps = project.fps();

    const TonDron::Background &bg = project.background();
    if (bg.kind == TonDron::BackgroundKind::Blur) {
        scene.backgroundColor = Qt::black;
        scene.backgroundBlur = true;
        scene.blurStrengthPx = bg.blurStrength;
        // The bottommost visual frame, decoded once — the CPU path decoded it a
        // second time here, effects and all.
        scene.blurSource = bottommostVisualFrame(project, timelineUs, width, height);
    } else {
        scene.backgroundColor = bg.color.isValid() ? bg.color : QColor(Qt::black);
    }

    // Track 0 is topmost and composites in front, so emit back-to-front.
    const QList<TonDron::Track> &tracks = project.tracks();
    for (int ti = tracks.size() - 1; ti >= 0; --ti) {
        const TonDron::Track &track = tracks.at(ti);
        if (track.hidden || track.type == TonDron::TrackType::Audio)
            continue;

        QSet<QString> transitionClipIds;
        TonDron::TimeUs transitionStart = 0;
        TonDron::TimeUs transitionEnd = 0;
        const TonDron::Transition *activeTransition =
            TonDron::activeTransitionAt(track, timelineUs, transitionStart, transitionEnd);
        if (activeTransition) {
            const TonDron::Clip *fromClip = TonDron::clipById(track, activeTransition->fromClipId);
            const TonDron::Clip *toClip = TonDron::clipById(track, activeTransition->toClipId);
            if (fromClip && toClip) {
                GpuItem item;
                item.isTransition = true;
                item.from = buildGpuLayer(*fromClip, timelineUs, projectWidth, projectHeight, renderScale,
                                          width, height, fps, options.maxTimeEchoHistoryFrames);
                item.to = buildGpuLayer(*toClip, timelineUs, projectWidth, projectHeight, renderScale,
                                        width, height, fps, options.maxTimeEchoHistoryFrames);
                item.progress = TonDron::transitionProgress(timelineUs, transitionStart, transitionEnd);
                // Time is measured from the start of the transition window so a
                // shader's u_time is a pure function of window position, like
                // u_progress.
                item.transitionTimeUs = timelineUs - transitionStart;
                if (const TransitionPresetEntry *def = transitionDefForId(activeTransition->kindId);
                    def && def->gpu.valid) {
                    item.transitionKey = QLatin1String(kTransitionCacheKeyPrefix) + activeTransition->kindId;
                    item.transitionGpu = &def->gpu;
                    item.transitionParams = resolvedTransitionParameters(*activeTransition, *def);
                }
                scene.items.append(item);

                transitionClipIds.insert(fromClip->id);
                transitionClipIds.insert(toClip->id);
            }
        }

        for (const TonDron::Clip &clip : track.clips) {
            if (transitionClipIds.contains(clip.id) || !clip.containsTime(timelineUs))
                continue;
            // The clip being edited in place on the preview is hidden here so the
            // QML inline editor shows in its stead (true WYSIWYG, single path).
            if (!options.skipClipId.isEmpty() && clip.id == options.skipClipId)
                continue;

            // Text with a per-span reveal expands into one layer per character/word/line so the
            // entrance/exit can stagger across the block; everything else is a single layer.
            if (clip.type == TonDron::ClipType::Text) {
                const TonDron::TextAnimUnit unit = activeSpanUnit(clip.textStyle);
                if (unit != TonDron::TextAnimUnit::Block) {
                    scene.items.append(buildTextSpanItems(clip, timelineUs, projectWidth, projectHeight,
                                                          renderScale, unit));
                    continue;
                }
            }

            GpuItem item;
            item.blend = clip.blendMode;
            item.layer = buildGpuLayer(clip, timelineUs, projectWidth, projectHeight, renderScale, width,
                                       height, fps, options.maxTimeEchoHistoryFrames);
            if (item.layer.valid)
                scene.items.append(item);
        }
    }

    return scene;
}

} // namespace

QImage FrameCompositor::compositeAt(TonDron::TimeUs timelineUs) const
{
    return compositeAt(timelineUs, RenderOptions{});
}

bool FrameCompositor::prepare(TonDron::TimeUs timelineUs, const RenderOptions &options, GpuScene *sceneOut,
                              int *widthOut, int *heightOut, double *renderScaleOut) const
{
    if (!m_project)
        return false;

    const int projectWidth = m_project->width();
    const int projectHeight = m_project->height();
    const double renderScale = qBound(0.1, options.previewScale, 1.0);
    const int width = qMax(1, static_cast<int>(std::lround(projectWidth * renderScale)));
    const int height = qMax(1, static_cast<int>(std::lround(projectHeight * renderScale)));
    if (width <= 0 || height <= 0)
        return false;

    QSet<QString> videoPaths;
    QSet<QString> audioPaths;
    collectActivePaths(m_project, timelineUs, videoPaths, audioPaths);
    ClipReaderPool::instance().retainActivePaths(videoPaths, audioPaths);
    ClipReaderPool::instance().setReadAheadUs(options.readAheadUs);

    // Start every clip's decode before compositing anything, so they run in
    // parallel across the per-path worker threads rather than serially below.
    ClipReaderPool::instance().warmVideoFrames(
        collectVideoRequests(m_project, timelineUs, width, height));

    *widthOut = width;
    *heightOut = height;
    *renderScaleOut = renderScale;
    if (sceneOut)
        *sceneOut = buildGpuScene(*m_project, timelineUs, width, height, renderScale, options);
    return true;
}

QImage FrameCompositor::compositeAt(TonDron::TimeUs timelineUs, const RenderOptions &options) const
{
    GpuScene scene;
    int width = 0;
    int height = 0;
    double renderScale = 1.0;
    if (!prepare(timelineUs, options, &scene, &width, &height, &renderScale))
        return {};

    return GpuCompositor::render(scene);
}

GpuFrameTexture FrameCompositor::compositeToTextureAt(TonDron::TimeUs timelineUs,
                                                      const RenderOptions &options) const
{
    if (!GpuCompositor::isAvailable())
        return {};

    GpuScene scene;
    int width = 0;
    int height = 0;
    double renderScale = 1.0;
    if (!prepare(timelineUs, options, &scene, &width, &height, &renderScale))
        return {};

    return GpuCompositor::renderToTexture(scene);
}


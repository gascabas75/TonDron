#include "CompositorService.h"

#include <QMetaType>
#include <cmath>

namespace {
constexpr TonDron::TimeUs kMaxPreviewFrameStalenessUs = 100'000;
constexpr double kAdaptiveScaleMin = 0.25;
constexpr double kAdaptiveScaleStepDown = 0.75;
constexpr double kAdaptiveScaleStepUp = 1.25;
constexpr int kLateBeforeScaleDown = 2;
constexpr int kOnTimeBeforeScaleUp = 6;
// Composites at the start of a run open and seek the decoders and are reliably
// slower than the ones after them. Charging those to the adaptive scale used to
// downscale the first seconds of every playback.
constexpr int kWarmupFrames = 3;
// Floor for the caller's budget: at 60 fps two ticks is 33 ms, close enough to
// normal jitter that the scale would rattle up and down against it.
constexpr int kMinLateFrameBudgetMs = 40;
}

CompositorWorker::CompositorWorker(QObject *parent)
    : QObject(parent)
{
}

void CompositorWorker::composite(TonDron::TimeUs timeUs, FrameCompositor::RenderOptions options,
                                 std::shared_ptr<const TonDron::Project> snapshot)
{
    // Keep the shared tree alive for the whole frame; setProject only borrows.
    m_snapshot = std::move(snapshot);
    if (!m_snapshot) {
        // Still report completion: the service treats a request as in flight
        // until the worker answers, and the quality-mode play loop waits for it.
        emit frameReady(GpuFrameTexture{}, timeUs);
        return;
    }
    m_compositor.setProject(m_snapshot.get());

    emit frameReady(m_compositor.compositeToTextureAt(timeUs, options), timeUs);
}

CompositorService::CompositorService(QObject *parent)
    : QObject(parent)
    , m_worker(new CompositorWorker)
{
    qRegisterMetaType<TonDron::TimeUs>("TonDron::TimeUs");
    qRegisterMetaType<std::shared_ptr<const TonDron::Project>>("std::shared_ptr<const TonDron::Project>");
    qRegisterMetaType<FrameCompositor::RenderOptions>("FrameCompositor::RenderOptions");
    qRegisterMetaType<GpuFrameTexture>("GpuFrameTexture");
    m_worker->moveToThread(&m_thread);
    connect(m_worker, &CompositorWorker::frameReady, this, &CompositorService::onWorkerFrameReady,
            Qt::QueuedConnection);
    m_thread.start();
}

CompositorService::~CompositorService()
{
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
    m_worker = nullptr;
}

void CompositorService::setProject(const TonDron::Project *project)
{
    m_project = project;
    invalidateSnapshot();
}

void CompositorService::invalidateSnapshot()
{
    ++m_liveGeneration;
    m_sharedSnapshot.reset();
}

void CompositorService::setDropLateFrames(bool drop)
{
    if (m_dropLateFrames == drop)
        return;
    m_dropLateFrames = drop;
    // Quality mode renders every frame at the requested scale; leaving a
    // downscale from a previous fast-mode run would defeat the point.
    resetAdaptiveState();
}

void CompositorService::setAdaptiveQuality(bool enabled)
{
    if (m_adaptiveQuality == enabled)
        return;
    m_adaptiveQuality = enabled;
    // Switching away hands the caller its requested scale back immediately;
    // switching on starts from full rather than from a stale measurement.
    resetAdaptiveState();
}

void CompositorService::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    // The scale itself survives across runs — a machine that could not keep up a
    // moment ago still cannot, and relearning that on every play would drop the
    // preview a second or two into each one. Only the streaks restart.
    m_lateStreak = 0;
    m_onTimeStreak = 0;
    m_warmupFramesLeft = active ? kWarmupFrames : 0;
}

void CompositorService::setLateFrameBudgetMs(int ms)
{
    m_lateFrameBudgetMs = qMax(kMinLateFrameBudgetMs, ms);
}

double CompositorService::adaptiveScaleFactor() const
{
    return m_adaptiveScale;
}

void CompositorService::resetAdaptiveState()
{
    m_adaptiveScale = 1.0;
    m_lateStreak = 0;
    m_onTimeStreak = 0;
    m_warmupFramesLeft = 0;
}

FrameCompositor::RenderOptions CompositorService::effectiveOptions(
    FrameCompositor::RenderOptions options) const
{
    if (!m_adaptiveQuality)
        return options;
    options.previewScale = qBound(0.1, options.previewScale * m_adaptiveScale, 1.0);
    return options;
}

void CompositorService::noteFrameLate(bool late)
{
    if (m_warmupFramesLeft > 0) {
        --m_warmupFramesLeft;
        return;
    }

    if (late) {
        m_onTimeStreak = 0;
        ++m_lateStreak;
        if (m_lateStreak >= kLateBeforeScaleDown && m_adaptiveScale > kAdaptiveScaleMin + 1e-6) {
            m_adaptiveScale = qMax(kAdaptiveScaleMin, m_adaptiveScale * kAdaptiveScaleStepDown);
            m_lateStreak = 0;
        }
        return;
    }

    m_lateStreak = 0;
    ++m_onTimeStreak;
    if (m_onTimeStreak >= kOnTimeBeforeScaleUp && m_adaptiveScale < 1.0 - 1e-6) {
        m_adaptiveScale = qMin(1.0, m_adaptiveScale * kAdaptiveScaleStepUp);
        m_onTimeStreak = 0;
    }
}

void CompositorService::dispatch(TonDron::TimeUs timeUs, const FrameCompositor::RenderOptions &options)
{
    if (!m_project)
        return;

    if (!m_sharedSnapshot || m_snapshotGeneration != m_liveGeneration) {
        // One uniquely-owned snapshot per generation; subsequent ticks only bump
        // the shared_ptr. Plain Project copy would keep sharing QMap/QList with
        // the live tree — unsafe once the GUI mutates while the worker reads.
        m_sharedSnapshot = std::make_shared<TonDron::Project>(m_project->detachedCopy());
        m_snapshotGeneration = m_liveGeneration;
    }

    m_renderElapsed.start();
    QMetaObject::invokeMethod(m_worker, "composite", Qt::QueuedConnection,
                              Q_ARG(TonDron::TimeUs, timeUs),
                              Q_ARG(FrameCompositor::RenderOptions, options),
                              Q_ARG(std::shared_ptr<const TonDron::Project>, m_sharedSnapshot));
}

void CompositorService::requestComposite(TonDron::TimeUs timeUs, FrameCompositor::RenderOptions options)
{
    options.previewScale = qBound(0.1, options.previewScale, 1.0);

    // The pending scale is published as whole percent, and the catch-up dispatch in
    // onWorkerFrameReady reads it back as percent/100. Dispatching the unrounded value here
    // would leave the two permanently unequal (0.16667 vs 0.17), so every finished frame would
    // re-dispatch at a slightly different canvas size — the preview would flip between, say,
    // 120x213 and 122x218 on every frame, rebuilding the presentation ring's FBOs each time and
    // handing the scene graph freshly allocated (black) textures. Round-trip through the same
    // integer here so both paths ask for one size. What is stored and compared is the requested
    // scale; the adaptive multiplier is a discrete ratchet applied at dispatch, so both paths
    // still derive the same size from it until the ratchet deliberately moves.
    const int previewScalePercent =
        qBound(10, static_cast<int>(std::lround(options.previewScale * 100.0)), 100);
    options.previewScale = previewScalePercent / 100.0;

    m_pendingTimeUs.store(timeUs, std::memory_order_release);
    m_pendingPreviewScalePercent.store(previewScalePercent, std::memory_order_release);
    m_pendingMaxTimeEchoHistoryFrames.store(options.maxTimeEchoHistoryFrames, std::memory_order_release);
    m_pendingReadAheadUs.store(options.readAheadUs, std::memory_order_release);
    if (m_requestPending.exchange(true, std::memory_order_acq_rel))
        return;

    m_lastDispatchedTimeUs = timeUs;
    m_lastDispatchedOptions = options;
    dispatch(timeUs, effectiveOptions(options));
}

void CompositorService::onWorkerFrameReady(const GpuFrameTexture &frame, TonDron::TimeUs timeUs)
{
    const qint64 renderMs = m_renderElapsed.isValid() ? m_renderElapsed.elapsed() : 0;
    const TonDron::TimeUs latest = m_pendingTimeUs.load(std::memory_order_acquire);
    FrameCompositor::RenderOptions latestOptions;
    latestOptions.previewScale =
        static_cast<double>(m_pendingPreviewScalePercent.load(std::memory_order_acquire)) / 100.0;
    latestOptions.maxTimeEchoHistoryFrames =
        m_pendingMaxTimeEchoHistoryFrames.load(std::memory_order_acquire);
    latestOptions.readAheadUs = m_pendingReadAheadUs.load(std::memory_order_acquire);

    // Quality mode shows every frame it renders, however far behind the request
    // it finished; only fast mode discards frames the playhead has run past.
    const bool stale = m_dropLateFrames && latest > timeUs
        && latest - timeUs > kMaxPreviewFrameStalenessUs;
    // Whether the frame is worth showing and whether it was rendered fast enough
    // are different questions. Adaptation asks the second one, in wall time, and
    // only about frames with a deadline: a paused or scrubbed frame is never late.
    if (m_adaptiveQuality && m_dropLateFrames && m_playbackActive)
        noteFrameLate(renderMs > m_lateFrameBudgetMs);
    if (!stale && frame.isValid())
        emit frameReady(frame);

    m_requestPending.store(false, std::memory_order_release);

    if (latest != m_lastDispatchedTimeUs
        || latestOptions.previewScale != m_lastDispatchedOptions.previewScale
        || latestOptions.maxTimeEchoHistoryFrames != m_lastDispatchedOptions.maxTimeEchoHistoryFrames
        || latestOptions.readAheadUs != m_lastDispatchedOptions.readAheadUs) {
        m_lastDispatchedTimeUs = latest;
        m_lastDispatchedOptions = latestOptions;
        if (!m_requestPending.exchange(true, std::memory_order_acq_rel))
            dispatch(latest, effectiveOptions(latestOptions));
    }

    // Last: a listener may start the next composite from here, and that request
    // must not be overwritten by the catch-up dispatch above.
    emit compositeFinished();
}

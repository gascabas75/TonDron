#include <QtTest>

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include "core/ClipAnimation.h"
#include "core/Keyframe.h"
#include "core/Project.h"
#include "core/ShapePath.h"
#include "core/SrtIO.h"
#include "core/SubtitleCue.h"
#include "core/TimelineOps.h"
#include "core/Transition.h"

#include <cmath>

class CoreTest : public QObject
{
    Q_OBJECT

private slots:
    void timeConversion();
    void keyframeHoldInterpolation();
    void keyframeLinearInterpolation();
    void keyframeEaseInterpolation();
    void keyframeBezierTangents();
    void disabledKeyframesFreezeAtFirstKey();
    void legacyTrackInterpolationMigratesLosslessly();
    void keyframeNearestQuery();
    void projectSerializationRoundTrip();
    void projectMetadataRoundTrip();
    void effectColorParamSurvivesRoundTrip();
    void clipTransformSerialization();
    void legacyFractionalTransformMigration();
    void volumeKeyframeSerialization();
    void projectLoadsLegacyV1Format();
    void projectRejectsUnreadableDocuments();
    void trackAllowsClipTypes();
    void subtitleCueSerialization();
    void subtitleCueLookup();
    void subtitleCuePacking();
    void srtRoundTrip();
    void srtParseEdgeCases();
    void insertTrackAtTopAllowsDuplicateTypes();
    void multiTrackSerializationRoundTrip();
    void textStyleAndBlendModeSerialization();
    void legacyBoldMigratesToFontWeight();
    void textPresetsAreWellFormed();
    void karaokeWordIndexTracksTheCue();
    void shapeStyleSerialization();
    void legacyShapeStyleLoadsWithDefaults();
    void shapeCatalogPathsFitBounds();
    void effectCatalogIdSerialization();
    void effectParamKeyframeSerialization();
    void detachedCopyIsolatesKeyframesFromLiveMutations();
    void effectTemplateStackSerialization();
    void audioEffectSerialization();
    void rgbSplitEffectParametersSerialization();
    void blockGlitchEffectParametersSerialization();
    void clipSpeedSourceMapping();
    void speedCurveMatchesConstantSpeed();
    void speedCurveRampRetimesDuration();
    void speedCurveMappingIsMonotonic();
    void speedCurveSubRangePreservesShape();
    void speedCurveSerialization();
    void clipReverseAndFlipSerialization();
    void clipSplitMergeRoundTrip();
    void clipLinkFieldsSerialization();
    void maskAndTransitionSerialization();
    void matteMaskSerialization();
    void faceTrackSerialization();
    void emojiClipSerialization();
    void allTransitionKindsRoundTrip();
    void transitionParametersRoundTrip();
    void legacyTransitionJsonStillLoads();
    void transitionAudioCurves();
    void physicalOverlapTransitionWindow();
    void clampClipStartNoOverlapPushesPastBlockers();
    void clampTrimEdgesIgnoreExistingOverlaps();
    void backgroundSerialization();
    void fadeSerializationAndMultiplier();
    void clipAnimationSerializationAndSample();
    void rebaseClipLayoutFreezesImplicitSize();
    void rebaseClipLayoutShiftsKeyframedPosition();
};

void CoreTest::timeConversion()
{
    QCOMPARE(TonDron::secondsToUs(1.0), TonDron::TimeUs{1'000'000});
    QCOMPARE(TonDron::usToSeconds(2'500'000), 2.5);
    QCOMPARE(TonDron::frameDurationUs(30), TonDron::TimeUs{33'333});
}

void CoreTest::keyframeHoldInterpolation()
{
    TonDron::KeyframeTrack<double> track;
    track.setKeyframe(0, 0.0);
    track.setKeyframe(TonDron::secondsToUs(2.0), 1.0);
    // Hold is a property of the key you are leaving, not of the whole track.
    track.setEasing(0, TonDron::Interpolation::Hold);
    QCOMPARE(track.evaluateAt(TonDron::secondsToUs(1.5)), 0.0);
    QCOMPARE(track.evaluateAt(TonDron::secondsToUs(2.0)), 1.0);
}

void CoreTest::keyframeLinearInterpolation()
{
    TonDron::KeyframeTrack<double> track;
    track.setKeyframe(0, 0.0);
    track.setKeyframe(TonDron::secondsToUs(2.0), 1.0);
    QCOMPARE(track.evaluateAt(TonDron::secondsToUs(1.0)), 0.5);
}

void CoreTest::disabledKeyframesFreezeAtFirstKey()
{
    TonDron::KeyframeTrack<double> track;
    track.setKeyframe(0, 0.0);
    track.setKeyframe(TonDron::secondsToUs(2.0), 1.0);
    QVERIFY(track.enabled());

    track.setEnabled(false);
    // Every sample reads as the first key while the animation is parked...
    QCOMPARE(track.evaluateAt(TonDron::secondsToUs(1.0)), 0.0);
    QCOMPARE(track.evaluateAt(TonDron::secondsToUs(2.0)), 0.0);
    // ...and the keys themselves are untouched, so switching back on restores the curve exactly.
    QCOMPARE(track.keyframes().size(), 2);
    track.setEnabled(true);
    QCOMPARE(track.evaluateAt(TonDron::secondsToUs(1.0)), 0.5);

    // The switch survives a save/load round trip.
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-kf");
    clip.type = TonDron::ClipType::Text;
    clip.timelineDuration = TonDron::secondsToUs(3.0);
    clip.opacity = track;
    clip.opacity.setEnabled(false);
    project.tracks()[0].clips.append(clip);

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
    QVERIFY(error.isEmpty());
    const TonDron::KeyframeTrack<double> &loadedTrack = loaded.tracks().at(0).clips.at(0).opacity;
    QVERIFY(!loadedTrack.enabled());
    QCOMPARE(loadedTrack.keyframes().size(), 2);
    // A project written before the switch existed loads with its animations on.
    QVERIFY(loaded.tracks().at(0).clips.at(0).transformX.enabled());
}

void CoreTest::keyframeBezierTangents()
{
    TonDron::KeyframeTrack<double> track;
    track.setKeyframe(0, 0.0);
    track.setKeyframe(TonDron::secondsToUs(1.0), 10.0);

    // Sharp attack, long settle: the out-handle of the first key held flat and far to the
    // right pushes the curve above the straight line for most of the segment.
    TonDron::Keyframe<double> *first = track.keyframeRef(0);
    QVERIFY(first != nullptr);
    first->outDx = TonDron::secondsToUs(0.8);
    first->outDy = 9.0;
    QVERIFY(track.evaluateAt(TonDron::secondsToUs(0.25)) > 2.5);

    // The endpoints stay pinned no matter what the handles do.
    QCOMPARE(track.evaluateAt(0), 0.0);
    QCOMPARE(track.evaluateAt(TonDron::secondsToUs(1.0)), 10.0);

    // A handle reaching past the segment must not fold the curve back on itself: time still
    // maps to exactly one value, so the result stays monotonic in a monotonic segment.
    first->outDx = TonDron::secondsToUs(5.0);
    first->outDy = 0.0;
    double prevValue = -1.0;
    for (int i = 0; i <= 20; ++i) {
        const double v = track.evaluateAt(TonDron::secondsToUs(i / 20.0));
        QVERIFY2(v >= prevValue - 1e-9, qPrintable(QStringLiteral("folded at %1").arg(i)));
        prevValue = v;
    }

    // Custom tangents match no preset, which is what leaves the chips unlit.
    QVERIFY(track.hasCustomTangents(0));
    track.setEasing(0, TonDron::Interpolation::Linear);
    QVERIFY(!track.hasCustomTangents(0));
}

void CoreTest::legacyTrackInterpolationMigratesLosslessly()
{
    // A project written before keyframes had tangents: one mode for the whole track.
    const auto legacyJson = [](const QString &mode) {
        return QJsonObject{
            {QStringLiteral("interpolation"), mode},
            {QStringLiteral("keyframes"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("timeUs"), 0.0}, {QStringLiteral("value"), 0.0}},
                 QJsonObject{{QStringLiteral("timeUs"), 1'000'000.0},
                             {QStringLiteral("value"), 10.0}},
             }},
        };
    };

    // Build a real project, serialize it, then rewrite the keyframe block into the legacy
    // shape — so the loader is exercised exactly as it would be on an old file.
    TonDron::Project project;
    project.tracks().append(TonDron::Track{});
    TonDron::Clip clip;
    clip.id = QStringLiteral("c1");
    clip.timelineDuration = TonDron::secondsToUs(2.0);
    TonDron::Effect effect;
    effect.catalogId = QStringLiteral("adjust.contrast");
    TonDron::KeyframeTrack<double> seed;
    seed.setKeyframe(0, 0.0);
    seed.setKeyframe(TonDron::secondsToUs(1.0), 10.0);
    effect.paramKeyframes.insert(QStringLiteral("contrast"), seed);
    clip.effects.append(effect);
    project.tracks()[0].clips.append(clip);
    const QJsonObject baseJson = project.toJson();

    struct Case { const char *mode; double at0_25; };
    const Case cases[] = {
        {"linear", 2.5},     // straight line
        {"ease", 1.5625},    // smoothstep(0.25) * 10
        {"hold", 0.0},       // steps at the next key
    };

    for (const Case &c : cases) {
        QJsonObject projectJson = baseJson;
        QJsonArray tracks = projectJson.value(QStringLiteral("tracks")).toArray();
        QJsonObject trackJson = tracks[0].toObject();
        QJsonArray clips = trackJson.value(QStringLiteral("clips")).toArray();
        QJsonObject clipJson = clips[0].toObject();
        QJsonArray effects = clipJson.value(QStringLiteral("effects")).toArray();
        QJsonObject effectJson = effects[0].toObject();
        QJsonObject params;
        params.insert(QStringLiteral("contrast"), legacyJson(QString::fromLatin1(c.mode)));
        effectJson.insert(QStringLiteral("paramKeyframes"), params);
        effects[0] = effectJson;
        clipJson.insert(QStringLiteral("effects"), effects);
        clips[0] = clipJson;
        trackJson.insert(QStringLiteral("clips"), clips);
        tracks[0] = trackJson;
        projectJson.insert(QStringLiteral("tracks"), tracks);

        QString error;
        const TonDron::Project loaded = TonDron::Project::fromJson(projectJson, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        // The loader may materialise default tracks, so find the clip rather than index into it.
        const TonDron::Clip *found = nullptr;
        for (const TonDron::Track &t : loaded.tracks()) {
            for (const TonDron::Clip &cl : t.clips) {
                if (!cl.effects.isEmpty())
                    found = &cl;
            }
        }
        QVERIFY(found != nullptr);

        const TonDron::KeyframeTrack<double> &kt =
            found->effects[0].paramKeyframes.value(QStringLiteral("contrast"));
        QCOMPARE(kt.keyframes().size(), 2);
        const double got = kt.evaluateAt(TonDron::secondsToUs(0.25));
        QVERIFY2(std::abs(got - c.at0_25) < 0.01,
                 qPrintable(QStringLiteral("%1: got %2, expected %3")
                                .arg(QString::fromLatin1(c.mode)).arg(got).arg(c.at0_25)));
    }
}

void CoreTest::keyframeEaseInterpolation()
{
    TonDron::KeyframeTrack<double> track;
    track.setKeyframe(0, 0.0);
    track.setKeyframe(TonDron::secondsToUs(1.0), 10.0);
    track.setEasing(0, TonDron::Interpolation::Ease);
    track.setEasing(TonDron::secondsToUs(1.0), TonDron::Interpolation::Ease);

    // The Ease preset is flat tangents a third of the way to each neighbour, which is exactly
    // the smoothstep the old track-wide mode produced: t*t*(3-2t) at t=0.25 is 0.15625.
    const double eased = track.evaluateAt(TonDron::secondsToUs(0.25));
    QVERIFY2(std::abs(eased - 1.5625) < 0.01,
             qPrintable(QStringLiteral("eased %1, expected 1.5625").arg(eased)));
    QCOMPARE(TonDron::interpolationToString(TonDron::Interpolation::Ease), QStringLiteral("ease"));
    QCOMPARE(TonDron::interpolationFromString(QStringLiteral("ease")), TonDron::Interpolation::Ease);

    // Zero-length handles are a straight line, because x and y then share blend weights.
    TonDron::KeyframeTrack<double> linear;
    linear.setKeyframe(0, 0.0);
    linear.setKeyframe(TonDron::secondsToUs(1.0), 10.0);
    QCOMPARE(linear.evaluateAt(TonDron::secondsToUs(0.25)), 2.5);
}

void CoreTest::keyframeNearestQuery()
{
    TonDron::KeyframeTrack<double> track;
    track.setKeyframe(TonDron::secondsToUs(1.0), 5.0);
    QCOMPARE(track.nearestKeyframe(TonDron::secondsToUs(1.01), TonDron::secondsToUs(0.05)),
             TonDron::secondsToUs(1.0));
    QCOMPARE(track.nearestKeyframe(TonDron::secondsToUs(2.0), TonDron::secondsToUs(0.05)), TonDron::TimeUs{-1});
}

void CoreTest::projectMetadataRoundTrip()
{
    TonDron::Project project;
    const QString id = project.id();
    QVERIFY(!id.isEmpty());

    project.setName(QStringLiteral("Documentary"));
    project.setAuthor(QStringLiteral("Ada"));
    project.setDescription(QStringLiteral("Rough cut"));
    const QDateTime created(QDate(2026, 3, 4), QTime(5, 6, 7), QTimeZone::UTC);
    project.setCreatedAt(created);
    project.setModifiedAt(created.addDays(2));

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.id(), id);
    QCOMPARE(loaded.name(), QStringLiteral("Documentary"));
    QCOMPARE(loaded.author(), QStringLiteral("Ada"));
    QCOMPARE(loaded.description(), QStringLiteral("Rough cut"));
    QCOMPARE(loaded.createdAt(), created);
    QCOMPARE(loaded.modifiedAt(), created.addDays(2));

    // An empty timeline routes through resetToDefaultTimeline() during the load, which mints a
    // fresh id — the saved one has to survive that.
    QVERIFY(loaded.tracks().size() > 0);
    QCOMPARE(TonDron::Project::fromJson(loaded.toJson(), &error).id(), id);

    // Two fresh projects are distinct documents, not the same one.
    QVERIFY(TonDron::Project().id() != TonDron::Project().id());
}

void CoreTest::projectSerializationRoundTrip()
{
    TonDron::Project project;
    project.setName(QStringLiteral("Test Project"));
    project.setFps(24);
    project.setResolution(1280, 720);

    TonDron::MediaAsset asset;
    asset.name = QStringLiteral("clip.mp4");
    asset.kind = TonDron::MediaKind::Video;
    asset.path = QStringLiteral("/tmp/clip.mp4");
    asset.durationUs = TonDron::secondsToUs(10.0);
    const QString assetId = project.addAsset(asset);

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-1");
    clip.assetId = assetId;
    clip.type = TonDron::ClipType::Video;
    clip.name = asset.name;
    clip.path = asset.path;
    clip.timelineStart = TonDron::secondsToUs(1.0);
    clip.timelineDuration = TonDron::secondsToUs(5.0);
    clip.srcIn = 0;
    clip.srcOut = TonDron::secondsToUs(5.0);
    project.tracks()[0].clips.append(clip);

    project.bookmarks().append({.timeUs = TonDron::secondsToUs(3.0), .label = QStringLiteral("Mark")});
    project.setWorkAreaInUs(TonDron::secondsToUs(1.0));
    project.setWorkAreaOutUs(TonDron::secondsToUs(4.0));

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.name(), project.name());
    QCOMPARE(loaded.fps(), 24);
    QCOMPARE(loaded.width(), 1280);
    QCOMPARE(loaded.tracks().size(), 1);
    QCOMPARE(loaded.tracks()[0].clips.size(), 1);
    QCOMPARE(loaded.tracks()[0].clips[0].timelineStart, clip.timelineStart);
    QCOMPARE(loaded.bookmarks().size(), 1);
    QCOMPARE(loaded.bookmarks()[0].label, QStringLiteral("Mark"));
    QVERIFY(loaded.hasWorkArea());
    QCOMPARE(loaded.workAreaInUs(), TonDron::secondsToUs(1.0));
    QCOMPARE(loaded.workAreaOutUs(), TonDron::secondsToUs(4.0));
}

// A colour parameter is stored as a "#rrggbb" string rather than a number, so it has to survive the
// project file as one. Effect params round-trip through QVariant, and a silent coercion to double
// here would reach the shader as black.
void CoreTest::effectColorParamSurvivesRoundTrip()
{
    TonDron::Project project;

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-1");
    clip.type = TonDron::ClipType::Video;
    clip.timelineDuration = TonDron::secondsToUs(5.0);

    TonDron::Effect effect;
    effect.catalogId = QStringLiteral("face_lipstick");
    effect.parameters.insert(QStringLiteral("shade"), QStringLiteral("#b03048"));
    effect.parameters.insert(QStringLiteral("opacity"), 0.8);
    effect.parameters.insert(QStringLiteral("coverInner"), true);
    clip.effects.append(effect);
    project.tracks()[0].clips.append(clip);

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.tracks()[0].clips.size(), 1);
    const TonDron::Effect &out = loaded.tracks()[0].clips[0].effects.at(0);

    const QVariant shade = out.parameters.value(QStringLiteral("shade"));
    QCOMPARE(shade.typeId(), QMetaType::QString);
    QCOMPARE(shade.toString(), QStringLiteral("#b03048"));
    QCOMPARE(out.parameters.value(QStringLiteral("opacity")).toDouble(), 0.8);
    QCOMPARE(out.parameters.value(QStringLiteral("coverInner")).toBool(), true);
}

void CoreTest::clipTransformSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Text});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-transform");
    clip.type = TonDron::ClipType::Text;
    clip.name = QStringLiteral("Title");
    clip.textContent = QStringLiteral("Hello");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(3.0);
    clip.transformX.setKeyframe(0, 100.0);
    clip.transformY.setKeyframe(0, 200.0);
    clip.transformW.setKeyframe(0, 640.0);
    clip.transformH.setKeyframe(0, 360.0);
    clip.rotation.setKeyframe(0, 45.0);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.transformX.evaluateAt(0), 100.0);
    QCOMPARE(loadedClip.transformY.evaluateAt(0), 200.0);
    QCOMPARE(loadedClip.transformW.evaluateAt(0), 640.0);
    QCOMPARE(loadedClip.transformH.evaluateAt(0), 360.0);
    QCOMPARE(loadedClip.rotation.evaluateAt(0), 45.0);
}

void CoreTest::legacyFractionalTransformMigration()
{
    // Old projects stored center-normalized posX/posY + scale; load them as
    // top-left pixel layout on the project canvas.
    auto kf = [](double value) {
        return QJsonObject{
            {QStringLiteral("interpolation"), QStringLiteral("linear")},
            {QStringLiteral("keyframes"),
             QJsonArray{QJsonObject{{QStringLiteral("timeUs"), 0.0},
                                    {QStringLiteral("value"), value}}}},
        };
    };
    const QJsonObject root{
        {QStringLiteral("version"), 2},
        {QStringLiteral("projectName"), QStringLiteral("LegacyTransform")},
        {QStringLiteral("fps"), 30},
        {QStringLiteral("width"), 1920},
        {QStringLiteral("height"), 1080},
        {QStringLiteral("tracks"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("video")},
                 {QStringLiteral("clips"),
                  QJsonArray{
                      QJsonObject{
                          {QStringLiteral("id"), QStringLiteral("legacy-clip")},
                          {QStringLiteral("type"), QStringLiteral("video")},
                          {QStringLiteral("name"), QStringLiteral("v")},
                          {QStringLiteral("timelineStartUs"), 0},
                          {QStringLiteral("timelineDurationUs"), 1000000},
                          {QStringLiteral("srcInUs"), 0},
                          {QStringLiteral("srcOutUs"), 1000000},
                          {QStringLiteral("posX"), kf(0.5)},
                          {QStringLiteral("posY"), kf(0.5)},
                          {QStringLiteral("scale"), kf(1.0)},
                      },
                  }},
             },
         }},
    };

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(root, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!loaded.tracks().isEmpty());
    QVERIFY(!loaded.tracks()[0].clips.isEmpty());
    const TonDron::Clip &clip = loaded.tracks()[0].clips[0];
    QCOMPARE(clip.transformW.evaluateAt(0), 1920.0);
    QCOMPARE(clip.transformH.evaluateAt(0), 1080.0);
    QCOMPARE(clip.transformX.evaluateAt(0), 0.0);
    QCOMPARE(clip.transformY.evaluateAt(0), 0.0);
}

void CoreTest::volumeKeyframeSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Audio});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-volume");
    clip.type = TonDron::ClipType::Audio;
    clip.name = QStringLiteral("Audio");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(4.0);
    clip.volume.setKeyframe(0, 1.0);
    clip.volume.setKeyframe(TonDron::secondsToUs(2.0), 0.5);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.volume.evaluateAt(0), 1.0);
    QCOMPARE(loadedClip.volume.evaluateAt(TonDron::secondsToUs(2.0)), 0.5);
    QCOMPARE(loadedClip.volume.evaluateAt(TonDron::secondsToUs(1.0)), 0.75);
}

void CoreTest::projectLoadsLegacyV1Format()
{
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("projectName"), QStringLiteral("Legacy")},
        {QStringLiteral("assets"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("a.mp4")},
                 {QStringLiteral("kind"), QStringLiteral("video")},
                 {QStringLiteral("durationSeconds"), 12.0},
                 {QStringLiteral("path"), QStringLiteral("/tmp/a.mp4")},
             },
         }},
        {QStringLiteral("tracks"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("video")},
                 {QStringLiteral("clips"),
                  QJsonArray{
                      QJsonObject{
                          {QStringLiteral("name"), QStringLiteral("a.mp4")},
                          {QStringLiteral("kind"), QStringLiteral("video")},
                          {QStringLiteral("path"), QStringLiteral("/tmp/a.mp4")},
                          {QStringLiteral("start"), 1.0},
                          {QStringLiteral("duration"), 4.0},
                          {QStringLiteral("inPoint"), 0.5},
                          {QStringLiteral("outPoint"), 4.5},
                          {QStringLiteral("assetIndex"), 0},
                      },
                  }},
             },
             QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}},
             QJsonObject{{QStringLiteral("type"), QStringLiteral("audio")}},
         }},
    };

    QString error;
    const TonDron::Project project = TonDron::Project::fromJson(root, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(project.name(), QStringLiteral("Legacy"));
    QCOMPARE(project.tracks()[0].clips.size(), 1);
    QCOMPARE(project.tracks()[0].clips[0].timelineStart, TonDron::secondsToUs(1.0));
    QCOMPARE(project.tracks()[0].clips[0].srcIn, TonDron::secondsToUs(0.5));
    QVERIFY(!project.tracks()[0].clips[0].assetId.isEmpty());
}

// fromJson used to have no failure path at all: every field fell back to a default and the
// errorOut param was only ever cleared. Both of these loaded as a plausible-looking project.
void CoreTest::projectRejectsUnreadableDocuments()
{
    // A document from a future format. The .drift container revision is bumped separately, so
    // ProjectBundle's own gate does not catch this.
    {
        TonDron::Project project;
        QJsonObject json = project.toJson();
        json[QStringLiteral("version")] = TonDron::Project::kCurrentVersion + 1;

        QString error;
        TonDron::Project::fromJson(json, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(error.contains(QStringLiteral("newer version")));
    }

    // Not a project document. Anything without a tracks array used to come back as an empty
    // project named "Untitled Project", which could then be saved back over the original.
    {
        QString error;
        TonDron::Project::fromJson(QJsonObject{}, &error);
        QVERIFY(!error.isEmpty());

        error.clear();
        TonDron::Project::fromJson(QJsonObject{{QStringLiteral("hello"), QStringLiteral("world")}},
                                 &error);
        QVERIFY(!error.isEmpty());
    }

    // The current version, and an empty timeline, both still load.
    {
        TonDron::Project project;
        project.tracks().clear();
        QString error;
        const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(loaded.tracks().size(), 1); // empty timeline falls back to one video track
    }
}

void CoreTest::trackAllowsClipTypes()
{
    TonDron::Track videoTrack{.type = TonDron::TrackType::Video};
    QVERIFY(videoTrack.allowsClipType(TonDron::ClipType::Video));
    QVERIFY(!videoTrack.allowsClipType(TonDron::ClipType::Image));
    QVERIFY(!videoTrack.allowsClipType(TonDron::ClipType::Audio));

    TonDron::Track audioTrack{.type = TonDron::TrackType::Audio};
    QVERIFY(audioTrack.allowsClipType(TonDron::ClipType::Audio));
    QVERIFY(!audioTrack.allowsClipType(TonDron::ClipType::Video));

    TonDron::Track shapeTrack{.type = TonDron::TrackType::Shape};
    QVERIFY(shapeTrack.allowsClipType(TonDron::ClipType::Image));
    QVERIFY(shapeTrack.allowsClipType(TonDron::ClipType::Shape));
    QVERIFY(!shapeTrack.allowsClipType(TonDron::ClipType::Video));

    TonDron::Track subtitleTrack{.type = TonDron::TrackType::Subtitle};
    QVERIFY(subtitleTrack.allowsClipType(TonDron::ClipType::Subtitle));
    QVERIFY(!subtitleTrack.allowsClipType(TonDron::ClipType::Text));
}

void CoreTest::subtitleCueSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Subtitle});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-subtitle");
    clip.type = TonDron::ClipType::Subtitle;
    clip.name = QStringLiteral("Subtitles (2)");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(30.0);
    clip.subtitleCues = {
        {TonDron::secondsToUs(1.0), TonDron::secondsToUs(4.0), QStringLiteral("Hello")},
        {TonDron::secondsToUs(5.0), TonDron::secondsToUs(8.0), QStringLiteral("World")},
    };
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.type, TonDron::ClipType::Subtitle);
    QCOMPARE(loadedClip.subtitleCues.size(), 2);
    QCOMPARE(loadedClip.subtitleCues[0].text, QStringLiteral("Hello"));
    QCOMPARE(loadedClip.subtitleCues[1].startUs, TonDron::secondsToUs(5.0));
}

void CoreTest::subtitleCueLookup()
{
    QList<TonDron::SubtitleCue> cues;
    cues.append({TonDron::secondsToUs(1.0), TonDron::secondsToUs(3.0), QStringLiteral("A")});
    cues.append({TonDron::secondsToUs(4.0), TonDron::secondsToUs(6.0), QStringLiteral("B")});

    const TonDron::SubtitleCue *active =
        TonDron::activeSubtitleCueAt(cues, TonDron::secondsToUs(2.5));
    QVERIFY(active);
    QCOMPARE(active->text, QStringLiteral("A"));
    QVERIFY(!TonDron::activeSubtitleCueAt(cues, TonDron::secondsToUs(3.5)));
    QCOMPARE(TonDron::subtitleCueIndexAt(cues, TonDron::secondsToUs(5.0)), 1);
}

void CoreTest::subtitleCuePacking()
{
    // One long Whisper segment should become several ~42-char single-line cues.
    QList<TonDron::SubtitleCue> input;
    input.append({TonDron::secondsToUs(0.0), TonDron::secondsToUs(10.0),
                  QStringLiteral("Hello everyone welcome to the show today we will talk about "
                                 "video editing and automatic subtitles.")});

    const QList<TonDron::SubtitleCue> packed = TonDron::packSubtitleCues(input, 42, 1);
    QVERIFY(packed.size() >= 2);
    for (const TonDron::SubtitleCue &cue : packed) {
        // A single oversize token may exceed the width; otherwise stay within 42.
        QVERIFY(cue.text.size() <= 42 || !cue.text.contains(QLatin1Char(' ')));
        QVERIFY(cue.endUs > cue.startUs);
        QVERIFY(!cue.text.contains(QLatin1Char('\n')));
    }
    QCOMPARE(packed.first().startUs, TonDron::secondsToUs(0.0));
    QCOMPARE(packed.last().endUs, TonDron::secondsToUs(10.0));

    // Short cues under the limit stay as a single cue.
    QList<TonDron::SubtitleCue> shortInput;
    shortInput.append(
        {TonDron::secondsToUs(1.0), TonDron::secondsToUs(2.0), QStringLiteral("Hi there")});
    const QList<TonDron::SubtitleCue> shortPacked = TonDron::packSubtitleCues(shortInput, 42, 1);
    QCOMPARE(shortPacked.size(), 1);
    QCOMPARE(shortPacked.first().text, QStringLiteral("Hi there"));
}

void CoreTest::srtRoundTrip()
{
    QList<TonDron::SubtitleCue> cues;
    cues.append({TonDron::secondsToUs(1.0), TonDron::secondsToUs(4.0), QStringLiteral("Hello")});
    cues.append({TonDron::secondsToUs(65.5), TonDron::secondsToUs(70.25),
                 QStringLiteral("Line one\nLine two")});

    const QString srt = TonDron::writeSrt(cues);
    QVERIFY(srt.contains(QStringLiteral("00:00:01,000 --> 00:00:04,000")));
    QVERIFY(srt.contains(QStringLiteral("00:01:05,500 --> 00:01:10,250")));
    QVERIFY(srt.contains(QStringLiteral("Line one\nLine two")));

    QList<TonDron::SubtitleCue> loaded;
    QString error;
    QVERIFY(TonDron::parseSrt(srt, &loaded, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.size(), 2);
    QCOMPARE(loaded[0].text, QStringLiteral("Hello"));
    QCOMPARE(loaded[0].startUs, TonDron::secondsToUs(1.0));
    QCOMPARE(loaded[0].endUs, TonDron::secondsToUs(4.0));
    QCOMPARE(loaded[1].text, QStringLiteral("Line one\nLine two"));
    QCOMPARE(loaded[1].startUs, TonDron::secondsToUs(65.5));
    QCOMPARE(loaded[1].endUs, TonDron::secondsToUs(70.25));
}

void CoreTest::srtParseEdgeCases()
{
    // Dot milliseconds (common non-strict variant) and UTF-8 BOM.
    const QString srt = QStringLiteral("\uFEFF1\n00:00:00.500 --> 00:00:02.000\nCafé\n");
    QList<TonDron::SubtitleCue> cues;
    QString error;
    QVERIFY(TonDron::parseSrt(srt, &cues, &error));
    QCOMPARE(cues.size(), 1);
    QCOMPARE(cues[0].text, QStringLiteral("Café"));
    QCOMPARE(cues[0].startUs, TonDron::secondsToUs(0.5));
    QCOMPARE(cues[0].endUs, TonDron::secondsToUs(2.0));

    QVERIFY(!TonDron::parseSrt(QString(), &cues, &error));
    QVERIFY(!error.isEmpty());
}

void CoreTest::insertTrackAtTopAllowsDuplicateTypes()
{
    TonDron::Project project;
    QCOMPARE(project.tracks().size(), 1);
    QCOMPARE(project.tracks()[0].type, TonDron::TrackType::Video);

    const int first = TonDron::insertTrackAtTopForClipType(project, TonDron::ClipType::Video);
    QCOMPARE(first, 0);
    QCOMPARE(project.tracks().size(), 2);
    QCOMPARE(project.tracks()[0].type, TonDron::TrackType::Video);
    QCOMPARE(project.tracks()[1].type, TonDron::TrackType::Video);

    const int second = TonDron::insertTrackAtTopForClipType(project, TonDron::ClipType::Audio);
    QCOMPARE(second, 0);
    QCOMPARE(project.tracks().size(), 3);
    QCOMPARE(project.tracks()[0].type, TonDron::TrackType::Audio);
}

void CoreTest::multiTrackSerializationRoundTrip()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video, .muted = true});
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Audio, .showWaveform = true});
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video, .hidden = true});
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Text});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-v2");
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = TonDron::secondsToUs(1.0);
    clip.timelineDuration = TonDron::secondsToUs(2.0);
    project.tracks()[2].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.tracks().size(), 4);
    QCOMPARE(loaded.tracks()[0].type, TonDron::TrackType::Video);
    QVERIFY(loaded.tracks()[0].muted);
    QCOMPARE(loaded.tracks()[1].type, TonDron::TrackType::Audio);
    QVERIFY(loaded.tracks()[1].showWaveform);
    QCOMPARE(loaded.tracks()[2].type, TonDron::TrackType::Video);
    QVERIFY(loaded.tracks()[2].hidden);
    QCOMPARE(loaded.tracks()[2].clips.size(), 1);
    QCOMPARE(loaded.tracks()[2].clips[0].id, QStringLiteral("clip-v2"));
    QCOMPARE(loaded.tracks()[3].type, TonDron::TrackType::Text);
}

void CoreTest::textStyleAndBlendModeSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Text});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-textstyle");
    clip.type = TonDron::ClipType::Text;
    clip.name = QStringLiteral("Title");
    clip.textContent = QStringLiteral("Hello");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(3.0);
    clip.blendMode = TonDron::BlendMode::Multiply;
    clip.textStyle.fontFamily = QStringLiteral("Courier New");
    clip.textStyle.pixelSize = 88;
    clip.textStyle.fontWeight = 300;
    clip.textStyle.italic = true;
    clip.textStyle.color = QColor(10, 20, 30, 200);
    clip.textStyle.align = TonDron::TextAlign::Right;
    clip.textStyle.valign = TonDron::TextVAlign::Bottom;
    clip.textStyle.wordWrap = false;
    clip.textStyle.lineHeight = 1.6;
    clip.textStyle.letterSpacing = 3.5;
    clip.textStyle.outlineEnabled = true;
    clip.textStyle.outlineWidth = 2.5;
    clip.textStyle.outlineColor = QColor(255, 0, 0);
    clip.textStyle.shadowEnabled = true;
    clip.textStyle.shadowOffsetX = -3.0;
    clip.textStyle.shadowOffsetY = 7.0;
    clip.textStyle.shadowBlur = 11.0;
    clip.textStyle.shadowOpacity = 0.42;
    clip.textStyle.shadowColor = QColor(0, 128, 255);
    clip.textStyle.glowEnabled = true;
    clip.textStyle.glowColor = QColor(0, 255, 128);
    clip.textStyle.glowRadius = 21.0;
    clip.textStyle.glowOpacity = 0.55;
    clip.textStyle.boxEnabled = true;
    clip.textStyle.boxColor = QColor(0, 0, 0, 100);
    clip.textStyle.boxPadding = 12.0;
    clip.textStyle.boxRadius = 5.0;
    clip.textStyle.packId = QStringLiteral("hormozi");
    clip.textStyle.wordHighlight = {true, QColor(12, 34, 56), 9.0, 3.0};
    clip.textStyle.underlineEnabled = true;
    clip.textStyle.underlineColor = QColor(200, 100, 50);
    clip.textStyle.underlineWidth = 7.5;
    clip.textStyle.underlineOffset = 2.5;
    clip.textStyle.accent.rule = TonDron::WordAccentRule::EveryNth;
    clip.textStyle.accent.n = 3;
    clip.textStyle.accent.phase = 1;
    clip.textStyle.accent.colorEnabled = true;
    clip.textStyle.accent.color = QColor(9, 8, 7);
    clip.textStyle.accent.sizeScale = 1.4;
    clip.textStyle.accent.outlineEnabled = true;
    clip.textStyle.accent.outlineWidth = 4.5;
    clip.textStyle.accent.outlineColor = QColor(1, 2, 3);
    clip.textStyle.accent.highlight = {true, QColor(60, 70, 80), 11.0, 6.0};
    clip.textStyle.animIn = {TonDron::TextAnimKind::Pop, TonDron::secondsToUs(0.3), TonDron::TextEase::Back};
    clip.textStyle.animOut = {TonDron::TextAnimKind::SlideDown, TonDron::secondsToUs(0.25),
                              TonDron::TextEase::EaseInOut};
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    const TonDron::TextStyle &s = loadedClip.textStyle;
    QCOMPARE(loadedClip.blendMode, TonDron::BlendMode::Multiply);
    QCOMPARE(s.fontFamily, QStringLiteral("Courier New"));
    QCOMPARE(s.pixelSize, 88);
    QCOMPARE(s.fontWeight, 300);
    QCOMPARE(s.italic, true);
    QCOMPARE(s.color, QColor(10, 20, 30, 200));
    QCOMPARE(s.align, TonDron::TextAlign::Right);
    QCOMPARE(s.valign, TonDron::TextVAlign::Bottom);
    QCOMPARE(s.wordWrap, false);
    QCOMPARE(s.lineHeight, 1.6);
    QCOMPARE(s.letterSpacing, 3.5);
    QCOMPARE(s.outlineEnabled, true);
    QCOMPARE(s.outlineWidth, 2.5);
    QCOMPARE(s.outlineColor, QColor(255, 0, 0));
    QCOMPARE(s.shadowEnabled, true);
    QCOMPARE(s.shadowOffsetX, -3.0);
    QCOMPARE(s.shadowOffsetY, 7.0);
    QCOMPARE(s.shadowBlur, 11.0);
    QCOMPARE(s.shadowOpacity, 0.42);
    QCOMPARE(s.shadowColor, QColor(0, 128, 255));
    QCOMPARE(s.glowEnabled, true);
    QCOMPARE(s.glowColor, QColor(0, 255, 128));
    QCOMPARE(s.glowRadius, 21.0);
    QCOMPARE(s.glowOpacity, 0.55);
    QCOMPARE(s.boxEnabled, true);
    QCOMPARE(s.boxColor, QColor(0, 0, 0, 100));
    QCOMPARE(s.boxPadding, 12.0);
    QCOMPARE(s.boxRadius, 5.0);
    QCOMPARE(s.packId, QStringLiteral("hormozi"));
    QCOMPARE(s.wordHighlight.enabled, true);
    QCOMPARE(s.wordHighlight.color, QColor(12, 34, 56));
    QCOMPARE(s.wordHighlight.padding, 9.0);
    QCOMPARE(s.wordHighlight.radius, 3.0);
    QCOMPARE(s.underlineEnabled, true);
    QCOMPARE(s.underlineColor, QColor(200, 100, 50));
    QCOMPARE(s.underlineWidth, 7.5);
    QCOMPARE(s.underlineOffset, 2.5);
    QCOMPARE(s.accent.rule, TonDron::WordAccentRule::EveryNth);
    QCOMPARE(s.accent.n, 3);
    QCOMPARE(s.accent.phase, 1);
    QCOMPARE(s.accent.colorEnabled, true);
    QCOMPARE(s.accent.color, QColor(9, 8, 7));
    QCOMPARE(s.accent.sizeScale, 1.4);
    QCOMPARE(s.accent.outlineEnabled, true);
    QCOMPARE(s.accent.outlineWidth, 4.5);
    QCOMPARE(s.accent.outlineColor, QColor(1, 2, 3));
    QCOMPARE(s.accent.highlight.enabled, true);
    QCOMPARE(s.accent.highlight.color, QColor(60, 70, 80));
    QCOMPARE(s.accent.highlight.padding, 11.0);
    QCOMPARE(s.accent.highlight.radius, 6.0);
    QCOMPARE(s.animIn.kind, TonDron::TextAnimKind::Pop);
    QCOMPARE(s.animIn.durationUs, TonDron::secondsToUs(0.3));
    QCOMPARE(s.animIn.ease, TonDron::TextEase::Back);
    QCOMPARE(s.animOut.kind, TonDron::TextAnimKind::SlideDown);
    QCOMPARE(s.animOut.durationUs, TonDron::secondsToUs(0.25));
    QCOMPARE(s.animOut.ease, TonDron::TextEase::EaseInOut);
}

void CoreTest::legacyBoldMigratesToFontWeight()
{
    // Projects written before the weight ladder carried a bold flag instead.
    const auto weightForLegacy = [](const QJsonObject &textStyle) {
        QJsonObject clip{
            {QStringLiteral("id"), QStringLiteral("c1")},
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("textContent"), QStringLiteral("Hi")},
            {QStringLiteral("timelineStart"), 0},
            {QStringLiteral("timelineDuration"), 1000000},
            {QStringLiteral("textStyle"), textStyle},
        };
        QJsonObject track{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("clips"), QJsonArray{clip}},
        };
        QJsonObject project{
            {QStringLiteral("version"), 2},
            {QStringLiteral("width"), 1920},
            {QStringLiteral("height"), 1080},
            {QStringLiteral("fps"), 30},
            {QStringLiteral("tracks"), QJsonArray{track}},
        };
        QString error;
        const TonDron::Project loaded = TonDron::Project::fromJson(project, &error);
        return loaded.tracks().at(0).clips.at(0).textStyle.fontWeight;
    };

    QCOMPARE(weightForLegacy({{QStringLiteral("bold"), true}}), 700);
    QCOMPARE(weightForLegacy({{QStringLiteral("bold"), false}}), 400);
    // A style object with neither key keeps the struct default.
    QCOMPARE(weightForLegacy({{QStringLiteral("pixelSize"), 40}}), 700);
    // A new-format style wins over any stale bold flag.
    QCOMPARE(weightForLegacy({{QStringLiteral("bold"), false}, {QStringLiteral("fontWeight"), 900}}), 900);

    // Pre-outlineEnabled projects treated any positive width as on.
    {
        QJsonObject clip{
            {QStringLiteral("id"), QStringLiteral("c1")},
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("textContent"), QStringLiteral("Hi")},
            {QStringLiteral("timelineStart"), 0},
            {QStringLiteral("timelineDuration"), 1000000},
            {QStringLiteral("textStyle"), QJsonObject{{QStringLiteral("outlineWidth"), 3.0}}},
        };
        QJsonObject track{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("clips"), QJsonArray{clip}},
        };
        QJsonObject project{
            {QStringLiteral("version"), 2},
            {QStringLiteral("width"), 1920},
            {QStringLiteral("height"), 1080},
            {QStringLiteral("fps"), 30},
            {QStringLiteral("tracks"), QJsonArray{track}},
        };
        QString err;
        const TonDron::Project loaded = TonDron::Project::fromJson(project, &err);
        QVERIFY(err.isEmpty());
        QCOMPARE(loaded.tracks().at(0).clips.at(0).textStyle.outlineEnabled, true);
        QCOMPARE(loaded.tracks().at(0).clips.at(0).textStyle.outlineWidth, 3.0);
    }
}

void CoreTest::textPresetsAreWellFormed()
{
    const QList<TonDron::TextPreset> &presets = TonDron::textPresets();
    QVERIFY(!presets.isEmpty());

    QSet<QString> ids;
    for (const TonDron::TextPreset &preset : presets) {
        QVERIFY(!preset.id.isEmpty());
        QVERIFY(!preset.label.isEmpty());
        QVERIFY(!preset.sampleText.isEmpty());
        QVERIFY(!ids.contains(preset.id));
        ids.insert(preset.id);
        QVERIFY(preset.style.pixelSize > 0);
        QVERIFY(!preset.style.fontFamily.isEmpty());
        QVERIFY(preset.style.fontWeight >= 100 && preset.style.fontWeight <= 900);
        QCOMPARE(TonDron::textStyleForPresetId(preset.id)->fontFamily, preset.style.fontFamily);
        QCOMPARE(TonDron::textPresetForId(preset.id)->label, preset.label);

        // A pack's accent has to be usable: a stride that advances, a size that renders, and an
        // override that actually changes something when a rule picks words out.
        const TonDron::WordAccent &accent = preset.style.accent;
        QVERIFY(accent.n >= 1);
        QVERIFY(accent.phase >= 0);
        QVERIFY(accent.sizeScale > 0.0);
        if (accent.rule != TonDron::WordAccentRule::None) {
            QVERIFY(accent.colorEnabled || accent.outlineEnabled || accent.highlight.enabled
                    || !qFuzzyCompare(accent.sizeScale, 1.0));
        }
        if (accent.colorEnabled)
            QVERIFY(accent.color.isValid());
        if (accent.highlight.enabled)
            QVERIFY(accent.highlight.color.isValid());
    }
    QVERIFY(TonDron::textStyleForPresetId(QStringLiteral("nope")) == nullptr);
}

void CoreTest::karaokeWordIndexTracksTheCue()
{
    const QString text = QStringLiteral("Number of thumbnails that");
    const TonDron::TimeUs start = TonDron::secondsToUs(2.0);
    const TonDron::TimeUs end = TonDron::secondsToUs(4.0);

    // Before the window there is no spoken word at all.
    QCOMPARE(TonDron::activeWordIndexAt(text, start, end, TonDron::secondsToUs(1.0)), -1);
    QCOMPARE(TonDron::activeWordIndexAt(text, start, start, TonDron::secondsToUs(2.5)), -1);

    // Inside it the index only ever advances, starts at the first word and ends on the last.
    QCOMPARE(TonDron::activeWordIndexAt(text, start, end, start), 0);
    int previous = 0;
    for (int step = 1; step <= 20; ++step) {
        const TonDron::TimeUs at = start + (end - start) * step / 21;
        const int index = TonDron::activeWordIndexAt(text, start, end, at);
        QVERIFY(index >= previous);
        QVERIFY(index < 4);
        previous = index;
    }
    QCOMPARE(TonDron::activeWordIndexAt(text, start, end, end - 1), 3);
    // Past the end (rounding at a cue boundary) keeps the last word lit rather than blanking it.
    QCOMPARE(TonDron::activeWordIndexAt(text, start, end, end), 3);
}

void CoreTest::shapeStyleSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Shape});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-shape");
    clip.type = TonDron::ClipType::Shape;
    clip.name = QStringLiteral("Hexagon");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::kImageClipDurationUs;
    clip.shapeStyle.kind = TonDron::ShapeKind::Hexagon;
    clip.shapeStyle.fillKind = TonDron::ShapeFillKind::LinearGradient;
    clip.shapeStyle.fill = QColor(10, 20, 30, 200);
    clip.shapeStyle.fillSecondary = QColor(40, 50, 60, 128);
    clip.shapeStyle.gradientAngle = 35.0;
    clip.shapeStyle.stroke = QColor(255, 255, 255);
    clip.shapeStyle.strokeWidth = 6.0;
    clip.shapeStyle.strokeStyle = TonDron::ShapeStrokeStyle::DashDot;
    clip.shapeStyle.cornerRadius = 18.0;
    clip.shapeStyle.points = 9;
    clip.shapeStyle.innerRatio = 0.33;
    clip.shapeStyle.headSize = 0.55;
    clip.shapeStyle.thickness = 0.22;
    clip.shapeStyle.tailX = 0.7;
    clip.shapeStyle.tailSize = 0.15;
    clip.transformX.setKeyframe(0, 100.0);
    clip.transformY.setKeyframe(0, 200.0);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.type, TonDron::ClipType::Shape);
    QCOMPARE(loadedClip.shapeStyle.kind, TonDron::ShapeKind::Hexagon);
    QCOMPARE(loadedClip.shapeStyle.fillKind, TonDron::ShapeFillKind::LinearGradient);
    QCOMPARE(loadedClip.shapeStyle.fill, QColor(10, 20, 30, 200));
    QCOMPARE(loadedClip.shapeStyle.fillSecondary, QColor(40, 50, 60, 128));
    QCOMPARE(loadedClip.shapeStyle.gradientAngle, 35.0);
    QCOMPARE(loadedClip.shapeStyle.stroke, QColor(255, 255, 255));
    QCOMPARE(loadedClip.shapeStyle.strokeWidth, 6.0);
    QCOMPARE(loadedClip.shapeStyle.strokeStyle, TonDron::ShapeStrokeStyle::DashDot);
    QCOMPARE(loadedClip.shapeStyle.cornerRadius, 18.0);
    QCOMPARE(loadedClip.shapeStyle.points, 9);
    QCOMPARE(loadedClip.shapeStyle.innerRatio, 0.33);
    QCOMPARE(loadedClip.shapeStyle.headSize, 0.55);
    QCOMPARE(loadedClip.shapeStyle.thickness, 0.22);
    QCOMPARE(loadedClip.shapeStyle.tailX, 0.7);
    QCOMPARE(loadedClip.shapeStyle.tailSize, 0.15);
    QCOMPARE(loadedClip.transformX.evaluateAt(0), 100.0);
    QCOMPARE(loadedClip.transformY.evaluateAt(0), 200.0);
}

// A project saved before shapes gained gradients, dash styles and geometry knobs carries only the
// original four keys, and must still load with the new fields at their defaults.
void CoreTest::legacyShapeStyleLoadsWithDefaults()
{
    const QJsonObject json{
        {QStringLiteral("version"), TonDron::Project::kCurrentVersion},
        {QStringLiteral("tracks"),
         QJsonArray{QJsonObject{
             {QStringLiteral("type"), QStringLiteral("shape")},
             {QStringLiteral("clips"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("id"), QStringLiteral("legacy-shape")},
                  {QStringLiteral("type"), QStringLiteral("shape")},
                  {QStringLiteral("timelineDurationUs"), qint64(TonDron::kImageClipDurationUs)},
                  {QStringLiteral("shapeStyle"),
                   QJsonObject{{QStringLiteral("kind"), QStringLiteral("pentagon")},
                               {QStringLiteral("fill"), QStringLiteral("#ffa060ff")},
                               {QStringLiteral("stroke"), QStringLiteral("#ffffffff")},
                               {QStringLiteral("strokeWidth"), 4.0}}}}}}}}}};

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);
    QVERIFY(error.isEmpty());

    const TonDron::ShapeStyle &style = loaded.tracks()[0].clips[0].shapeStyle;
    const TonDron::ShapeStyle defaults;
    QCOMPARE(style.kind, TonDron::ShapeKind::Pentagon);
    QCOMPARE(style.fill, QColor(160, 96, 255));
    QCOMPARE(style.fillKind, defaults.fillKind);
    QCOMPARE(style.fillSecondary, defaults.fillSecondary);
    QCOMPARE(style.gradientAngle, defaults.gradientAngle);
    QCOMPARE(style.strokeStyle, defaults.strokeStyle);
    QCOMPARE(style.cornerRadius, defaults.cornerRadius);
    QCOMPARE(style.points, defaults.points);
    QCOMPARE(style.innerRatio, defaults.innerRatio);
}

// Guards ~28 hand-written path formulas: a typo shows up as an empty path or one that escapes the
// layout rect and gets clipped out of the layer.
void CoreTest::shapeCatalogPathsFitBounds()
{
    const QRectF bounds(0, 0, 200, 120);
    QVERIFY(!TonDron::shapeCatalog().isEmpty());

    for (const TonDron::ShapeCatalogEntry &entry : TonDron::shapeCatalog()) {
        const QPainterPath path = TonDron::shapePath(entry.style, bounds);
        QVERIFY2(!path.isEmpty(), qPrintable(entry.id));
        QVERIFY2(entry.aspect > 0.0, qPrintable(entry.id));

        // Cubic control points can bow a hair outside the hull, so allow a small tolerance.
        const QRectF box = path.boundingRect();
        QVERIFY2(bounds.adjusted(-1, -1, 1, 1).contains(box), qPrintable(entry.id));
        QVERIFY2(box.width() > bounds.width() * 0.3, qPrintable(entry.id));
        QVERIFY2(box.height() > bounds.height() * 0.3, qPrintable(entry.id));

        QVERIFY2(!TonDron::shapeSvgPath(entry.style, bounds).isEmpty(), qPrintable(entry.id));
        // Ids are what QML and the drag mime data carry, so every one must resolve.
        QVERIFY2(TonDron::shapeCatalogEntry(entry.id) != nullptr, qPrintable(entry.id));
    }
}

void CoreTest::effectCatalogIdSerialization()
{
    TonDron::Project project;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-effects");
    clip.type = TonDron::ClipType::Video;
    clip.name = QStringLiteral("Video");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(2.0);

    TonDron::Effect effect;
    effect.name = QStringLiteral("eq");
    effect.catalogId = QStringLiteral("adjust.contrast");
    effect.parameters.insert(QStringLiteral("contrast"), 1.4);
    clip.effects.append(effect);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.effects.size(), 1);
    QCOMPARE(loadedClip.effects[0].catalogId, QStringLiteral("adjust.contrast"));
    QCOMPARE(loadedClip.effects[0].parameters.value(QStringLiteral("contrast")).toDouble(), 1.4);
    // Projects written before animated params existed carry no paramKeyframes at all.
    QVERIFY(loadedClip.effects[0].paramKeyframes.isEmpty());
}

// An animated effect parameter is only worth anything if it survives a save/load, and the static
// value has to come back alongside it — that is what an un-keyed frame falls back to.
void CoreTest::effectParamKeyframeSerialization()
{
    TonDron::Project project;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-animated-effect");
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(4.0);

    TonDron::Effect effect;
    effect.catalogId = QStringLiteral("adjust.contrast");
    effect.parameters.insert(QStringLiteral("contrast"), 1.4);
    TonDron::KeyframeTrack<double> track;
    track.setKeyframe(0, 0.5);
    track.setKeyframe(TonDron::secondsToUs(2.0), 2.5);
    track.setEasing(0, TonDron::Interpolation::Ease);
    track.setEasing(TonDron::secondsToUs(2.0), TonDron::Interpolation::Ease);
    effect.paramKeyframes.insert(QStringLiteral("contrast"), track);
    clip.effects.append(effect);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Effect &loadedEffect = loaded.tracks()[0].clips[0].effects[0];
    QCOMPARE(loadedEffect.parameters.value(QStringLiteral("contrast")).toDouble(), 1.4);
    const TonDron::KeyframeTrack<double> &loadedTrack =
        loadedEffect.paramKeyframes.value(QStringLiteral("contrast"));
    QCOMPARE(loadedTrack.keyframes().size(), 2);
    QVERIFY(loadedTrack.easingAt(0) == TonDron::Interpolation::Ease);
    QVERIFY(loadedTrack.easingAt(TonDron::secondsToUs(2.0)) == TonDron::Interpolation::Ease);

    // valueAt is what the compositor reads: the track wins where it has keys, and an unkeyed
    // param falls back to the static value.
    QCOMPARE(loadedEffect.valueAt(QStringLiteral("contrast"), 0).toDouble(), 0.5);
    QCOMPARE(loadedEffect.valueAt(QStringLiteral("contrast"), TonDron::secondsToUs(2.0)).toDouble(), 2.5);
    const double mid = loadedEffect.valueAt(QStringLiteral("contrast"), TonDron::secondsToUs(1.0)).toDouble();
    QVERIFY(mid > 0.5 && mid < 2.5);
    QCOMPARE(loadedEffect.valueAt(QStringLiteral("nosuch"), 0).isValid(), false);

    // resolvedAt bakes the animated params down; everything else is carried through untouched.
    const TonDron::Effect resolved = loadedEffect.resolvedAt(TonDron::secondsToUs(2.0));
    QCOMPARE(resolved.parameters.value(QStringLiteral("contrast")).toDouble(), 2.5);
    QCOMPARE(resolved.catalogId, QStringLiteral("adjust.contrast"));
}

// Plain Project copy shares QMap payloads with the source. Mutating keyframes on the live
// project must not touch a compositor snapshot (and must not race a concurrent reader).
void CoreTest::detachedCopyIsolatesKeyframesFromLiveMutations()
{
    TonDron::Project live;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-detach");
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(2.0);
    clip.opacity.setKeyframe(0, 1.0);

    TonDron::Effect effect;
    effect.catalogId = QStringLiteral("adjust.contrast");
    effect.parameters.insert(QStringLiteral("contrast"), 1.0);
    TonDron::KeyframeTrack<double> track;
    track.setKeyframe(0, 0.5);
    effect.paramKeyframes.insert(QStringLiteral("contrast"), track);
    clip.effects.append(effect);
    live.tracks()[0].clips.append(clip);

    const TonDron::Project snapshot = live.detachedCopy();
    QCOMPARE(snapshot.tracks().at(0).clips.at(0).opacity.evaluateAt(0), 1.0);
    QCOMPARE(snapshot.tracks().at(0).clips.at(0).effects.at(0).valueAt(QStringLiteral("contrast"), 0).toDouble(),
             0.5);

    // Mutate every COW container the compositor reads — list structure and nested maps.
    live.tracks()[0].clips[0].opacity.setKeyframe(0, 0.25);
    live.tracks()[0].clips[0].opacity.setKeyframe(TonDron::secondsToUs(1.0), 0.0);
    live.tracks()[0].clips[0].effects[0].paramKeyframes[QStringLiteral("contrast")].setKeyframe(
        0, 2.0);
    live.tracks()[0].clips[0].effects[0].parameters.insert(QStringLiteral("contrast"), 2.0);
    TonDron::Clip extra;
    extra.id = QStringLiteral("clip-extra");
    extra.type = TonDron::ClipType::Video;
    live.tracks()[0].clips.append(extra);

    QCOMPARE(snapshot.tracks().size(), 1);
    QCOMPARE(snapshot.tracks().at(0).clips.size(), 1);
    QCOMPARE(snapshot.tracks().at(0).clips.at(0).opacity.evaluateAt(0), 1.0);
    QCOMPARE(snapshot.tracks().at(0).clips.at(0).opacity.keyframes().size(), 1);
    QCOMPARE(snapshot.tracks().at(0).clips.at(0).effects.at(0).valueAt(QStringLiteral("contrast"), 0).toDouble(),
             0.5);
    QCOMPARE(snapshot.tracks().at(0).clips.at(0).effects.at(0).parameters.value(QStringLiteral("contrast")).toDouble(),
             1.0);

    const TonDron::Effect resolved =
        snapshot.tracks().at(0).clips.at(0).effects.at(0).resolvedAt(0);
    QCOMPARE(resolved.parameters.value(QStringLiteral("contrast")).toDouble(), 0.5);
}

// A beat-synced template applies several effects and per-param keyframes in one edit; all of
// that has to survive save/load so scrubbing after reopen matches what preview showed.
void CoreTest::effectTemplateStackSerialization()
{
    TonDron::Project project;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-template");
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(4.0);

    TonDron::Effect shake;
    shake.catalogId = QStringLiteral("beat_shake");
    shake.parameters.insert(QStringLiteral("amount"), 0.0);
    TonDron::KeyframeTrack<double> amountTrack;
    amountTrack.setKeyframe(0, 0.7);
    amountTrack.setKeyframe(TonDron::secondsToUs(0.18), 0.0);
    amountTrack.setKeyframe(TonDron::secondsToUs(1.0), 0.65);
    amountTrack.setKeyframe(TonDron::secondsToUs(1.09), 0.0);
    shake.paramKeyframes.insert(QStringLiteral("amount"), amountTrack);
    clip.effects.append(shake);

    TonDron::Effect strobe;
    strobe.catalogId = QStringLiteral("strobe_flash");
    strobe.parameters.insert(QStringLiteral("flash"), 0.0);
    TonDron::KeyframeTrack<double> flashTrack;
    flashTrack.setKeyframe(0, 0.5);
    flashTrack.setKeyframe(TonDron::secondsToUs(0.09), 0.0);
    strobe.paramKeyframes.insert(QStringLiteral("flash"), flashTrack);
    clip.effects.append(strobe);

    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.effects.size(), 2);
    QCOMPARE(loadedClip.effects[0].catalogId, QStringLiteral("beat_shake"));
    QCOMPARE(loadedClip.effects[1].catalogId, QStringLiteral("strobe_flash"));

    const TonDron::KeyframeTrack<double> &loadedAmount =
        loadedClip.effects[0].paramKeyframes.value(QStringLiteral("amount"));
    QCOMPARE(loadedAmount.keyframes().size(), 4);
    QCOMPARE(loadedAmount.keyframes().value(0).value, 0.7);
    QCOMPARE(loadedAmount.keyframes().value(TonDron::secondsToUs(1.0)).value, 0.65);

    const TonDron::KeyframeTrack<double> &loadedFlash =
        loadedClip.effects[1].paramKeyframes.value(QStringLiteral("flash"));
    QCOMPARE(loadedFlash.keyframes().size(), 2);
    QCOMPARE(loadedFlash.keyframes().value(0).value, 0.5);
    QCOMPARE(loadedClip.effects[1].valueAt(QStringLiteral("flash"), 0).toDouble(), 0.5);
}

// Audio effects live in a separate list from video effects on the clip and must survive a project
// round-trip independently — a regression here silently drops a clip's sound design on reload.
void CoreTest::audioEffectSerialization()
{
    TonDron::Project project;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-audio-fx");
    clip.type = TonDron::ClipType::Audio;
    clip.name = QStringLiteral("Audio");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(2.0);

    TonDron::Effect telephone;
    telephone.name = QStringLiteral("Telephone");
    telephone.catalogId = QStringLiteral("transmission.telephone");
    clip.audioEffects.append(telephone);

    TonDron::Effect bitcrush;
    bitcrush.name = QStringLiteral("Bitcrush");
    bitcrush.catalogId = QStringLiteral("texture.bitcrush");
    bitcrush.parameters.insert(QStringLiteral("bits"), 6.0);
    bitcrush.parameters.insert(QStringLiteral("mix"), 0.7);
    clip.audioEffects.append(bitcrush);

    // A video effect on the same clip must not bleed into the audio list and vice versa.
    TonDron::Effect contrast;
    contrast.catalogId = QStringLiteral("adjust.contrast");
    clip.effects.append(contrast);

    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.audioEffects.size(), 2);
    QCOMPARE(loadedClip.audioEffects[0].catalogId, QStringLiteral("transmission.telephone"));
    QCOMPARE(loadedClip.audioEffects[1].catalogId, QStringLiteral("texture.bitcrush"));
    QCOMPARE(loadedClip.audioEffects[1].parameters.value(QStringLiteral("bits")).toDouble(), 6.0);
    QCOMPARE(loadedClip.audioEffects[1].parameters.value(QStringLiteral("mix")).toDouble(), 0.7);
    QCOMPARE(loadedClip.effects.size(), 1);
    QCOMPARE(loadedClip.effects[0].catalogId, QStringLiteral("adjust.contrast"));
}

void CoreTest::rgbSplitEffectParametersSerialization()
{
    TonDron::Project project;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-rgb-split");
    clip.type = TonDron::ClipType::Video;
    clip.name = QStringLiteral("Video");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(2.0);

    TonDron::Effect effect;
    effect.name = QStringLiteral("rgb_split");
    effect.catalogId = QStringLiteral("rgb_split");
    effect.parameters.insert(QStringLiteral("amount"), 12.0);
    effect.parameters.insert(QStringLiteral("angle"), 45.0);
    effect.parameters.insert(QStringLiteral("animated"), true);
    effect.parameters.insert(QStringLiteral("speed"), 2.5);
    clip.effects.append(effect);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.effects.size(), 1);
    QCOMPARE(loadedClip.effects[0].catalogId, QStringLiteral("rgb_split"));
    const QMap<QString, QVariant> &params = loadedClip.effects[0].parameters;
    QCOMPARE(params.value(QStringLiteral("amount")).toDouble(), 12.0);
    QCOMPARE(params.value(QStringLiteral("angle")).toDouble(), 45.0);
    QCOMPARE(params.value(QStringLiteral("animated")).toBool(), true);
    QCOMPARE(params.value(QStringLiteral("speed")).toDouble(), 2.5);
}

void CoreTest::blockGlitchEffectParametersSerialization()
{
    TonDron::Project project;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-block-glitch");
    clip.type = TonDron::ClipType::Video;
    clip.name = QStringLiteral("Video");
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(2.0);

    TonDron::Effect effect;
    effect.name = QStringLiteral("block_glitch");
    effect.catalogId = QStringLiteral("block_glitch");
    effect.parameters.insert(QStringLiteral("intensity"), 0.5);
    effect.parameters.insert(QStringLiteral("blockSize"), 48.0);
    effect.parameters.insert(QStringLiteral("shiftAmount"), 36.0);
    effect.parameters.insert(QStringLiteral("frequency"), 0.4);
    effect.parameters.insert(QStringLiteral("seed"), 7.0);
    clip.effects.append(effect);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &loadedClip = loaded.tracks()[0].clips[0];
    QCOMPARE(loadedClip.effects.size(), 1);
    QCOMPARE(loadedClip.effects[0].catalogId, QStringLiteral("block_glitch"));
    const QMap<QString, QVariant> &params = loadedClip.effects[0].parameters;
    QCOMPARE(params.value(QStringLiteral("intensity")).toDouble(), 0.5);
    QCOMPARE(params.value(QStringLiteral("blockSize")).toDouble(), 48.0);
    QCOMPARE(params.value(QStringLiteral("shiftAmount")).toDouble(), 36.0);
    QCOMPARE(params.value(QStringLiteral("frequency")).toDouble(), 0.4);
    QCOMPARE(params.value(QStringLiteral("seed")).toDouble(), 7.0);
}

void CoreTest::clipSpeedSourceMapping()
{
    TonDron::Clip clip;
    clip.timelineStart = TonDron::secondsToUs(1.0);
    clip.timelineDuration = TonDron::secondsToUs(4.0);
    clip.srcIn = TonDron::secondsToUs(2.0);
    clip.speed = 2.0;
    clip.srcOut = clip.srcIn + clip.sourceSpanUs();

    QCOMPARE(clip.sourceSpanUs(), TonDron::secondsToUs(8.0));
    QCOMPARE(clip.timelineToSourceUs(TonDron::secondsToUs(3.0)), TonDron::secondsToUs(6.0));

    clip.syncSrcOutFromSpeed(TonDron::secondsToUs(20.0));
    QCOMPARE(clip.srcOut, clip.srcIn + TonDron::secondsToUs(8.0));

    clip.reverse = true;
    // At timeline start → near srcOut; at +1s timeline with speed 2 → srcOut - 2s
    QCOMPARE(clip.timelineToSourceUs(TonDron::secondsToUs(1.0)), clip.srcOut);
    QCOMPARE(clip.timelineToSourceUs(TonDron::secondsToUs(2.0)), clip.srcOut - TonDron::secondsToUs(2.0));
}

namespace {

// A ramp with no handles is linear in speed across pos, which keeps the expected values above
// closed-form rather than something only the implementation can produce.
TonDron::SpeedCurve linearRamp(double from, double to)
{
    TonDron::SpeedPoint start;
    start.pos = 0.0;
    start.speed = from;
    start.corner = true;
    TonDron::SpeedPoint end;
    end.pos = 1.0;
    end.speed = to;
    end.corner = true;

    TonDron::SpeedCurve curve;
    curve.setPoints({start, end});
    return curve;
}

TonDron::Clip curvedClip(const TonDron::SpeedCurve &curve, double srcInSec, double srcOutSec)
{
    TonDron::Clip clip;
    clip.timelineStart = TonDron::secondsToUs(1.0);
    clip.srcIn = TonDron::secondsToUs(srcInSec);
    clip.srcOut = TonDron::secondsToUs(srcOutSec);
    clip.speedCurve = curve;
    clip.syncDurationFromSpeedCurve();
    return clip;
}

} // namespace

void CoreTest::speedCurveMatchesConstantSpeed()
{
    TonDron::Clip scalar;
    scalar.timelineStart = TonDron::secondsToUs(1.0);
    scalar.srcIn = TonDron::secondsToUs(2.0);
    scalar.srcOut = scalar.srcIn + TonDron::secondsToUs(8.0);
    scalar.speed = 2.0;
    scalar.timelineDuration = TonDron::secondsToUs(4.0);

    const TonDron::Clip curved = curvedClip(TonDron::SpeedCurve::flat(2.0), 2.0, 10.0);

    QCOMPARE(curved.timelineDuration, scalar.timelineDuration);
    for (int i = 0; i <= 20; ++i) {
        const TonDron::TimeUs at = scalar.timelineStart + (scalar.timelineDuration * i) / 20;
        QVERIFY(qAbs(curved.timelineToSourceUs(at) - scalar.timelineToSourceUs(at)) <= 2);
    }
}

void CoreTest::speedCurveRampRetimesDuration()
{
    // 1× ramping to 4× over a 10s source: ∫dp/(1+3p) = ln(4)/3.
    const TonDron::Clip clip = curvedClip(linearRamp(1.0, 4.0), 0.0, 10.0);

    const double expectedSeconds = 10.0 * std::log(4.0) / 3.0;
    QVERIFY(qAbs(TonDron::usToSeconds(clip.timelineDuration) - expectedSeconds) < 0.001);

    // And the inverse: p = (exp(3t/span) - 1) / 3.
    for (int i = 1; i < 10; ++i) {
        const double t = expectedSeconds * i / 10.0;
        const double expectedPos = (std::exp(3.0 * t / 10.0) - 1.0) / 3.0;
        const TonDron::TimeUs at = clip.timelineStart + TonDron::secondsToUs(t);
        const double actual = TonDron::usToSeconds(clip.timelineToSourceUs(at) - clip.srcIn) / 10.0;
        QVERIFY(qAbs(actual - expectedPos) < 0.002);
    }
}

void CoreTest::speedCurveMappingIsMonotonic()
{
    TonDron::SpeedPoint a;
    a.pos = 0.0;
    a.speed = 4.0;
    TonDron::SpeedPoint b;
    b.pos = 0.4;
    b.speed = 0.2;
    TonDron::SpeedPoint c;
    c.pos = 1.0;
    c.speed = 8.0;
    // Give the dip real tangents, so the flattening and not just the corner case is exercised.
    b.inDx = -0.1;
    b.outDx = 0.15;

    TonDron::SpeedCurve curve;
    curve.setPoints({a, b, c});
    const TonDron::Clip clip = curvedClip(curve, 0.0, 12.0);

    QVERIFY(clip.timelineDuration > 0);
    TonDron::TimeUs previous = -1;
    for (int i = 0; i <= 500; ++i) {
        const TonDron::TimeUs at = clip.timelineStart + (clip.timelineDuration * i) / 500;
        const TonDron::TimeUs source = clip.timelineToSourceUs(at);
        QVERIFY(source >= previous);
        QVERIFY(source >= clip.srcIn && source <= clip.srcOut);
        previous = source;
    }
}

void CoreTest::speedCurveSubRangePreservesShape()
{
    const TonDron::SpeedCurve curve = linearRamp(1.0, 4.0);
    const TonDron::SpeedCurve head = curve.subRange(0.0, 0.5);
    const TonDron::SpeedCurve tail = curve.subRange(0.5, 1.0);

    for (int i = 0; i <= 10; ++i) {
        const double f = i / 10.0;
        QVERIFY(qAbs(head.speedAt(f) - curve.speedAt(f * 0.5)) < 0.01);
        QVERIFY(qAbs(tail.speedAt(f) - curve.speedAt(0.5 + f * 0.5)) < 0.01);
    }
}

void CoreTest::speedCurveSerialization()
{
    TonDron::SpeedPoint mid;
    mid.pos = 0.5;
    mid.speed = 0.25;
    mid.inDx = -0.2;
    mid.inDy = 0.1;
    mid.outDx = 0.3;
    mid.outDy = -0.05;
    mid.corner = true;

    TonDron::SpeedPoint start;
    start.pos = 0.0;
    start.speed = 1.0;
    TonDron::SpeedPoint end;
    end.pos = 1.0;
    end.speed = 2.0;

    TonDron::SpeedCurve curve;
    curve.setPoints({start, mid, end});

    TonDron::Project project;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-curve");
    clip.type = TonDron::ClipType::Video;
    clip.srcIn = 0;
    clip.srcOut = TonDron::secondsToUs(6.0);
    clip.speedCurve = curve;
    clip.syncDurationFromSpeedCurve();
    project.tracks()[0].clips.append(clip);

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
    QVERIFY(error.isEmpty());

    const TonDron::Clip &out = loaded.tracks()[0].clips[0];
    QVERIFY(out.hasSpeedCurve());
    QCOMPARE(out.speedCurve.points().size(), 3);
    const TonDron::SpeedPoint &loadedMid = out.speedCurve.points().at(1);
    QCOMPARE(loadedMid.pos, 0.5);
    QCOMPARE(loadedMid.speed, 0.25);
    QCOMPARE(loadedMid.inDx, -0.2);
    QCOMPARE(loadedMid.outDx, 0.3);
    QCOMPARE(loadedMid.corner, true);
    QCOMPARE(out.speedCurve.retimedDurationUs(out.srcOut - out.srcIn), clip.timelineDuration);
}

void CoreTest::clipReverseAndFlipSerialization()
{
    TonDron::Project project;
    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-flip");
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(2.0);
    clip.srcIn = TonDron::secondsToUs(1.0);
    clip.srcOut = TonDron::secondsToUs(3.0);
    clip.reverse = true;
    clip.flipH = true;
    clip.flipV = true;
    clip.speed = 1.5;
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);
    QVERIFY(error.isEmpty());
    const TonDron::Clip &out = loaded.tracks()[0].clips[0];
    QCOMPARE(out.reverse, true);
    QCOMPARE(out.flipH, true);
    QCOMPARE(out.flipV, true);
    QCOMPARE(out.speed, 1.5);
}

void CoreTest::clipSplitMergeRoundTrip()
{
    TonDron::Clip head;
    head.id = QStringLiteral("head");
    head.type = TonDron::ClipType::Video;
    head.assetId = QStringLiteral("asset-a");
    head.path = QStringLiteral("/tmp/a.mp4");
    head.timelineStart = 0;
    head.timelineDuration = TonDron::secondsToUs(4.0);
    head.srcIn = TonDron::secondsToUs(1.0);
    head.srcOut = TonDron::secondsToUs(5.0);
    head.speed = 1.0;

    TonDron::Clip tail;
    QVERIFY(TonDron::splitClipAtOffset(head, tail, TonDron::secondsToUs(2.0)));
    tail.id = QStringLiteral("tail");
    QCOMPARE(head.timelineDuration, TonDron::secondsToUs(2.0));
    QCOMPARE(tail.timelineStart, TonDron::secondsToUs(2.0));
    QCOMPARE(head.srcOut, TonDron::secondsToUs(3.0));
    QCOMPARE(tail.srcIn, TonDron::secondsToUs(3.0));
    QVERIFY(TonDron::clipsCanMerge(head, tail));

    const TonDron::Clip merged = TonDron::mergeClips(head, tail);
    QCOMPARE(merged.timelineDuration, TonDron::secondsToUs(4.0));
    QCOMPARE(merged.srcIn, TonDron::secondsToUs(1.0));
    QCOMPARE(merged.srcOut, TonDron::secondsToUs(5.0));

    // Reverse split: earlier half maps to higher source.
    TonDron::Clip rev;
    rev.id = QStringLiteral("rev");
    rev.type = TonDron::ClipType::Video;
    rev.assetId = QStringLiteral("asset-a");
    rev.path = QStringLiteral("/tmp/a.mp4");
    rev.timelineStart = 0;
    rev.timelineDuration = TonDron::secondsToUs(4.0);
    rev.srcIn = TonDron::secondsToUs(1.0);
    rev.srcOut = TonDron::secondsToUs(5.0);
    rev.reverse = true;
    TonDron::Clip revTail;
    QVERIFY(TonDron::splitClipAtOffset(rev, revTail, TonDron::secondsToUs(2.0)));
    revTail.id = QStringLiteral("rev-tail");
    QCOMPARE(rev.srcIn, TonDron::secondsToUs(3.0));
    QCOMPARE(rev.srcOut, TonDron::secondsToUs(5.0));
    QCOMPARE(revTail.srcIn, TonDron::secondsToUs(1.0));
    QCOMPARE(revTail.srcOut, TonDron::secondsToUs(3.0));
    QVERIFY(TonDron::clipsCanMerge(rev, revTail));
}

void CoreTest::clipLinkFieldsSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-v");
    clip.linkId = QStringLiteral("link-abc");
    clip.suppressEmbeddedAudio = true;
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(2.0);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);
    QVERIFY(error.isEmpty());
    const TonDron::Clip &out = loaded.tracks()[0].clips[0];
    QCOMPARE(out.linkId, QStringLiteral("link-abc"));
    QCOMPARE(out.suppressEmbeddedAudio, true);
}

void CoreTest::maskAndTransitionSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});

    TonDron::Clip clipA;
    clipA.id = QStringLiteral("clip-a");
    clipA.type = TonDron::ClipType::Video;
    clipA.timelineStart = 0;
    clipA.timelineDuration = TonDron::secondsToUs(2.0);
    clipA.speed = 2.0;
    clipA.mask.shape = TonDron::MaskShape::Ellipse;
    clipA.mask.w = 0.5;
    clipA.mask.feather = 4.0;

    TonDron::Clip clipB;
    clipB.id = QStringLiteral("clip-b");
    clipB.type = TonDron::ClipType::Video;
    clipB.timelineStart = TonDron::secondsToUs(2.0);
    clipB.timelineDuration = TonDron::secondsToUs(2.0);

    project.tracks()[0].clips.append(clipA);
    project.tracks()[0].clips.append(clipB);

    TonDron::Transition transition;
    transition.id = QStringLiteral("tr-1");
    transition.fromClipId = clipA.id;
    transition.toClipId = clipB.id;
    transition.kindId = QStringLiteral("dip");
    transition.durationUs = TonDron::secondsToUs(0.5);
    project.tracks()[0].transitions.append(transition);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.tracks()[0].clips[0].speed, 2.0);
    QCOMPARE(loaded.tracks()[0].clips[0].mask.shape, TonDron::MaskShape::Ellipse);
    QCOMPARE(loaded.tracks()[0].transitions.size(), 1);
    QCOMPARE(loaded.tracks()[0].transitions[0].kindId, QStringLiteral("dip"));
    QCOMPARE(loaded.tracks()[0].transitions[0].fromClipId, QStringLiteral("clip-a"));
}

// A segmentation result is only as durable as its matte reference: losing the path or the source
// offset on reload would silently slide the mask off the subject.
void CoreTest::matteMaskSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-matte");
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(3.0);
    clip.mask.shape = TonDron::MaskShape::Matte;
    clip.mask.mattePath = QStringLiteral("/tmp/mattes/abc.mkv");
    clip.mask.matteSrcOffsetUs = TonDron::secondsToUs(1.5);
    clip.mask.invert = true;
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Mask &mask = loaded.tracks()[0].clips[0].mask;
    QCOMPARE(mask.shape, TonDron::MaskShape::Matte);
    QCOMPARE(mask.mattePath, QStringLiteral("/tmp/mattes/abc.mkv"));
    QCOMPARE(mask.matteSrcOffsetUs, TonDron::secondsToUs(1.5));
    QCOMPARE(mask.invert, true);
}

void CoreTest::faceTrackSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-face");
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(3.0);
    clip.faceTrackPath = QStringLiteral("/tmp/facetracks/abc.json");
    clip.faceTrackSrcOffsetUs = TonDron::secondsToUs(2.25);
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    const TonDron::Clip &out = loaded.tracks()[0].clips[0];
    QCOMPARE(out.faceTrackPath, QStringLiteral("/tmp/facetracks/abc.json"));
    QCOMPARE(out.faceTrackSrcOffsetUs, TonDron::secondsToUs(2.25));

    // A project written before face tracking existed carries neither key, and must still load with
    // the clip simply having no track rather than failing.
    QJsonObject legacy = json;
    QJsonArray legacyTracks = legacy.value(QStringLiteral("tracks")).toArray();
    QJsonObject legacyTrack = legacyTracks.at(0).toObject();
    QJsonArray legacyClips = legacyTrack.value(QStringLiteral("clips")).toArray();
    QJsonObject legacyClip = legacyClips.at(0).toObject();
    legacyClip.remove(QStringLiteral("faceTrackPath"));
    legacyClip.remove(QStringLiteral("faceTrackSrcOffsetUs"));
    legacyClips.replace(0, legacyClip);
    legacyTrack.insert(QStringLiteral("clips"), legacyClips);
    legacyTracks.replace(0, legacyTrack);
    legacy.insert(QStringLiteral("tracks"), legacyTracks);

    const TonDron::Project old = TonDron::Project::fromJson(legacy, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(old.tracks()[0].clips[0].faceTrackPath.isEmpty());
    QCOMPARE(old.tracks()[0].clips[0].faceTrackSrcOffsetUs, TonDron::TimeUs(0));
}

void CoreTest::emojiClipSerialization()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});

    TonDron::Clip clip;
    clip.id = QStringLiteral("clip-emoji");
    clip.type = TonDron::ClipType::Image;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(3.0);
    clip.path = QStringLiteral("/tmp/emoji/1f600.png");
    clip.emoji = QStringLiteral("\U0001F600");
    project.tracks()[0].clips.append(clip);

    const QJsonObject json = project.toJson();
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(json, &error);

    QVERIFY(error.isEmpty());
    // The sequence is what survives a move between machines; the cached raster path does not.
    QCOMPARE(loaded.tracks()[0].clips[0].emoji, QStringLiteral("\U0001F600"));

    // A sticker or any other image clip written before the picker existed has no key at all.
    QJsonObject legacy = json;
    QJsonArray legacyTracks = legacy.value(QStringLiteral("tracks")).toArray();
    QJsonObject legacyTrack = legacyTracks.at(0).toObject();
    QJsonArray legacyClips = legacyTrack.value(QStringLiteral("clips")).toArray();
    QJsonObject legacyClip = legacyClips.at(0).toObject();
    legacyClip.remove(QStringLiteral("emoji"));
    legacyClips.replace(0, legacyClip);
    legacyTrack.insert(QStringLiteral("clips"), legacyClips);
    legacyTracks.replace(0, legacyTrack);
    legacy.insert(QStringLiteral("tracks"), legacyTracks);

    const TonDron::Project old = TonDron::Project::fromJson(legacy, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(old.tracks()[0].clips[0].emoji.isEmpty());
}

// The pre-shader enum serialized exactly these strings, so a project written by an older build
// must still resolve to the right transition package.
static TonDron::Project projectWithTransition(const QString &kindId,
                                            const QMap<QString, QVariant> &params = {})
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});

    TonDron::Clip clipA;
    clipA.id = QStringLiteral("a");
    clipA.type = TonDron::ClipType::Video;
    clipA.timelineStart = 0;
    clipA.timelineDuration = TonDron::secondsToUs(1.0);

    TonDron::Clip clipB;
    clipB.id = QStringLiteral("b");
    clipB.type = TonDron::ClipType::Video;
    clipB.timelineStart = TonDron::secondsToUs(1.0);
    clipB.timelineDuration = TonDron::secondsToUs(1.0);

    project.tracks()[0].clips.append(clipA);
    project.tracks()[0].clips.append(clipB);

    TonDron::Transition transition;
    transition.id = QStringLiteral("tr");
    transition.fromClipId = clipA.id;
    transition.toClipId = clipB.id;
    transition.kindId = kindId;
    transition.parameters = params;
    project.tracks()[0].transitions.append(transition);
    return project;
}

void CoreTest::allTransitionKindsRoundTrip()
{
    const QStringList kinds = {
        QStringLiteral("crossfade"),  QStringLiteral("dip"),        QStringLiteral("dip_white"),
        QStringLiteral("wipe_left"),  QStringLiteral("wipe_right"), QStringLiteral("wipe_up"),
        QStringLiteral("wipe_down"),  QStringLiteral("push_left"),  QStringLiteral("zoom_in"),
    };

    for (const QString &kind : kinds) {
        const TonDron::Project project = projectWithTransition(kind);
        QString error;
        const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(loaded.tracks()[0].transitions[0].kindId, kind);
    }
}

void CoreTest::transitionParametersRoundTrip()
{
    QMap<QString, QVariant> params;
    params.insert(QStringLiteral("softness"), 0.25);
    params.insert(QStringLiteral("invert"), true);

    const TonDron::Project project = projectWithTransition(QStringLiteral("luma_fade"), params);
    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const TonDron::Transition &t = loaded.tracks()[0].transitions[0];
    QCOMPARE(t.kindId, QStringLiteral("luma_fade"));
    QCOMPARE(t.parameters.value(QStringLiteral("softness")).toDouble(), 0.25);
    QCOMPARE(t.parameters.value(QStringLiteral("invert")).toBool(), true);
}

// A project file written before transitions became packages has no "parameters" key at all.
void CoreTest::legacyTransitionJsonStillLoads()
{
    QJsonObject legacy = projectWithTransition(QStringLiteral("wipe_up")).toJson();
    QJsonArray tracks = legacy.value(QStringLiteral("tracks")).toArray();
    QJsonObject track = tracks.at(0).toObject();
    QJsonArray transitions = track.value(QStringLiteral("transitions")).toArray();
    QJsonObject t = transitions.at(0).toObject();
    t.remove(QStringLiteral("parameters"));
    transitions.replace(0, t);
    track.insert(QStringLiteral("transitions"), transitions);
    tracks.replace(0, track);
    legacy.insert(QStringLiteral("tracks"), tracks);

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(legacy, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.tracks()[0].transitions[0].kindId, QStringLiteral("wipe_up"));
    QVERIFY(loaded.tracks()[0].transitions[0].parameters.isEmpty());
}

void CoreTest::transitionAudioCurves()
{
    // crossfade: linear, sums to 1 at every point.
    const auto mid = TonDron::transitionAudioGains(QStringLiteral("crossfade"), 0.5);
    QCOMPARE(mid.outgoing, 0.5);
    QCOMPARE(mid.incoming, 0.5);

    // dip: silent at the midpoint, matching the visual dip through black.
    const auto dip = TonDron::transitionAudioGains(QStringLiteral("dip"), 0.5);
    QCOMPARE(dip.outgoing, 0.0);
    QCOMPARE(dip.incoming, 0.0);
    QCOMPARE(TonDron::transitionAudioGains(QStringLiteral("dip"), 0.0).outgoing, 1.0);
    QCOMPARE(TonDron::transitionAudioGains(QStringLiteral("dip"), 1.0).incoming, 1.0);

    // hold: no ducking at all.
    const auto hold = TonDron::transitionAudioGains(QStringLiteral("hold"), 0.5);
    QCOMPARE(hold.outgoing, 1.0);
    QCOMPARE(hold.incoming, 1.0);
}

void CoreTest::physicalOverlapTransitionWindow()
{
    TonDron::Track track;
    track.type = TonDron::TrackType::Video;

    TonDron::Clip clipA;
    clipA.id = QStringLiteral("a");
    clipA.timelineStart = 0;
    clipA.timelineDuration = TonDron::secondsToUs(2.0);

    TonDron::Clip clipB;
    clipB.id = QStringLiteral("b");
    clipB.timelineStart = TonDron::secondsToUs(1.5);
    clipB.timelineDuration = TonDron::secondsToUs(2.0);

    track.clips.append(clipA);
    track.clips.append(clipB);

    QVERIFY(TonDron::clipsPhysicallyOverlap(clipA, clipB));
    QCOMPARE(TonDron::physicalOverlapDurationUs(clipA, clipB), TonDron::secondsToUs(0.5));

    TonDron::Transition transition;
    transition.fromClipId = clipA.id;
    transition.toClipId = clipB.id;
    transition.durationUs = TonDron::secondsToUs(1.0); // ignored when overlapping

    TonDron::TimeUs startUs = 0;
    TonDron::TimeUs endUs = 0;
    QVERIFY(TonDron::transitionWindow(track, transition, startUs, endUs));
    QCOMPARE(startUs, TonDron::secondsToUs(1.5));
    QCOMPARE(endUs, TonDron::secondsToUs(2.0));
}

void CoreTest::clampClipStartNoOverlapPushesPastBlockers()
{
    TonDron::Track track;
    track.type = TonDron::TrackType::Video;

    TonDron::Clip blocker;
    blocker.id = QStringLiteral("blocker");
    blocker.timelineStart = TonDron::secondsToUs(1.0);
    blocker.timelineDuration = TonDron::secondsToUs(2.0);
    track.clips.append(blocker);

    TonDron::Clip moving;
    moving.id = QStringLiteral("moving");
    moving.timelineDuration = TonDron::secondsToUs(1.0);

    const QSet<QString> exclude{moving.id};
    // Dropping into the blocker should land just after it.
    QCOMPARE(TonDron::clampClipStartNoOverlap(track, exclude, TonDron::secondsToUs(1.5),
                                            moving.timelineDuration),
             TonDron::secondsToUs(3.0));
    // Abutting the blocker is allowed.
    QCOMPARE(TonDron::clampClipStartNoOverlap(track, exclude, TonDron::secondsToUs(3.0),
                                            moving.timelineDuration),
             TonDron::secondsToUs(3.0));
    // Clear space before the blocker stays put.
    QCOMPARE(TonDron::clampClipStartNoOverlap(track, exclude, 0, moving.timelineDuration), 0);
}

void CoreTest::clampTrimEdgesIgnoreExistingOverlaps()
{
    TonDron::Track track;
    track.type = TonDron::TrackType::Video;

    TonDron::Clip left;
    left.id = QStringLiteral("left");
    left.timelineStart = 0;
    left.timelineDuration = TonDron::secondsToUs(2.0);

    TonDron::Clip mid;
    mid.id = QStringLiteral("mid");
    mid.timelineStart = TonDron::secondsToUs(1.0); // already overlaps left
    mid.timelineDuration = TonDron::secondsToUs(2.0);

    TonDron::Clip right;
    right.id = QStringLiteral("right");
    right.timelineStart = TonDron::secondsToUs(4.0);
    right.timelineDuration = TonDron::secondsToUs(1.0);

    track.clips.append(left);
    track.clips.append(mid);
    track.clips.append(right);

    const QSet<QString> excludeMid{mid.id};
    // Extending mid left must not jump past the already-overlapping left clip.
    QCOMPARE(TonDron::clampClipStartAgainstLeftNeighbors(track, excludeMid, mid.timelineStart,
                                                       TonDron::secondsToUs(0.5)),
             TonDron::secondsToUs(0.5));
    // Extending mid right stops at the abutting/gapped right neighbor.
    QCOMPARE(TonDron::clampClipEndNoOverlap(track, excludeMid, mid.timelineEnd(),
                                          TonDron::secondsToUs(4.5)),
             TonDron::secondsToUs(4.0));
}

void CoreTest::backgroundSerialization()
{
    // Default background is opaque black / Color and must survive a round-trip.
    {
        TonDron::Project project;
        const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson());
        QCOMPARE(loaded.background().kind, TonDron::BackgroundKind::Color);
        QCOMPARE(loaded.background().color, QColor(Qt::black));
    }

    // Non-default (blur + color + strength) round-trips.
    {
        TonDron::Project project;
        TonDron::Background bg;
        bg.kind = TonDron::BackgroundKind::Blur;
        bg.color = QColor(QStringLiteral("#ff2563eb"));
        bg.blurStrength = 42.0;
        project.setBackground(bg);

        QString error;
        const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(loaded.background().kind, TonDron::BackgroundKind::Blur);
        QCOMPARE(loaded.background().color, QColor(QStringLiteral("#ff2563eb")));
        QCOMPARE(loaded.background().blurStrength, 42.0);
    }

    // Projects saved before this field default to solid black.
    {
        const QJsonObject root{
            {QStringLiteral("version"), 3},
            {QStringLiteral("projectName"), QStringLiteral("NoBackground")},
            {QStringLiteral("fps"), 30},
            {QStringLiteral("width"), 1920},
            {QStringLiteral("height"), 1080},
            {QStringLiteral("tracks"), QJsonArray{}},
        };
        QString error;
        const TonDron::Project loaded = TonDron::Project::fromJson(root, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(loaded.background().kind, TonDron::BackgroundKind::Color);
        QCOMPARE(loaded.background().color, QColor(Qt::black));
    }
}

void CoreTest::fadeSerializationAndMultiplier()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});

    TonDron::Clip clip;
    clip.id = QStringLiteral("fade-clip");
    clip.type = TonDron::ClipType::Video;
    clip.timelineStart = TonDron::secondsToUs(1.0);
    clip.timelineDuration = TonDron::secondsToUs(4.0);
    clip.fadeInUs = TonDron::secondsToUs(1.0);
    clip.fadeOutUs = TonDron::secondsToUs(2.0);
    clip.fadeCurve = TonDron::FadeCurve::Linear;
    project.tracks()[0].clips.append(clip);

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const TonDron::Clip &c = loaded.tracks()[0].clips[0];
    QCOMPARE(c.fadeInUs, TonDron::secondsToUs(1.0));
    QCOMPARE(c.fadeOutUs, TonDron::secondsToUs(2.0));
    QCOMPARE(c.fadeCurve, TonDron::FadeCurve::Linear);

    // Linear ramp: at the very edges gain is 0, at the fade midpoints 0.5, and
    // fully present between the fades.
    QCOMPARE(c.fadeMultiplier(c.timelineStart), 0.0);
    QVERIFY(qAbs(c.fadeMultiplier(c.timelineStart + TonDron::secondsToUs(0.5)) - 0.5) < 1e-6);
    QVERIFY(qAbs(c.fadeMultiplier(c.timelineStart + TonDron::secondsToUs(1.5)) - 1.0) < 1e-6);
    QVERIFY(qAbs(c.fadeMultiplier(c.timelineEnd() - TonDron::secondsToUs(1.0)) - 0.5) < 1e-6);

    // Presets must diverge early in the fade so Smooth / Natural are audible and visible.
    // (Smoothstep equals Linear at t=0.5, so sample at quarter-fade.)
    TonDron::Clip smooth = c;
    smooth.fadeCurve = TonDron::FadeCurve::Smooth;
    TonDron::Clip natural = c;
    natural.fadeCurve = TonDron::FadeCurve::EqualPower;
    const TonDron::TimeUs earlyIn = c.timelineStart + TonDron::secondsToUs(0.25);
    const double linearEarly = c.fadeMultiplier(earlyIn);
    const double smoothEarly = smooth.fadeMultiplier(earlyIn);
    const double naturalEarly = natural.fadeMultiplier(earlyIn);
    QVERIFY(smoothEarly < linearEarly - 0.05);
    QVERIFY(naturalEarly > linearEarly + 0.05);

    // Custom shape round-trips and drives the multiplier.
    TonDron::Clip custom = c;
    custom.fadeCurve = TonDron::FadeCurve::Custom;
    custom.fadeShape.setPoints({QPointF(0.0, 0.0), QPointF(0.5, 0.25), QPointF(1.0, 1.0)});
    project.tracks()[0].clips[0] = custom;
    const TonDron::Project customLoaded = TonDron::Project::fromJson(project.toJson(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const TonDron::Clip &cc = customLoaded.tracks()[0].clips[0];
    QCOMPARE(cc.fadeCurve, TonDron::FadeCurve::Custom);
    QVERIFY(!cc.fadeShape.isEmpty());
    const TonDron::TimeUs midIn = c.timelineStart + TonDron::secondsToUs(0.5);
    QVERIFY(qAbs(cc.fadeMultiplier(midIn) - 0.25) < 1e-6);

    // A clip with no fades is always fully present.
    TonDron::Clip plain;
    plain.timelineStart = 0;
    plain.timelineDuration = TonDron::secondsToUs(2.0);
    QCOMPARE(plain.fadeMultiplier(TonDron::secondsToUs(1.0)), 1.0);
}

void CoreTest::clipAnimationSerializationAndSample()
{
    TonDron::Project project;
    project.tracks().clear();
    project.tracks().append(TonDron::Track{.type = TonDron::TrackType::Video});

    TonDron::Clip clip;
    clip.id = QStringLiteral("anim-clip");
    clip.type = TonDron::ClipType::Shape;
    clip.timelineStart = 0;
    clip.timelineDuration = TonDron::secondsToUs(2.0);
    clip.animIn = {TonDron::ClipAnimKind::Fade, TonDron::secondsToUs(1.0), TonDron::ClipAnimEase::Linear,
                   TonDron::FadeCurve::Linear};
    clip.animOut = {TonDron::ClipAnimKind::ZoomIn, TonDron::secondsToUs(0.5), TonDron::ClipAnimEase::EaseOut,
                    TonDron::FadeCurve::EqualPower};
    project.tracks()[0].clips.append(clip);

    QString error;
    const TonDron::Project loaded = TonDron::Project::fromJson(project.toJson(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const TonDron::Clip &c = loaded.tracks()[0].clips[0];
    QCOMPARE(c.animIn.kind, TonDron::ClipAnimKind::Fade);
    QCOMPARE(c.animIn.durationUs, TonDron::secondsToUs(1.0));
    QCOMPARE(c.animIn.curve, TonDron::FadeCurve::Linear);
    QCOMPARE(c.animOut.kind, TonDron::ClipAnimKind::ZoomIn);
    QCOMPARE(c.animOut.curve, TonDron::FadeCurve::EqualPower);

    // Fade kind owns opacity via fadeMultiplier (not body-anim sample).
    QVERIFY(qAbs(c.fadeMultiplier(TonDron::secondsToUs(0.5)) - 0.5) < 1e-6);
    const TonDron::ClipAnimSample midIn =
        TonDron::evaluateClipAnimation(c.timelineStart, c.timelineDuration, c.animIn, {},
                                     TonDron::secondsToUs(0.5), 100.0, 100.0);
    QVERIFY(qAbs(midIn.opacity - 1.0) < 1e-6);

    TonDron::Clip zoom;
    zoom.timelineStart = 0;
    zoom.timelineDuration = TonDron::secondsToUs(2.0);
    zoom.animIn = {TonDron::ClipAnimKind::ZoomIn, TonDron::secondsToUs(1.0), TonDron::ClipAnimEase::Linear,
                   TonDron::FadeCurve::Linear};
    const TonDron::ClipAnimSample zoomMid =
        TonDron::evaluateClipAnimation(zoom.timelineStart, zoom.timelineDuration, zoom.animIn, {},
                                     TonDron::secondsToUs(0.5), 100.0, 100.0);
    QVERIFY(qAbs(zoomMid.scale - 0.8) < 1e-6); // 0.6 + 0.4 * 0.5
    QVERIFY(zoomMid.scale < 1.0);

    // Smooth style bends motion progress vs linear at quarter-time.
    TonDron::Clip smoothZoom = zoom;
    smoothZoom.animIn.curve = TonDron::FadeCurve::Smooth;
    const TonDron::ClipAnimSample smoothMid =
        TonDron::evaluateClipAnimation(smoothZoom.timelineStart, smoothZoom.timelineDuration,
                                     smoothZoom.animIn, {}, TonDron::secondsToUs(0.25), 100.0, 100.0);
    const TonDron::ClipAnimSample linearQuarter =
        TonDron::evaluateClipAnimation(zoom.timelineStart, zoom.timelineDuration, zoom.animIn, {},
                                     TonDron::secondsToUs(0.25), 100.0, 100.0);
    QVERIFY(smoothMid.scale < linearQuarter.scale - 0.01);
}

// A canvas resize must not move or rescale anything: clips that relied on the
// implicit full-canvas size get that size frozen, so they overflow the smaller
// frame instead of shrinking with it.
void CoreTest::rebaseClipLayoutFreezesImplicitSize()
{
    TonDron::Project project;
    project.setResolution(1920, 1080);

    TonDron::Track track;
    track.type = TonDron::TrackType::Video;

    TonDron::Clip implicitSize; // no transform keyframes at all
    implicitSize.type = TonDron::ClipType::Video;
    track.clips.append(implicitSize);

    TonDron::Clip explicitSize;
    explicitSize.type = TonDron::ClipType::Image;
    explicitSize.transformW.setKeyframe(0, 640.0);
    explicitSize.transformH.setKeyframe(0, 360.0);
    explicitSize.transformX.setKeyframe(0, 100.0);
    explicitSize.transformY.setKeyframe(0, 50.0);
    track.clips.append(explicitSize);

    TonDron::Clip audio; // audio carries no layout; must be left alone
    audio.type = TonDron::ClipType::Audio;
    track.clips.append(audio);

    project.tracks().clear(); // drop the default timeline; this test owns the document
    project.tracks().append(track);

    // Crop to a 1520x1080 window starting 400px in from the left.
    TonDron::rebaseClipLayout(project, 1920, 1080, 400.0, 0.0);
    project.setResolution(1520, 1080);

    const TonDron::Track &out = project.tracks().at(0);

    // The implicit clip keeps its original 1920x1080 footprint and is pushed
    // left by the crop origin, so it now overflows both sides of the frame.
    QCOMPARE(out.clips.at(0).transformW.evaluateAt(0), 1920.0);
    QCOMPARE(out.clips.at(0).transformH.evaluateAt(0), 1080.0);
    QCOMPARE(out.clips.at(0).transformX.evaluateAt(0), -400.0);
    QCOMPARE(out.clips.at(0).transformY.evaluateAt(0), 0.0);

    // Explicit sizes are untouched; only the position shifts.
    QCOMPARE(out.clips.at(1).transformW.evaluateAt(0), 640.0);
    QCOMPARE(out.clips.at(1).transformH.evaluateAt(0), 360.0);
    QCOMPARE(out.clips.at(1).transformX.evaluateAt(0), -300.0);
    QCOMPARE(out.clips.at(1).transformY.evaluateAt(0), 50.0);

    QVERIFY(out.clips.at(2).transformW.isEmpty());
    QVERIFY(out.clips.at(2).transformX.isEmpty());
}

// Animated positions must shift wholesale, so the motion path is preserved
// relative to the content rather than being flattened to one value.
void CoreTest::rebaseClipLayoutShiftsKeyframedPosition()
{
    TonDron::Project project;
    project.setResolution(1920, 1080);

    TonDron::Track track;
    track.type = TonDron::TrackType::Video;

    TonDron::Clip clip;
    clip.type = TonDron::ClipType::Video;
    clip.transformX.setKeyframe(0, 0.0);
    clip.transformX.setKeyframe(TonDron::secondsToUs(2.0), 800.0);
    clip.transformY.setKeyframe(0, 200.0);
    track.clips.append(clip);

    project.tracks().clear(); // drop the default timeline; this test owns the document
    project.tracks().append(track);

    TonDron::rebaseClipLayout(project, 1920, 1080, 120.0, 60.0);

    const TonDron::Clip &out = project.tracks().at(0).clips.at(0);
    QCOMPARE(out.transformX.keyframes().size(), 2);
    QCOMPARE(out.transformX.evaluateAt(0), -120.0);
    QCOMPARE(out.transformX.evaluateAt(TonDron::secondsToUs(2.0)), 680.0);
    QCOMPARE(out.transformY.evaluateAt(0), 140.0);
}

QTEST_MAIN(CoreTest)
#include "tst_core.moc"

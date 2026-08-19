cmake_minimum_required(VERSION 3.21)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Oldest macOS release TonDron runs on")

project(TonDron VERSION 0.2.2 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(WITH_BGREMOVAL "Enable OpenCV-based background removal" OFF)

set(TonDron_ADDON_INDEX_URL "https://TonDron-addons.cutwire.org/v1/index"
    CACHE STRING "Addon index endpoint; empty disables the addon service")
set(TonDron_ADDON_CLIENT_TOKEN "79a388bba9301cacc81cc6eb0724c81e948e1b27c69aee1b"
    CACHE STRING "Client token sent to the addon service as X-TonDron-Client")

set(TonDron_UPDATE_FEED_URL "https://api.github.com/repos/CutWire-Studios/TonDron/releases/latest"
    CACHE STRING "Release feed polled for updates; empty disables the update check")

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Multimedia Test Concurrent Widgets OpenGL Network QuickEffects LinguistTools)
find_package(FFmpeg REQUIRED)

option(TonDron_FETCH_ONNXRUNTIME
    "Download prebuilt ONNX Runtime headers when they are not installed (cmake/FetchOnnxRuntime.cmake)" ON)
option(TonDron_BUNDLE_ONNXRUNTIME
    "Stage a CPU ONNX Runtime into the build tree for development" ON)
find_package(OnnxRuntime QUIET)
if(TonDron_BUNDLE_ONNXRUNTIME AND OnnxRuntime_FOUND AND NOT ONNXRUNTIME_RUNTIME_LIBS
   AND TonDron_FETCH_ONNXRUNTIME)
    message(STATUS "ONNX Runtime headers found without a library; downloading one to bundle")
    set(OnnxRuntime_FOUND FALSE)
endif()
if(NOT OnnxRuntime_FOUND AND TonDron_FETCH_ONNXRUNTIME)
    include(FetchOnnxRuntime)
    find_package(OnnxRuntime REQUIRED)
endif()
if(NOT OnnxRuntime_FOUND)
    message(FATAL_ERROR
        "ONNX Runtime headers not found. Install them and set ONNXRUNTIME_ROOT, "
        "or enable TonDron_FETCH_ONNXRUNTIME to download them automatically.")
endif()
message(STATUS "ONNX Runtime headers: ${OnnxRuntime_INCLUDE_DIRS}")
if(TonDron_BUNDLE_ONNXRUNTIME AND NOT ONNXRUNTIME_RUNTIME_LIBS)
    message(WARNING
        "TonDron_BUNDLE_ONNXRUNTIME is on but no ONNX Runtime library was found to stage. "
        "The build tree will have no runtime; install an Acceleration addon or point "
        "TonDron_ONNXRUNTIME_DIR at an extracted release.")
endif()

if(WITH_BGREMOVAL)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
endif()

find_package(PkgConfig QUIET)
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_shared)
    set(TonDron_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd)
    set(TonDron_ZSTD_TARGET zstd::libzstd)
elseif(PkgConfig_FOUND)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    set(TonDron_ZSTD_TARGET PkgConfig::ZSTD)
else()
    message(FATAL_ERROR "libzstd not found: no zstd CMake config and no pkg-config to fall back on")
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

if(PkgConfig_FOUND)
    pkg_check_modules(SOUNDTOUCH QUIET IMPORTED_TARGET soundtouch)
endif()
if(TARGET PkgConfig::SOUNDTOUCH)
    set(TonDron_SOUNDTOUCH_TARGET PkgConfig::SOUNDTOUCH)
    get_target_property(_st_pc_includes PkgConfig::SOUNDTOUCH INTERFACE_INCLUDE_DIRECTORIES)
    if(_st_pc_includes)
        set(_st_usable "")
        foreach(_dir IN LISTS _st_pc_includes)
            if(EXISTS "${_dir}/soundtouch/SoundTouch.h")
                list(APPEND _st_usable "${_dir}")
            elseif(EXISTS "${_dir}/SoundTouch.h")
                get_filename_component(_parent "${_dir}" DIRECTORY)
                if(EXISTS "${_parent}/soundtouch/SoundTouch.h")
                    list(APPEND _st_usable "${_parent}")
                else()
                    list(APPEND _st_usable "${_dir}")
                endif()
            elseif(IS_DIRECTORY "${_dir}")
                list(APPEND _st_usable "${_dir}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _st_usable)
        set_target_properties(PkgConfig::SOUNDTOUCH PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_st_usable}")
    endif()
else()
    find_package(SoundTouch CONFIG REQUIRED)
    set(TonDron_SOUNDTOUCH_TARGET SoundTouch::SoundTouch)
    get_target_property(_st_includes SoundTouch::SoundTouch INTERFACE_INCLUDE_DIRECTORIES)
    if(_st_includes)
        set(_st_usable "")
        foreach(_dir IN LISTS _st_includes)
            if(IS_DIRECTORY "${_dir}")
                list(APPEND _st_usable "${_dir}")
            else()
                message(STATUS "SoundTouch: ignoring exported include dir that does not exist: ${_dir}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _st_usable)
        set_target_properties(SoundTouch::SoundTouch PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_st_usable}")
    endif()
endif()

set(JUCE_MODULES_ONLY ON CACHE BOOL "Only configure the JUCE modules" FORCE)

option(TonDron_FETCH_JUCE "Download JUCE when TonDron_JUCE_DIR is not set" ON)
set(TonDron_JUCE_DIR "" CACHE STRING
    "JUCE source tree — absolute, or relative to this file; overrides TonDron_FETCH_JUCE")
set(TonDron_JUCE_TAG "8.0.15" CACHE STRING "JUCE release tag to fetch when TonDron_JUCE_DIR is unset")

if(TonDron_JUCE_DIR)
    if(IS_ABSOLUTE "${TonDron_JUCE_DIR}")
        set(_TonDron_juce_dir "${TonDron_JUCE_DIR}")
    else()
        set(_TonDron_juce_dir "${CMAKE_CURRENT_SOURCE_DIR}/${TonDron_JUCE_DIR}")
    endif()
    if(NOT EXISTS "${_TonDron_juce_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "TonDron_JUCE_DIR=${TonDron_JUCE_DIR} does not point at a JUCE source tree "
            "(looked in ${_TonDron_juce_dir}).")
    endif()
    message(STATUS "JUCE: using ${_TonDron_juce_dir}")
    add_subdirectory("${_TonDron_juce_dir}" "${CMAKE_BINARY_DIR}/juce-build")
elseif(TonDron_FETCH_JUCE)
    message(STATUS "JUCE: fetching ${TonDron_JUCE_TAG}")
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${TonDron_JUCE_TAG}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(JUCE)
else()
    message(FATAL_ERROR
        "JUCE not available. Point TonDron_JUCE_DIR at a JUCE source tree, "
        "or enable TonDron_FETCH_JUCE to download it automatically.")
endif()

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

# --- core ---
add_library(TonDroncore STATIC
    src/core/Effect.h
    src/core/Effect.cpp
    src/core/EffectPreset.h
    src/core/MediaAsset.h
    src/core/MediaAsset.cpp
    src/core/Clip.h
    src/core/Clip.cpp
    src/core/Bezier.h
    src/core/SpeedCurve.h
    src/core/SpeedCurve.cpp
    src/core/FadeShape.h
    src/core/FadeShape.cpp
    src/core/ClipAnimation.h
    src/core/ClipAnimation.cpp
    src/core/SubtitleCue.h
    src/core/SubtitleCue.cpp
    src/core/SrtIO.h
    src/core/SrtIO.cpp
    src/core/Mask.h
    src/core/Mask.cpp
    src/core/Transition.h
    src/core/Transition.cpp
    src/core/TextStyle.h
    src/core/TextStyle.cpp
    src/core/ShapeStyle.h
    src/core/ShapeStyle.cpp
    src/core/ShapePath.h
    src/core/ShapePath.cpp
    src/core/Track.h
    src/core/Track.cpp
    src/core/Project.h
    src/core/Project.cpp
    src/core/TimelineOps.h
    src/core/TimelineOps.cpp
    src/core/commands/ProjectCommands.h
    src/core/commands/ProjectCommands.cpp
)
target_include_directories(TonDroncore PUBLIC src)
target_link_libraries(TonDroncore PUBLIC Qt6::Core Qt6::Gui)

add_library(TonDronaudiodsp STATIC
    src/engine/audio/AudioEffectProcessor.h
    src/engine/audio/AudioEffectProcessor.cpp
    src/engine/audio/FilterProcessors.h
    src/engine/audio/FilterProcessors.cpp
    src/engine/audio/DynamicsProcessors.h
    src/engine/audio/DynamicsProcessors.cpp
    src/engine/audio/ModulationProcessors.h
    src/engine/audio/ModulationProcessors.cpp
    src/engine/audio/SoundTouchPitchProcessor.h
    src/engine/audio/SoundTouchPitchProcessor.cpp
    src/engine/audio/AudioEffectFactory.h
    src/engine/audio/AudioEffectFactory.cpp
    src/engine/audio/AudioEffectRack.h
    src/engine/audio/AudioEffectRack.cpp
    src/engine/audio/ClipAudioRetimer.h
    src/engine/audio/ClipAudioRetimer.cpp
)
set_target_properties(TonDronaudiodsp PROPERTIES AUTOMOC OFF)
target_include_directories(TonDronaudiodsp PUBLIC src)
target_link_libraries(TonDronaudiodsp PUBLIC TonDroncore)
target_link_libraries(TonDronaudiodsp PRIVATE juce::juce_dsp juce::juce_recommended_config_flags
    ${TonDron_SOUNDTOUCH_TARGET})
target_compile_definitions(TonDronaudiodsp PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_DISABLE_JUCE_VERSION_PRINTING=1
)

# --- engine ---
add_library(TonDronengine STATIC
    src/engine/AddonPackage.h
    src/engine/AddonPackage.cpp
    src/engine/AddonRegistry.h
    src/engine/AddonRegistry.cpp
    src/engine/AddonSigningKey.h
    src/engine/MediaProbe.h
    src/engine/MediaProbe.cpp
    src/engine/MediaThumbnail.h
    src/engine/MediaThumbnail.cpp
    src/engine/FilmstripTileCache.h
    src/engine/FilmstripTileCache.cpp
    src/engine/MediaWaveform.h
    src/engine/MediaWaveform.cpp
    src/engine/WaveformBlockCache.h
    src/engine/WaveformBlockCache.cpp
    src/engine/AudioOnsets.h
    src/engine/AudioOnsets.cpp
    src/engine/Exporter.h
    src/engine/Exporter.cpp
    src/engine/ClipReader.h
    src/engine/ClipReader.cpp
    src/engine/ClipReaderWorker.h
    src/engine/ClipReaderWorker.cpp
    src/engine/ClipReaderPool.h
    src/engine/ClipReaderPool.cpp
    src/engine/EffectProcessor.h
    src/engine/EffectProcessor.cpp
    src/engine/EffectCatalog.h
    src/engine/EffectCatalog.cpp
    src/engine/FontCatalog.h
    src/engine/FontCatalog.cpp
    src/engine/StickerCatalog.h
    src/engine/StickerCatalog.cpp
    src/engine/EmojiCatalog.h
    src/engine/EmojiCatalog.cpp
    src/engine/EmojiTable.h
    src/engine/EmojiTable.cpp
    src/engine/TextRaster.h
    src/engine/TextRaster.cpp
    src/engine/EffectPackageLoader.h
    src/engine/EffectPackageLoader.cpp
    src/engine/TransitionCatalog.h
    src/engine/TransitionCatalog.cpp
    src/engine/TransitionPackageLoader.h
    src/engine/TransitionPackageLoader.cpp
    src/engine/GpuPackageParse.h
    src/engine/GpuPackageParse.cpp
    src/engine/GpuEffectDefinition.h
    src/engine/GpuCompositor.h
    src/engine/GpuCompositor.cpp
    src/engine/GlRuntime.h
    src/engine/GlRuntime.cpp
    src/engine/GpuEffectExecutor.h
    src/engine/GpuEffectExecutor.cpp
    src/engine/CompositorFrameHistory.h
    src/engine/CompositorFrameHistory.cpp
    src/engine/FrameCompositor.h
    src/engine/FrameCompositor.cpp
    src/engine/MaskApplier.h
    src/engine/MaskApplier.cpp
    src/engine/AudioEffectCatalog.h
    src/engine/AudioEffectCatalog.cpp
    src/engine/EffectTemplateCatalog.h
    src/engine/EffectTemplateCatalog.cpp
    src/engine/AudioMixer.h
    src/engine/AudioMixer.cpp
    src/engine/WhisperTokenizer.h
    src/engine/WhisperTokenizer.cpp
    src/engine/WhisperTranscriber.h
    src/engine/WhisperTranscriber.cpp
    src/engine/OrtSupport.h
    src/engine/OrtRuntime.h
    src/engine/OrtRuntime.cpp
    src/engine/Sam2Segmenter.h
    src/engine/Sam2Segmenter.cpp
    src/engine/FaceLandmarker.h
    src/engine/FaceLandmarker.cpp
    src/engine/FaceTrack.h
    src/engine/FaceTrack.cpp
    src/engine/DeepFilterDenoiser.h
    src/engine/DeepFilterDenoiser.cpp
    src/engine/MatteWriter.h
    src/engine/MatteWriter.cpp
    src/engine/ReverseProxyCache.h
    src/engine/ReverseProxyCache.cpp
    src/engine/ReverseRenderer.h
    src/engine/ReverseRenderer.cpp
    src/engine/AudioFileWriter.h
    src/engine/AudioFileWriter.cpp
    src/engine/ProjectBundle.h
    src/engine/ProjectBundle.cpp
    src/engine/ProjectDependencies.h
    src/engine/ProjectDependencies.cpp
)

target_include_directories(TonDronengine PUBLIC src)
target_link_libraries(TonDronengine PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL FFmpeg::FFmpeg TonDroncore TonDronaudiodsp onnxruntime::headers)
target_link_libraries(TonDronengine PRIVATE ${TonDron_ZSTD_TARGET} OpenSSL::Crypto ${CMAKE_DL_LIBS})
target_compile_definitions(TonDronengine PRIVATE TonDron_VERSION="${PROJECT_VERSION}")

if(WITH_BGREMOVAL)
    target_include_directories(TonDronengine PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(TonDronengine PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(TonDronengine PUBLIC TonDron_WITH_BGREMOVAL)
endif()

# --- application - UNICO y DEFINITIVO ---
qt_add_executable(tondron-videoedit
    src/main.cpp
    src/TonDronImageProvider.cpp
    src/SegmentImageProvider.cpp
    src/SegmentImageStore.cpp
    src/ClipPreviewImageProvider.cpp
    src/ClipPreviewImageStore.cpp
    src/TextStylePreviewImageProvider.cpp
    src/models/AddonManager.cpp
    src/models/UpdateChecker.cpp
    src/models/AppController.cpp
    src/models/FileDialogs.cpp
    src/models/AssetLibrary.cpp
    src/models/TimelineModel.cpp
    src/models/ClipListModel.cpp
    src/models/EditorState.h
    src/mcp/McpSession.cpp
    src/mcp/McpCatalog.cpp
    src/mcp/McpProtocol.cpp
    src/mcp/McpHttp.cpp
    src/mcp/McpDispatcher.cpp
    src/mcp/McpDispatcherExtended.cpp
    src/mcp/McpServer.cpp
    src/mcp/McpStdio.cpp
    src/playback/PlaybackEngine.cpp
    src/playback/PlaybackClock.cpp
    src/playback/ClipPreviewPlayer.cpp
    src/playback/CompositorService.cpp
    src/preview/PreviewItem.cpp
)

target_include_directories(tondron-videoedit PUBLIC src)
target_link_libraries(tondron-videoedit PRIVATE TonDronengine)

if(WIN32)
    set_target_properties(tondron-videoedit PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(tondron-videoedit PRIVATE resources/windows/TonDron.rc)
    set_source_files_properties(resources/windows/TonDron.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/resources/windows/TonDron.ico")
endif()

if(APPLE)
    set(TonDron_MACOS_EXECUTABLE "TonDron")
    set(TonDron_MACOS_BUNDLE_ID "com.tondron.TonDron")
    set(TonDron_MACOS_ICON_FILE "TonDron.icns")
    configure_file(
        "${CMAKE_SOURCE_DIR}/resources/macos/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        @ONLY)
    set(TonDron_MACOS_ICNS "${CMAKE_SOURCE_DIR}/resources/macos/${TonDron_MACOS_ICON_FILE}")
    target_sources(tondron-videoedit PRIVATE "${TonDron_MACOS_ICNS}")
    set_source_files_properties("${TonDron_MACOS_ICNS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    set_target_properties(tondron-videoedit PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "${TonDron_MACOS_EXECUTABLE}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
endif()

set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/TonDron.png PROPERTIES
  QT_RESOURCE_ALIAS "TonDron.png"
)
qt_add_resources(tondron-videoedit "TonDron_app_icon"
  PREFIX "/app"
  FILES
    ${CMAKE_SOURCE_DIR}/resources/TonDron.png
)

set_source_files_properties(src/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE true)
set_source_files_properties(src/qml/Toasts.qml PROPERTIES QT_QML_SINGLETON_TYPE true)

file(GLOB TonDron_LUCIDE_ICONS RELATIVE "${CMAKE_SOURCE_DIR}" CONFIGURE_DEPENDS "resources/icons/*.png")

qt_add_qml_module(tondron-videoedit
  URI TonDron
  VERSION 1.0
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/TonDron
  QML_FILES
        src/qml/Main.qml
        src/qml/Theme.qml
        src/qml/Toasts.qml
        src/qml/EditorHeader.qml
        src/qml/AssetsPanel.qml
        src/qml/PreviewPanel.qml
        src/qml/components/preview/TransformOverlay.qml
        src/qml/components/preview/CropOverlay.qml
        src/qml/components/preview/PreviewToolbar.qml
        src/qml/SegmentationWindow.qml
        src/qml/DenoiseWindow.qml
        src/qml/SpeedCurveWindow.qml
        src/qml/FadeCurveWindow.qml
        src/qml/PropertiesPanel.qml
        src/qml/components/properties/GeneralInspector.qml
        src/qml/components/properties/TextInspector.qml
        src/qml/components/properties/TransformInspector.qml
        src/qml/components/properties/AudioInspector.qml
        src/qml/components/properties/AudioEffectsInspector.qml
        src/qml/components/properties/SpeedFadeInspector.qml
        src/qml/components/properties/AnimationInspector.qml
        src/qml/components/properties/TransitionInspector.qml
        src/qml/components/properties/BlendingInspector.qml
        src/qml/components/properties/ShapeInspector.qml
        src/qml/components/properties/MasksInspector.qml
        src/qml/components/properties/EffectsInspector.qml
        src/qml/components/assets/TextAssetsTab.qml
        src/qml/components/assets/SubtitlesTab.qml
        src/qml/components/assets/StickersTab.qml
        src/qml/components/assets/ShapesTab.qml
        src/qml/components/assets/SoundsTab.qml
        src/qml/components/assets/SettingsTab.qml
        src/qml/components/assets/ShortcutsTab.qml
        src/qml/components/assets/MediaAssetsTab.qml
        src/qml/components/assets/AssetCategoryChips.qml
        src/qml/components/assets/AssetFavoriteButton.qml
        src/qml/TimelinePanel.qml
        src/qml/components/timeline/TimelineToolbar.qml
        src/qml/components/timeline/TrackHeaderColumn.qml
        src/qml/components/timeline/TimelineClipItem.qml
        src/qml/components/IconGlyph.qml
        src/qml/components/IconButton.qml
        src/qml/components/PanelFrame.qml
        src/qml/components/PanelSplitHandle.qml
        src/qml/components/ClipFilmstrip.qml
        src/qml/components/AppScrollBar.qml
        src/qml/components/KeyframeDiamond.qml
        src/qml/components/PropertyKeyframeRow.qml
        src/qml/components/KeyframeGraph.qml
        src/qml/components/SubtitleCueLane.qml
        src/qml/components/ShortcutCaptureField.qml
        src/qml/components/ShapePreview.qml
        src/qml/components/ColorSwatchField.qml
        src/qml/components/EffectBrowser.qml
        src/qml/components/AudioEffectBrowser.qml
        src/qml/components/EffectTemplateBrowser.qml
        src/qml/components/ThemedSlider.qml
        src/qml/components/ThemedTextField.qml
        src/qml/components/ThemedNumberField.qml
        src/qml/components/ThemedTextArea.qml
        src/qml/components/SubtitleEditor.qml
        src/qml/components/ThemedComboBox.qml
        src/qml/components/FontPicker.qml
        src/qml/components/CollapsibleSection.qml
        src/qml/components/TextStylePackThumb.qml
        src/qml/components/TextStylePackPicker.qml
        src/qml/components/EmojiPicker.qml
        src/qml/components/ThemedButton.qml
        src/qml/components/ThemedChip.qml
        src/qml/components/ThemedProgressBar.qml
        src/qml/components/ThemedLabel.qml
        src/qml/components/ThemedDialog.qml
        src/qml/components/AddonManagerDialog.qml
        src/qml/components/AddonStartupDialog.qml
        src/qml/components/MissingAddonsDialog.qml
        src/qml/components/UpdateDialog.qml
        src/qml/components/ProjectSetupDialog.qml
        src/qml/components/LayoutChooserDialog.qml
        src/qml/components/ProjectPropertiesDialog.qml
        src/qml/components/PackageProgressDialog.qml
        src/qml/components/ExportDialog.qml
        src/qml/components/ExportProgressDialog.qml
        src/qml/components/ReverseProgressDialog.qml
        src/qml/components/SubtitleProgressDialog.qml
        src/qml/components/CircularProgress.qml
        src/qml/components/RecoveryDialog.qml
        src/qml/components/UnsavedChangesDialog.qml
        src/qml/components/RecentProjectsPopup.qml
        src/qml/components/NewTrackMenu.qml
        src/qml/components/ToastHost.qml
        src/qml/components/EmptyState.qml
        src/qml/components/SkeletonBox.qml
        src/qml/components/ThemedSwitch.qml
        src/qml/components/ThemedCheckBox.qml
        src/qml/components/ThemedToggleButton.qml
        src/qml/components/ThemedToolTip.qml
        src/qml/components/ThemedContextMenu.qml
        src/qml/components/ThemedMenuItem.qml
        src/qml/components/ThemedMenuSeparator.qml
        src/qml/components/LabelledProgressRing.qml
    RESOURCES
        resources/fonts/Inter.ttf
        ${TonDron_LUCIDE_ICONS}
)

target_link_libraries(tondron-videoedit PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickEffects
    Qt6::Multimedia
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    TonDroncore
    TonDronengine
)
target_include_directories(tondron-videoedit PRIVATE src src/preview)

file(GLOB TonDron_MCP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/mcp/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/mcp/*.h")
if(TonDron_MCP_SOURCES)
    set_source_files_properties(${TonDron_MCP_SOURCES} PROPERTIES QT_EXCLUDE_FROM_TRANSLATION ON)
endif()

file(GLOB TonDron_TS_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/i18n/TonDron_*.ts")
qt_add_translations(tondron-videoedit
    TS_FILES
        "${CMAKE_SOURCE_DIR}/i18n/TonDron.ts"
        ${TonDron_TS_FILES}
    RESOURCE_PREFIX "/i18n"
    SOURCES
        src/engine/EffectCatalog.cpp
        src/engine/EffectTemplateCatalog.cpp
        src/engine/AudioEffectCatalog.cpp
        src/engine/TransitionCatalog.cpp
        src/engine/FontCatalog.cpp
        src/engine/ReverseRenderer.cpp
        src/engine/ProjectBundle.cpp
        src/core/Project.cpp
        src/core/SrtIO.cpp
        src/core/ShapeStyle.cpp
        src/core/TextStyle.cpp
        src/core/SubtitleCue.cpp
    LUPDATE_OPTIONS -no-obsolete -locations relative
)
if(NOT TARGET update_translations AND TARGET TonDron_lupdate)
    add_custom_target(update_translations DEPENDS TonDron_lupdate)
endif()

target_compile_definitions(tondron-videoedit PRIVATE
    TonDron_ADDON_INDEX_URL="${TonDron_ADDON_INDEX_URL}"
    TonDron_ADDON_CLIENT_TOKEN="${TonDron_ADDON_CLIENT_TOKEN}"
    TonDron_UPDATE_FEED_URL="${TonDron_UPDATE_FEED_URL}"
    TonDron_VERSION="${PROJECT_VERSION}"
)

function(TonDron_stage_packages target assetdir)
    get_target_property(_bundle ${target} MACOSX_BUNDLE)
    if(_bundle)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/${assetdir}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/${assetdir}"
            COMMENT "Copy ${assetdir} packages into ${target}.app/Contents/Resources")
        return()
    endif()
    get_target_property(_outdir ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT _outdir)
        set(_outdir "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    file(RELATIVE_PATH _key "${CMAKE_BINARY_DIR}" "${_outdir}/${assetdir}")
    string(MAKE_C_IDENTIFIER "TonDron_stage_${_key}" _stage)
    get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_multi_config)
        set(_outdir "${_outdir}/$<CONFIG>")
    endif()
    if(NOT TARGET ${_stage})
        add_custom_target(${_stage}
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/${assetdir}"
                    "${_outdir}/${assetdir}"
            COMMENT "Copy ${assetdir} packages into ${_outdir}"
        )
    endif()
    add_dependencies(${target} ${_stage})
endfunction()

function(TonDron_copy_effects target)
    TonDron_stage_packages(${target} effects)
endfunction()

function(TonDron_copy_transitions target)
    TonDron_stage_packages(${target} transitions)
endfunction()

function(TonDron_copy_audio_effects target)
    TonDron_stage_packages(${target} audio-effects)
endfunction()

function(TonDron_bundle_onnxruntime target)
    if(TonDron_BUNDLE_ONNXRUNTIME AND ONNXRUNTIME_RUNTIME_LIBS)
        get_target_property(_bundle ${target} MACOSX_BUNDLE)
        if(_bundle)
            set(_ortdir "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/onnxruntime/lib")
        else()
            set(_ortdir "$<TARGET_FILE_DIR:${target}>/onnxruntime/lib")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_ortdir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${ONNXRUNTIME_RUNTIME_LIBS}
                    "${_ortdir}"
            COMMENT "Stage onnxruntime into ${target}'s build tree"
        )
    endif()
endfunction()

function(TonDron_copy_effect_templates target)
    TonDron_stage_packages(${target} effect-templates)
endfunction()

TonDron_copy_effects(tondron-videoedit)
TonDron_copy_effect_templates(tondron-videoedit)
TonDron_copy_transitions(tondron-videoedit)
TonDron_copy_audio_effects(tondron-videoedit)
TonDron_bundle_onnxruntime(tondron-videoedit)

include(GNUInstallDirs)

if(APPLE)
    install(TARGETS tondron-videoedit BUNDLE DESTINATION . COMPONENT tondron-videoedit)
else()
    install(TARGETS tondron-videoedit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tondron-videoedit)
    foreach(_assetdir effects transitions effect-templates audio-effects)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/${_assetdir}
                DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tondron-videoedit)
    endforeach()
    install(FILES flatpak/com.tondron.TonDron.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT tondron-videoedit)
    install(FILES flatpak/com.tondron.TonDron.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo COMPONENT tondron-videoedit)
    foreach(_size 64 128 256 512)
        install(FILES
            flatpak/share/icons/hicolor/${_size}x${_size}/apps/com.tondron.TonDron.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps COMPONENT tondron-videoedit)
    endforeach()
endif()

add_subdirectory(tools)
enable_testing()
add_subdirectory(tests)

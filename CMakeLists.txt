cmake_minimum_required(VERSION 3.22)
project(SeePosition VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.7
)

FetchContent_MakeAvailable(juce)

juce_add_gui_app(SeePosition
    PRODUCT_NAME "SeePosition"
)

juce_generate_juce_header(SeePosition)

target_sources(SeePosition
    PRIVATE
        main.cpp
        Source/AudioCaptureService.cpp
        Source/AudioCaptureService.h
        Source/Dsp/BandAnalyzer.h
        Source/Dsp/DeltaEq.h
        Source/Dsp/FingerprintGate.h
        Source/Dsp/VoiceGate.h
        Source/MainComponent.cpp
        Source/MainComponent.h
        Source/WasapiLoopbackCapture.cpp
        Source/WasapiLoopbackCapture.h
)

target_compile_definitions(SeePosition
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(SeePosition
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# 指纹模板包必须和 exe 放在一起，否则人声门自动失效（不影响启动）
add_custom_command(TARGET SeePosition POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/fingerprints.ewg"
        "$<TARGET_FILE_DIR:SeePosition>/fingerprints.ewg"
    COMMENT "复制 fingerprints.ewg 到输出目录")

if (MSVC)
    target_compile_options(SeePosition PRIVATE /utf-8)
    
    # Add Windows resource file for application icon
    target_sources(SeePosition PRIVATE icon/resource.rc)
endif()


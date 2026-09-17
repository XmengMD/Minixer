/*

    IMPORTANT! This file is auto-generated each time you save your
    project - if you alter its contents, your changes may be overwritten!

    This is the header file that your files should include in order to get all the
    JUCE library headers. You should avoid including the JUCE headers directly in
    your own source files, because that wouldn't pick up the correct configuration
    options for your app.

*/

#pragma once


#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>


#if defined (JUCE_PROJUCER_VERSION) && JUCE_PROJUCER_VERSION < JUCE_VERSION
 /** If you've hit this error then the version of the Projucer that was used to generate this project is
     older than the version of the JUCE modules being included. To fix this error, re-save your project
     using the latest version of the Projucer or, if you aren't using the Projucer to manage your project,
     remove the JUCE_PROJUCER_VERSION define.
 */
 #error "This project was last saved using an outdated version of the Projucer! Re-save this project with the latest version to fix this error."
#endif


#if ! JUCE_DONT_DECLARE_PROJECTINFO
namespace ProjectInfo
{
    const char* const  projectName    = "Minixer";
    const char* const  companyName    = "";
    // 版本字符串/版本号统一由 CMake（CMakeLists.txt 顶部）派生并注入宏；
    // 未通过 CMake 编译（如 Projucer 直接导出）时使用下列兜底值。
#if defined (MINIXER_DISPLAY_VERSION)
    const char* const  versionString  = MINIXER_DISPLAY_VERSION;
#else
    const char* const  versionString  = "0.4.1 Beta";
#endif
#if defined (MINIXER_VERSION_MAJOR) && defined (MINIXER_VERSION_MINOR) && defined (MINIXER_VERSION_PATCH)
    const int          versionNumber  = (MINIXER_VERSION_MAJOR << 16)
                                        | (MINIXER_VERSION_MINOR << 8)
                                        | MINIXER_VERSION_PATCH;
#else
    const int          versionNumber  = 0x40100;
#endif
}
#endif

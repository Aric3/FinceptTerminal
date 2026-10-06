cmake_minimum_required(VERSION 3.21)
project(FinceptCocoaPatch LANGUAGES CXX OBJCXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Gui Widgets)
if(NOT COCOA_SOURCE_DIR)
    message(FATAL_ERROR "Use build-cocoa-patch.sh to provide verified Qt sources")
endif()
set(cocoa "${COCOA_SOURCE_DIR}")
set(files
    main.mm
    qcocoaapplication.h
    qcocoaapplication.mm
    qcocoaapplicationdelegate.h
    qcocoaapplicationdelegate.mm
    qcocoabackingstore.h
    qcocoabackingstore.mm
    qcocoaclipboard.h
    qcocoaclipboard.mm
    qcocoacursor.h
    qcocoacursor.mm
    qcocoadrag.h
    qcocoadrag.mm
    qcocoaeventdispatcher.h
    qcocoaeventdispatcher.mm
    qcocoahelpers.h
    qcocoahelpers.mm
    qcocoainputcontext.h
    qcocoainputcontext.mm
    qcocoaintegration.h
    qcocoaintegration.mm
    qcocoaintrospection.h
    qcocoaintrospection.mm
    qcocoamenu.h
    qcocoamenu.mm
    qcocoamenubar.h
    qcocoamenubar.mm
    qcocoamenuitem.h
    qcocoamenuitem.mm
    qcocoamenuloader.h
    qcocoamenuloader.mm
    qcocoamimetypes.h
    qcocoamimetypes.mm
    qcocoanativeinterface.h
    qcocoanativeinterface.mm
    qcocoansmenu.h
    qcocoansmenu.mm
    qcocoascreen.h
    qcocoascreen.mm
    qcocoaservices.h
    qcocoaservices.mm
    qcocoasystemtrayicon.h
    qcocoasystemtrayicon.mm
    qcocoatheme.h
    qcocoatheme.mm
    qcocoawindow.h
    qcocoawindow.mm
    qcocoawindowmanager.h
    qcocoawindowmanager.mm
    qiosurfacegraphicsbuffer.h
    qiosurfacegraphicsbuffer.mm
    qmacclipboard.h
    qmacclipboard.mm
    qmultitouch_mac.mm
    qmultitouch_mac_p.h
    qnsview.h
    qnsview.mm
    qnswindow.h
    qnswindow.mm
    qnswindowdelegate.h
    qnswindowdelegate.mm
    qcocoacolordialoghelper.h
    qcocoacolordialoghelper.mm
    qcocoafiledialoghelper.h
    qcocoafiledialoghelper.mm
    qcocoafontdialoghelper.h
    qcocoafontdialoghelper.mm
    qcocoamessagedialog.h
    qcocoamessagedialog.mm
    qcocoaglcontext.h
    qcocoaglcontext.mm
    qcocoaaccessibility.h
    qcocoaaccessibility.mm
    qcocoaaccessibilityelement.h
    qcocoaaccessibilityelement.mm
    qcocoasessionmanager.cpp
    qcocoasessionmanager.h
)
list(TRANSFORM files PREPEND "${cocoa}/")
add_library(qcocoa MODULE ${files})
set_target_properties(qcocoa PROPERTIES PREFIX "lib" SUFFIX ".dylib")
target_include_directories(qcocoa PRIVATE "${cocoa}")
target_compile_definitions(qcocoa PRIVATE QT_NO_FOREACH QT_BUILD_PLUGIN)
target_link_libraries(qcocoa PRIVATE Qt6::Core Qt6::CorePrivate Qt6::Gui Qt6::GuiPrivate)
foreach(f AppKit Carbon CoreServices CoreVideo IOKit IOSurface Metal QuartzCore UniformTypeIdentifiers)
    target_link_libraries(qcocoa PRIVATE "-framework ${f}")
endforeach()
qt_add_resources(qcocoa qcocoaresources PREFIX "/qt-project.org/mac/cursors" BASE "${cocoa}" FILES
    "${cocoa}/images/sizeallcursor.png" "${cocoa}/images/spincursor.png" "${cocoa}/images/waitcursor.png")
# Qt 6.8's OpenGL fallback references AGL, removed from current Xcode SDKs.
get_target_property(gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
list(FILTER gl_libs EXCLUDE REGEX "AGL")
set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${gl_libs}")
add_executable(cocoa_smoke accessibility_smoke.mm)
target_link_libraries(cocoa_smoke PRIVATE Qt6::Widgets "-framework AppKit")

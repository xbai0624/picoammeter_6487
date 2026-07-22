cmake_minimum_required(VERSION 3.16)
project(pico6487_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(PICO_MOCK_GPIB "Build with a mock GPIB layer (synthetic data, no hardware/driver needed)" OFF)

# Prefer Qt6, fall back to Qt5
find_package(Qt6 COMPONENTS Widgets SerialPort QUIET)
if(Qt6_FOUND)
    set(QT_LIBS Qt6::Widgets Qt6::SerialPort)
else()
    find_package(Qt5 5.12 COMPONENTS Widgets SerialPort REQUIRED)
    set(QT_LIBS Qt5::Widgets Qt5::SerialPort)
endif()

add_executable(pico6487_gui
    src/main.cpp
    src/CommInterface.h
    src/GpibInterface.h
    src/GpibInterface.cpp
    src/SerialInterface.h
    src/SerialInterface.cpp
    src/Pico6487Driver.h
    src/Pico6487Driver.cpp
    src/AcquisitionThread.h
    src/AcquisitionThread.cpp
    src/PlotWidget.h
    src/PlotWidget.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

if(WIN32)
    set_target_properties(pico6487_gui PROPERTIES WIN32_EXECUTABLE ON)
endif()

target_link_libraries(pico6487_gui PRIVATE ${QT_LIBS})

if(PICO_MOCK_GPIB)
    target_compile_definitions(pico6487_gui PRIVATE PICO_MOCK_GPIB)
else()
    if(WIN32)
        # NI-488.2: ni4882.lib / ni4882.dll (ANSI 32/64-bit API)
        # NI installs its C support files under Program Files (x86) even on
        # 64-bit Windows; the lib64 directory there holds the 64-bit ni4882.lib.
        set(NI_C_SUPPORT
            "$ENV{ProgramFiles}/National Instruments/Shared/ExternalCompilerSupport/C"
            "C:/Program Files (x86)/National Instruments/Shared/ExternalCompilerSupport/C"
            "C:/Program Files/National Instruments/Shared/ExternalCompilerSupport/C"
        )
        set(NI_LIB_HINTS)
        set(NI_INC_HINTS)
        foreach(dir IN LISTS NI_C_SUPPORT)
            list(APPEND NI_LIB_HINTS "${dir}/lib64/msvc" "${dir}/lib32/msvc")
            list(APPEND NI_INC_HINTS "${dir}/include")
        endforeach()
        find_library(GPIB_LIBRARY NAMES ni4882 gpib-32 PATHS ${NI_LIB_HINTS})
        find_path(GPIB_INCLUDE_DIR NAMES ni4882.h ni488.h PATHS ${NI_INC_HINTS})
    else()
        # linux-gpib: libgpib + gpib/ib.h
        find_library(GPIB_LIBRARY NAMES gpib)
        find_path(GPIB_INCLUDE_DIR NAMES gpib/ib.h)
    endif()

    if(GPIB_LIBRARY AND GPIB_INCLUDE_DIR)
        target_include_directories(pico6487_gui PRIVATE ${GPIB_INCLUDE_DIR})
        target_link_libraries(pico6487_gui PRIVATE ${GPIB_LIBRARY})
        message(STATUS "GPIB support enabled: ${GPIB_LIBRARY}")
    else()
        # RS-232 still works; selecting GPIB in the GUI reports a clear error.
        target_compile_definitions(pico6487_gui PRIVATE PICO_NO_GPIB)
        message(WARNING
            "GPIB library/headers not found - building with RS-232 support only. "
            "Install NI-488.2 (Windows) or linux-gpib (Linux) for GPIB support.")
    endif()
endif()

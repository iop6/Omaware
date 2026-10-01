cmake_minimum_required(VERSION 3.22)
# The one place the version is set; see CHANGELOG.md and "Versions" in README.md.
project(OmaWare VERSION 1.2.0 LANGUAGES C CXX)
# Development builds end in "-dev"; set this to "" for a release.
set(OMAWARE_VERSION_SUFFIX "-dev")
include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Keep the build machine's folder names out of the binaries (file names in messages and debug info).
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=" "-ffile-prefix-map=${CMAKE_BINARY_DIR}/=")
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network Quick Qml QuickControls2 Test Xml)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBVIRT REQUIRED IMPORTED_TARGET libvirt libvirt-qemu)
pkg_check_modules(VNC REQUIRED IMPORTED_TARGET libvncclient>=0.9.15)
find_package(tomlplusplus REQUIRED)
add_library(omaware_core src/backend.cpp src/backend.h src/management.cpp src/checkpoints.cpp src/checkpoints.h src/snapshothistory.cpp src/snapshothistory.h src/configuration.cpp src/configuration.h src/workspace.cpp src/workspace.h src/diagnostics.cpp src/diagnostics.h src/theme.cpp src/theme.h src/console.cpp src/console.h src/domainconfig.cpp src/domainconfig.h src/networkcatalog.cpp src/networkcatalog.h src/containment.cpp src/containment.h src/instance.cpp src/instance.h src/paths.cpp src/paths.h src/isolibrary.cpp src/isolibrary.h src/updater.cpp src/updater.h)
target_link_libraries(omaware_core PUBLIC Qt6::Network Qt6::Quick Qt6::Qml Qt6::Xml PkgConfig::LIBVIRT PkgConfig::VNC tomlplusplus::tomlplusplus)
target_include_directories(omaware_core PUBLIC src)
target_compile_options(omaware_core PRIVATE -Wall -Wextra -Wpedantic)
# The bridge helper runs as root via pkexec, so it lives at a fixed root-owned path regardless of the
# app's install prefix. OmaWare refuses to run it unless it and its directories are root-owned.
set(OMAWARE_HELPER_PATH "/usr/local/libexec/omaware/authorize-bridge" CACHE STRING "Root-owned bridge authorization helper")
# GitHub repository whose releases the built-in updater follows.
set(OMAWARE_UPDATE_REPO "iop6/Omaware" CACHE STRING "GitHub owner/repository for updates")
target_compile_definitions(omaware_core PRIVATE OMAWARE_HELPER_PATH="${OMAWARE_HELPER_PATH}" OMAWARE_UPDATE_REPO="${OMAWARE_UPDATE_REPO}")
qt_add_executable(omaware src/main.cpp)
set(OMAWARE_QML qml/Main.qml qml/ConsoleToolbar.qml qml/AppIcon.qml qml/AppButton.qml qml/StatusBadge.qml qml/MetricCard.qml qml/TrendChart.qml qml/LiveTile.qml qml/UsageBar.qml qml/VcpuHeatmap.qml qml/FleetStats.qml qml/Spark.qml qml/TextMeter.qml qml/MonitorPage.qml qml/LogDrawer.qml qml/CornerBrackets.qml qml/VmRow.qml qml/DetailRow.qml qml/VmDetails.qml qml/NetworkDialog.qml qml/AppField.qml qml/AppSelect.qml qml/EditorDialog.qml qml/NetworksPage.qml qml/NetworkTopology.qml qml/TopologyNode.qml qml/TopologyInspector.qml qml/CreateVmDialog.qml qml/HardwareDialog.qml qml/PendingDialog.qml qml/SnapshotsPage.qml qml/SnapshotGraph.qml qml/CheckpointJobs.qml qml/AppTextArea.qml qml/AppCheckBox.qml qml/AppRadioButton.qml qml/AppMenuItem.qml qml/AppMenu.qml qml/AppDialog.qml qml/SectionHeading.qml qml/AppProgressBar.qml qml/AppScrollBar.qml qml/AppDisclosure.qml qml/ActionPalette.qml qml/ErrorNotice.qml qml/AppBusyIndicator.qml qml/AlreadyRunning.qml qml/IsoShopPage.qml qml/IsoDropZone.qml qml/UpdateDialog.qml)
qt_add_resources(omaware qml PREFIX / FILES ${OMAWARE_QML})
target_link_libraries(omaware PRIVATE omaware_core Qt6::QuickControls2)
target_compile_definitions(omaware PRIVATE OMAWARE_VERSION="${PROJECT_VERSION}${OMAWARE_VERSION_SUFFIX}")
enable_testing()
qt_add_executable(omaware-tests tests/test_core.cpp)
target_link_libraries(omaware-tests PRIVATE omaware_core Qt6::Test)
add_test(NAME adapters COMMAND omaware-tests)
set_tests_properties(adapters PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(vnc-resize-test patches/test-libvncclient-zero-screen.c)
target_link_libraries(vnc-resize-test PRIVATE PkgConfig::VNC)
add_test(NAME vnc-resize-compatibility COMMAND vnc-resize-test)
add_executable(vnc-abi-test tests/test_vnc_abi.c)
target_link_libraries(vnc-abi-test PRIVATE PkgConfig::VNC)
add_test(NAME vnc-client-layout COMMAND vnc-abi-test)
qt_add_executable(omaware-integration tests/integration.cpp)
qt_add_resources(omaware-integration qml_test PREFIX / FILES ${OMAWARE_QML})
target_link_libraries(omaware-integration PRIVATE omaware_core Qt6::Test Qt6::QuickControls2)
# Explicit opt-in: this test creates a disposable, diskless domain in qemu:///session.
# Run manually in the Ubuntu VM; never automatically through ctest.
qt_add_executable(omaware-management-tests tests/test_management.cpp)
qt_add_resources(omaware-management-tests qml_management PREFIX / FILES ${OMAWARE_QML})
target_link_libraries(omaware-management-tests PRIVATE omaware_core Qt6::Test Qt6::QuickControls2)

# ---- Installation (scripts/build.sh --install) ----
# The patched LibVNCClient is private to OmaWare: install it beside the app and find it through RPATH.
option(OMAWARE_BUNDLE_VNC "Install the private LibVNCClient next to OmaWare" OFF)
if(OMAWARE_BUNDLE_VNC)
    set_target_properties(omaware PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/omaware")
    install(DIRECTORY "${VNC_LIBRARY_DIRS}/" DESTINATION "${CMAKE_INSTALL_LIBDIR}/omaware" COMPONENT app
        FILES_MATCHING PATTERN "libvncclient.so*" PATTERN "pkgconfig" EXCLUDE PATTERN "cmake" EXCLUDE)
endif()
install(TARGETS omaware RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT app)
install(FILES packaging/omaware.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT app)
# Opt-in, administrator-only: sudo cmake --install <build> --component helper
get_filename_component(OMAWARE_HELPER_DIR "${OMAWARE_HELPER_PATH}" DIRECTORY)
get_filename_component(OMAWARE_HELPER_NAME "${OMAWARE_HELPER_PATH}" NAME)
install(PROGRAMS scripts/authorize-bridge.py DESTINATION "${OMAWARE_HELPER_DIR}" RENAME "${OMAWARE_HELPER_NAME}" COMPONENT helper EXCLUDE_FROM_ALL)

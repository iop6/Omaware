cmake_minimum_required(VERSION 3.22)
# The one place the version is set; see CHANGELOG.md and "Versions" in README.md.
project(OmaWare VERSION 1.11.0 LANGUAGES C CXX)
# Development builds end in "-dev"; set this to "" for a release.
set(OMAWARE_VERSION_SUFFIX "-dev")

include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# Keep the build machine's folder names out of the binaries (file names in messages and debug info).
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=" "-ffile-prefix-map=${CMAKE_BINARY_DIR}/=")

find_package(Qt6 6.4 REQUIRED COMPONENTS Core DBus Gui Network Quick Qml QuickControls2 Test Xml)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(LIBVIRT REQUIRED IMPORTED_TARGET libvirt libvirt-qemu)
pkg_check_modules(VNC REQUIRED IMPORTED_TARGET libvncclient>=0.9.15)
find_package(tomlplusplus REQUIRED)
find_package(ZLIB REQUIRED)
# crypt_r makes the password hashes cloud-init sets up new VMs with.
find_library(CRYPT_LIBRARY crypt REQUIRED)

# The bridge helper runs as root via pkexec, so it lives at a fixed root-owned path regardless of the
# app's install prefix. OmaWare refuses to run it unless it and its directories are root-owned.
set(OMAWARE_HELPER_PATH "/usr/local/libexec/omaware/authorize-bridge" CACHE STRING "Root-owned bridge authorization helper")
# GitHub repository whose releases the built-in updater follows.
set(OMAWARE_UPDATE_REPO "iop6/Omaware" CACHE STRING "GitHub owner/repository for updates")

# ---- Everything but main(), shared by the app and the tests ----
add_library(omaware_core
    src/agentbridge.cpp src/agentbridge.h
    src/agentmanagement.cpp
    src/agentprovision.cpp src/agentprovision.h
    src/agentprovisionbridge.cpp
    src/agentreply.h
    src/agenttransfer.cpp src/agenttransfer.h
    src/applianceimport.cpp src/applianceimport.h
    src/backend.cpp src/backend.h
    src/bridgehelper.cpp src/bridgehelper.h
    src/checkpoints.cpp src/checkpoints.h
    src/cloudimages.cpp src/cloudimages.h
    src/cloudseed.cpp src/cloudseed.h
    src/configuration.cpp src/configuration.h
    src/console.cpp src/console.h
    src/containment.cpp src/containment.h
    src/diagnostics.cpp src/diagnostics.h
    src/domainconfig.cpp src/domainconfig.h
    src/guestinput.cpp src/guestinput.h
    src/hostnetworks.cpp
    src/instance.cpp src/instance.h
    src/isolibrary.cpp src/isolibrary.h
    src/labfile.cpp src/labfile.h
    src/labplan.cpp src/labplan.h
    src/labs.cpp src/labs.h
    src/logins.cpp src/logins.h
    src/management.cpp src/management.h
    src/mcpserver.cpp src/mcpserver.h
    src/networkcatalog.cpp src/networkcatalog.h
    src/paths.cpp src/paths.h
    src/snapshothistory.cpp src/snapshothistory.h
    src/snapshotoperations.cpp
    src/theme.cpp src/theme.h
    src/unattended.cpp src/unattended.h
    src/updater.cpp src/updater.h
    src/virtutil.cpp src/virtutil.h
    src/vmcreation.cpp
    src/vmfiles.cpp src/vmfiles.h
    src/vmoperations.cpp
    src/workspace.cpp src/workspace.h
)
target_include_directories(omaware_core PUBLIC src)
target_link_libraries(omaware_core PUBLIC
    Qt6::DBus Qt6::Network Qt6::Quick Qt6::Qml Qt6::Xml
    PkgConfig::ARCHIVE PkgConfig::LIBVIRT PkgConfig::VNC tomlplusplus::tomlplusplus ZLIB::ZLIB ${CRYPT_LIBRARY})
target_compile_options(omaware_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(omaware_core PRIVATE
    OMAWARE_HELPER_PATH="${OMAWARE_HELPER_PATH}" OMAWARE_UPDATE_REPO="${OMAWARE_UPDATE_REPO}")

# ---- The app ----
set(OMAWARE_QML
    qml/Main.qml
    # Shared controls
    qml/AppBusyIndicator.qml qml/AppButton.qml qml/AppCheckBox.qml qml/AppDialog.qml qml/AppDisclosure.qml
    qml/AppField.qml qml/AppIcon.qml qml/AppMenu.qml qml/AppMenuItem.qml qml/AppProgressBar.qml
    qml/AppRadioButton.qml qml/AppScrollBar.qml qml/AppSelect.qml qml/AppTextArea.qml
    qml/CornerBrackets.qml qml/DetailRow.qml qml/ErrorNotice.qml qml/HelpButton.qml qml/SectionHeading.qml
    qml/StatusBadge.qml qml/TextMeter.qml
    # The window's dialogs, menus and banners
    qml/ActionPalette.qml qml/ActivityDialog.qml qml/AgentBanner.qml qml/AgentQuestionDialog.qml
    qml/AlreadyRunning.qml qml/AppearanceInfo.qml qml/ConfirmDialog.qml qml/DeleteVmDialog.qml qml/ExitDialog.qml
    qml/HelpCenter.qml qml/KeyboardMap.qml qml/LabDialog.qml qml/LogDrawer.qml qml/ThemePicker.qml
    qml/UpdateDialog.qml qml/VmMenu.qml
    # VMs: the list, console, details, snapshots and creation
    qml/CheckpointJobs.qml qml/ConsoleToolbar.qml qml/CreateVmDialog.qml qml/EditorDialog.qml
    qml/HardwareDialog.qml qml/PendingDialog.qml qml/SnapshotGraph.qml qml/SnapshotsPage.qml qml/VmDetails.qml
    qml/VmRow.qml
    # Monitor
    qml/FleetStats.qml qml/LiveTile.qml qml/MetricCard.qml qml/MonitorPage.qml qml/Spark.qml qml/TrendChart.qml
    qml/UsageBar.qml qml/VcpuHeatmap.qml
    # Networks and the network map
    qml/NetworkDialog.qml qml/NetworksPage.qml qml/NetworkTopology.qml qml/TopologyCableMenu.qml
    qml/TopologyCanvasMenu.qml qml/TopologyDropMenu.qml qml/TopologyInspector.qml qml/TopologyNode.qml
    qml/TopologyRemoveDialog.qml qml/TopologySwitchMenu.qml qml/TopologyVmMenu.qml
    # The OS Shop
    qml/IsoDropZone.qml qml/IsoShopPage.qml qml/ShopCard.qml qml/ShopChip.qml qml/ShopDetails.qml
    # The guides the Help window shows
    docs/USER-GUIDE.md docs/NETWORKS.md
)
qt_add_executable(omaware src/main.cpp)
qt_add_resources(omaware qml PREFIX / FILES ${OMAWARE_QML})
target_link_libraries(omaware PRIVATE omaware_core Qt6::QuickControls2)
target_compile_definitions(omaware PRIVATE OMAWARE_VERSION="${PROJECT_VERSION}${OMAWARE_VERSION_SUFFIX}")

# ---- Tests run by ctest: safe anywhere, no VMs ----
enable_testing()
set(OMAWARE_OFFSCREEN "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

qt_add_executable(omaware-tests tests/test_core.cpp)
target_link_libraries(omaware-tests PRIVATE omaware_core Qt6::Test)
add_test(NAME adapters COMMAND omaware-tests)
set_tests_properties(adapters PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(omaware-appliance-tests tests/test_applianceimport.cpp)
target_link_libraries(omaware-appliance-tests PRIVATE omaware_core Qt6::Test)
add_test(NAME appliance-import COMMAND omaware-appliance-tests)
set_tests_properties(appliance-import PROPERTIES TIMEOUT 120)

# These two load the QML from the source tree.
qt_add_executable(omaware-appliance-ui-tests tests/test_applianceui.cpp)
target_link_libraries(omaware-appliance-ui-tests PRIVATE omaware_core Qt6::Test Qt6::QuickControls2)
target_compile_definitions(omaware-appliance-ui-tests PRIVATE OMAWARE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME appliance-ui COMMAND omaware-appliance-ui-tests)
set_tests_properties(appliance-ui PROPERTIES TIMEOUT 30 ENVIRONMENT "${OMAWARE_OFFSCREEN}")

qt_add_executable(omaware-topology-tests tests/test_topology.cpp)
target_link_libraries(omaware-topology-tests PRIVATE omaware_core Qt6::Test Qt6::QuickControls2)
target_compile_definitions(omaware-topology-tests PRIVATE OMAWARE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME network-map COMMAND omaware-topology-tests)
set_tests_properties(network-map PROPERTIES TIMEOUT 60 ENVIRONMENT "${OMAWARE_OFFSCREEN}")

qt_add_executable(omaware-agenttools-tests tests/test_agenttools.cpp)
target_link_libraries(omaware-agenttools-tests PRIVATE omaware_core Qt6::Test)
add_test(NAME agent-tools COMMAND omaware-agenttools-tests)

qt_add_executable(omaware-transfer-io-tests tests/test_agenttransfer_io.cpp)
target_link_libraries(omaware-transfer-io-tests PRIVATE omaware_core)
add_test(NAME agent-transfer-io COMMAND omaware-transfer-io-tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME mcp-stdio COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp_stdio.py $<TARGET_FILE:omaware>)
set_tests_properties(agent-transfer-io mcp-stdio PROPERTIES TIMEOUT 45)

# The patched LibVNCClient: the resize fixes (see docs/VNC-COMPATIBILITY.md) and its struct layout.
add_executable(vnc-resize-test patches/test-libvncclient-zero-screen.c)
target_link_libraries(vnc-resize-test PRIVATE PkgConfig::VNC)
add_test(NAME vnc-resize-compatibility COMMAND vnc-resize-test)

add_executable(vnc-abi-test tests/test_vnc_abi.c)
target_link_libraries(vnc-abi-test PRIVATE PkgConfig::VNC)
add_test(NAME vnc-client-layout COMMAND vnc-abi-test)

# ---- VM test suites: they create and change real VMs in qemu:///session ----
# Explicit opt-in only (OMAWARE_VM_TEST, see docs/DEVELOPMENT.md); run them by hand on a disposable
# machine, never through ctest.
qt_add_executable(omaware-integration tests/integration.cpp)
qt_add_resources(omaware-integration qml_test PREFIX / FILES ${OMAWARE_QML})
target_link_libraries(omaware-integration PRIVATE omaware_core Qt6::Test Qt6::QuickControls2)

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
install(PROGRAMS scripts/authorize-bridge.py DESTINATION "${OMAWARE_HELPER_DIR}" RENAME "${OMAWARE_HELPER_NAME}"
    COMPONENT helper EXCLUDE_FROM_ALL)
# A copy beside the built app, as in packages, so OmaWare can show the exact command that installs it
# without cmake: sudo install -D -o root -g root -m 0755 <build>/authorize-bridge <helper path>.
configure_file(scripts/authorize-bridge.py "${CMAKE_CURRENT_BINARY_DIR}/authorize-bridge" COPYONLY
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

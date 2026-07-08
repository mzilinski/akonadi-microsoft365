# SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Akonadi Microsoft Graph resource. Structured to be dropped into kdepim-runtime's
# resources/ (add `add_subdirectory(graph)` in resources/CMakeLists.txt), or built
# out-of-tree against installed KDE PIM / Akonadi dev packages.

cmake_minimum_required(VERSION 3.16)

# ---- standalone (out-of-tree) bootstrap -------------------------------------
# When built inside kdepim-runtime all of this is already set up by the top-level
# CMakeLists; only run it when this directory is the top-level source dir.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(akonadi-graph-resource VERSION 0.1.0)

    set(KF_MIN_VERSION "6.0.0")
    find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
    set(CMAKE_MODULE_PATH
        ${ECM_MODULE_PATH}
        ${CMAKE_MODULE_PATH}
    )

    include(KDEInstallDirs)
    include(KDECMakeSettings)
    include(KDECompilerSettings NO_POLICY_SCOPE)
    include(ECMQtDeclareLoggingCategory)

    find_package(
        Qt6
        REQUIRED
        COMPONENTS
            Core
            DBus
            Gui
            Network
            NetworkAuth
            Test
            Widgets
    )
    find_package(KF6Config ${KF_MIN_VERSION} CONFIG REQUIRED)
    find_package(KF6CoreAddons ${KF_MIN_VERSION} CONFIG REQUIRED)
    find_package(KF6I18n ${KF_MIN_VERSION} CONFIG REQUIRED)
    find_package(KF6Mime CONFIG REQUIRED)
    find_package(KF6CalendarCore CONFIG REQUIRED)
    find_package(KF6Contacts CONFIG REQUIRED)
    find_package(KPim6Akonadi CONFIG REQUIRED)
    find_package(KPim6AkonadiMime CONFIG REQUIRED)
    find_package(Qt6Keychain CONFIG REQUIRED)
endif()

# KMail requires separate send/receive resources.
set(HAVE_SEPARATE_MTA_RESOURCE TRUE)

add_subdirectory(graphclient)

add_definitions(-DTRANSLATION_DOMAIN=\"akonadi_graph_resource\")

ecm_qt_declare_logging_category(graph_common_SRCS
    HEADER graph_debug.h
    IDENTIFIER GRAPH_LOG
    CATEGORY_NAME org.kde.pim.graph
    DESCRIPTION "graph resource (kdepim-runtime)"
    EXPORT KDEPIMRUNTIME
)

# ---- receiving resource ----------------------------------------------------
set(graphresource_SRCS
    graphresource.cpp
    graphsyncstateattribute.cpp
    mail/graphmailhandler.cpp
    calendar/grapheventhandler.cpp
    contact/graphcontacthandler.cpp
    jobs/graphbatchjob.cpp
    jobs/graphfetchfoldersjob.cpp
    jobs/graphfetchitemsjob.cpp
    jobs/graphfetchitempayloadjob.cpp
    jobs/graphfetchpimitemsjob.cpp
    graphresource.h
    graphsyncstateattribute.h
    mail/graphmailhandler.h
    calendar/grapheventhandler.h
    contact/graphcontacthandler.h
    jobs/graphbatchjob.h
    jobs/graphfetchfoldersjob.h
    jobs/graphfetchitemsjob.h
    jobs/graphfetchitempayloadjob.h
    jobs/graphfetchpimitemsjob.h
    ${graph_common_SRCS}
)

kconfig_add_kcfg_files(graphresource_SRCS graphsettingsbase.kcfgc)

# D-Bus interface: the MTA agent forwards outgoing MIME via sendMessage()/messageSent().
qt_generate_dbus_interface(${CMAKE_CURRENT_SOURCE_DIR}/graphresource.h
    org.kde.Akonadi.Graph.Resource.xml
    OPTIONS -a
)
qt_add_dbus_adaptor(graphresource_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/org.kde.Akonadi.Graph.Resource.xml
    graphresource.h GraphResource graphresourceadaptor GraphResourceAdaptor
)

add_executable(akonadi_graph_resource ${graphresource_SRCS})
target_include_directories(
    akonadi_graph_resource
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(
    akonadi_graph_resource
    KPim6::AkonadiAgentWidgetBase
    KPim6::AkonadiCore
    KPim6::AkonadiMime
    KF6::Mime
    KF6::CalendarCore
    KF6::Contacts
    KF6::I18n
    KF6::ConfigCore
    KF6::ConfigGui
    Qt::DBus
    graphclient
)

# ---- transport (send) resource ---------------------------------------------
if(HAVE_SEPARATE_MTA_RESOURCE)
    set(graphmtaresource_SRCS
        graphmtaresource.cpp
        graphmtaresource.h
        ${graph_common_SRCS}
    )
    qt_add_dbus_interface(graphmtaresource_SRCS
        ${CMAKE_CURRENT_BINARY_DIR}/org.kde.Akonadi.Graph.Resource.xml
        graphresourceinterface
    )
    add_executable(akonadi_graphmta_resource ${graphmtaresource_SRCS})
    target_include_directories(
        akonadi_graphmta_resource
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(
        akonadi_graphmta_resource
        KPim6::AkonadiAgentWidgetBase
        KPim6::AkonadiCore
        KF6::Mime
        KF6::I18n
        Qt::DBus
        graphclient
    )
endif()

# ---- config plugin (loaded by the client's agent configuration dialog) ------
kcoreaddons_add_plugin(graphconfig
    INSTALL_NAMESPACE "pim6/akonadi/config"
)
target_sources(graphconfig PRIVATE graphconfig.cpp)
target_link_libraries(
    graphconfig
    PUBLIC
        KPim6::AkonadiCore
        KF6::ConfigCore
        KF6::I18n
        Qt::Widgets
)

# ---- smoke test (standalone builds only, not installed) ---------------------
# End-to-end check of the client layer + sync jobs against a real tenant;
# run with a bearer token in GRAPH_ACCESS_TOKEN.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(
        graphsmoketest
        tools/graphsmoketest.cpp
        graphsyncstateattribute.cpp
        jobs/graphbatchjob.cpp
        jobs/graphfetchfoldersjob.cpp
        jobs/graphfetchitemsjob.cpp
        jobs/graphfetchitempayloadjob.cpp
        ${graph_common_SRCS}
    )
    target_include_directories(
        graphsmoketest
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(
        graphsmoketest
        KPim6::AkonadiCore
        KPim6::AkonadiMime
        KF6::Mime
        KF6::I18n
        graphclient
    )

    add_executable(akonadifetchtest tools/akonadifetchtest.cpp)
    target_link_libraries(
        akonadifetchtest
        KPim6::AkonadiCore
        KF6::Mime
        Qt::Core
    )

    add_executable(sendtest tools/sendtest.cpp)
    target_link_libraries(
        sendtest
        KF6::Mime
        Qt::Core
        Qt::DBus
    )

    add_executable(specialcheck tools/specialcheck.cpp)
    target_link_libraries(
        specialcheck
        KPim6::AkonadiCore
        KPim6::AkonadiMime
        Qt::Core
    )

    add_executable(
        phase4check
        tools/phase4check.cpp
        calendar/grapheventhandler.cpp
        contact/graphcontacthandler.cpp
        jobs/graphfetchpimitemsjob.cpp
    )
    target_link_libraries(
        phase4check
        KPim6::AkonadiCore
        KF6::CalendarCore
        KF6::Contacts
        Qt::Core
        graphclient
    )

    add_executable(
        writetest
        tools/writetest.cpp
        calendar/grapheventhandler.cpp
        contact/graphcontacthandler.cpp
    )
    target_link_libraries(
        writetest
        KPim6::AkonadiCore
        KF6::CalendarCore
        KF6::Contacts
        Qt::Core
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/graphresource.desktop DESTINATION "${KDE_INSTALL_DATAROOTDIR}/akonadi/agents")
install(
    TARGETS
        akonadi_graph_resource
        ${KDE_INSTALL_TARGETS_DEFAULT_ARGS}
)

if(HAVE_SEPARATE_MTA_RESOURCE)
    install(
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/graphmtaresource.desktop
        DESTINATION "${KDE_INSTALL_DATAROOTDIR}/akonadi/agents"
    )
    install(
        TARGETS
            akonadi_graphmta_resource
            ${KDE_INSTALL_TARGETS_DEFAULT_ARGS}
    )
endif()

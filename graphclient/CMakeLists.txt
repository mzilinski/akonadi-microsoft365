# SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
# SPDX-License-Identifier: LGPL-2.0-or-later
# Graph protocol/client layer (REST/JSON + OAuth2). Built as a static lib, linked by
# both the receiving resource and the MTA resource — mirrors ewsclient.

set(graphclient_SRCS
    graphclient.cpp
    graphrequest.cpp
    auth/graphoauth.cpp
    graphclient.h
    graphrequest.h
    auth/graphoauth.h
)

add_library(graphclient STATIC ${graphclient_SRCS})

target_link_libraries(
    graphclient
    PUBLIC
        Qt::Core
        Qt::Gui # QDesktopServices (browser login)
        Qt::Network
        Qt::NetworkAuth # QOAuth2AuthorizationCodeFlow
        KPim6::AkonadiCore
        KF6::Mime
        Qt6Keychain::Qt6Keychain
)
target_include_directories(graphclient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_property(
    TARGET
        graphclient
    PROPERTY
        POSITION_INDEPENDENT_CODE
            ON
)

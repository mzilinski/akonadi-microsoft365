/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Whether a change the server did not take is sent again, and when. Kept apart from
    GraphResource so that the decision can be tested without Akonadi.
*/

#pragma once

#include <algorithm>

namespace GraphRetryPolicy
{
/// Attempts for a change whose failures include uncertain ones. Changes that provably
/// did not reach the server get many more (about two hours), as they wait for the
/// network or the service to come back — but not forever, since a held-back change
/// stops every sync and item retrieval behind it.
constexpr int kMaxAttempts = 5;
constexpr int kMaxNotExecutedAttempts = 24;
constexpr int kFirstDelaySeconds = 30;
constexpr int kMaxDelaySeconds = 300;

struct Decision {
    bool retry = false;
    int delaySeconds = 0;
};

/// @p notExecuted, @p uncertain and @p permanent count the failed calls of a change by
/// GraphRequest::Failure; @p applied tells that part of it already took effect, and
/// @p attempt how often it has been held back so far.
[[nodiscard]] inline Decision decide(bool repeatable, bool applied, int notExecuted, int uncertain, int permanent, int attempt)
{
    // Repeatable: sending everything again is harmless, so any temporary failure is worth
    // another attempt, and calls that fail for good just fail again. Otherwise only a
    // change of which provably nothing reached the server may be sent again.
    const bool eligible = repeatable ? notExecuted + uncertain > 0 : !applied && notExecuted > 0 && uncertain == 0 && permanent == 0;
    if (!eligible) {
        return {};
    }
    // Nothing reached the server (offline, throttled, no token): keep the change while
    // that lasts. Anything else gets a few attempts.
    const bool onlyNotExecuted = uncertain == 0 && permanent == 0;
    if (attempt >= (onlyNotExecuted ? kMaxNotExecutedAttempts : kMaxAttempts)) {
        return {};
    }
    const int shift = std::min(attempt, 4);
    return {true, std::min(kFirstDelaySeconds << shift, kMaxDelaySeconds)};
}
}

/* This file is part of OpenTTD, distributed under the GNU GPL version 2. */
/** @file test_network_revision.cpp Local simulation builds must not masquerade as one revision. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../network/core/network_game_info.h"
#include "../network/core/config.h"
#include "../rev.h"
#include "../safeguards.h"

extern const char _openttd_revision_simulation_hash[];

TEST_CASE("Network revision - local simulation changes require matching source identity")
{
	const std::string revision(GetNetworkRevisionString());
	CHECK(revision.size() < NETWORK_REVISION_LENGTH);
	CHECK(IsNetworkCompatibleVersion(revision));
	std::string different = revision;
	different.back() = different.back() == '0' ? '1' : '0';
	CHECK_FALSE(IsNetworkCompatibleVersion(different));
	if (_openttd_revision_simulation_hash[0] != '\0') {
		/* Previous local builds used the commit hash with the same '-m'. */
		const std::string old = "old-build-m" + std::string(_openttd_revision_hash).substr(0, 10);
		CHECK_FALSE(IsNetworkCompatibleVersion(old));
	}
}

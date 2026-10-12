// WalkerCore — GameIdentity SDK-free API tests.
//
// These tests pin the architectural rule "no SCS SDK types in domain
// headers": GameIdentity must be constructible, comparable, and usable
// without ever including scssdk.h. The FromFields constructor takes POD
// fields and the test exercises the equality operator and major/minor
// extraction.

#include "TestHarness.h"
#include "walkercore/GameVersion.h"

namespace walkercore {

void TestGameIdentityFromFields() {
    // ETS2 1.61.1.0 — version encoded as (1 << 16) | 13 = 0x1000D.
    const GameIdentity ets2 =
        GameIdentity::FromFields("eut2", (1u << 16) | 13u);
    EXPECT_STREQ(ets2.game_id, "eut2");
    EXPECT_TRUE(ets2.is_ets2);
    EXPECT_EQ(ets2.major, static_cast<unsigned short>(1));
    EXPECT_EQ(ets2.minor, static_cast<unsigned short>(13));
    EXPECT_EQ(ets2.game_version, static_cast<uint32_t>((1u << 16) | 13u));
}

void TestGameIdentityFromFieldsAts() {
    // ATS 1.61.3 — version encoded as (1 << 16) | 15 = 0x1000F.
    const GameIdentity ats =
        GameIdentity::FromFields("ats", (1u << 16) | 15u);
    EXPECT_STREQ(ats.game_id, "ats");
    EXPECT_FALSE(ats.is_ets2);
    EXPECT_EQ(ats.major, static_cast<unsigned short>(1));
    EXPECT_EQ(ats.minor, static_cast<unsigned short>(15));
}

void TestGameIdentityFromFieldsNullGameId() {
    // Null game_id is coerced to empty string by the constructor so the
    // invariant "game_id is never null when constructed via FromFields"
    // holds even when the SDK hands us a null pointer.
    const GameIdentity none = GameIdentity::FromFields(nullptr, 0u);
    EXPECT_STREQ(none.game_id, "");
    EXPECT_FALSE(none.is_ets2);
    EXPECT_EQ(none.major, static_cast<unsigned short>(0));
    EXPECT_EQ(none.minor, static_cast<unsigned short>(0));
}

void TestGameIdentityEquality() {
    const GameIdentity a = GameIdentity::FromFields("eut2", (1u << 16) | 13u);
    const GameIdentity b = GameIdentity::FromFields("eut2", (1u << 16) | 13u);
    const GameIdentity c = GameIdentity::FromFields("eut2", (1u << 16) | 14u);
    const GameIdentity d = GameIdentity::FromFields("ats",  (1u << 16) | 13u);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_FALSE(a == c);  // minor differs
    EXPECT_FALSE(a == d);  // game_id differs
    EXPECT_TRUE(a != c);
    EXPECT_TRUE(a != d);
}

void TestGameIdentityMajorMinorHighBits() {
    // SCS-encoded versions where the major half is non-zero. We extract
    // the cached major/minor by bitshift, not by SCS_GET_*. This test
    // catches a regression if someone reintroduces an SDK dependency in
    // the .cpp without re-checking the encoding.
    const uint32_t v = (42u << 16) | 7u;
    const GameIdentity id = GameIdentity::FromFields("eut2", v);
    EXPECT_EQ(id.major, static_cast<unsigned short>(42));
    EXPECT_EQ(id.minor, static_cast<unsigned short>(7));
}

}  // namespace walkercore
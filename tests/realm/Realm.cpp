#include <realm/Realm.hpp>

#include <gtest/gtest.h>

using namespace Realm;

TEST(Realm, initializesStoppedWithSafeDefaults) {
    CRealm realm(42, "codex");

    EXPECT_EQ(realm.id(), 42);
    EXPECT_EQ(realm.name(), "codex");
    EXPECT_EQ(realm.state(), eRealmState::STOPPED);
    EXPECT_EQ(realm.inputOwner(), eRealmInputOwner::NONE);
    EXPECT_EQ(realm.compositorPID(), 0);
    EXPECT_EQ(realm.exitCode(), -1);
    EXPECT_TRUE(realm.runtimeDirectory().empty());
    EXPECT_TRUE(realm.waylandSocket().empty());
    EXPECT_TRUE(realm.configPath().empty());
    EXPECT_TRUE(realm.logPath().empty());
}

TEST(Realm, inputOwnerNamesAreStable) {
    EXPECT_EQ(realmInputOwnerName(eRealmInputOwner::AGENT), "agent");
    EXPECT_EQ(realmInputOwnerName(eRealmInputOwner::HUMAN), "human");
    EXPECT_EQ(realmInputOwnerName(eRealmInputOwner::NONE), "none");
}

TEST(Realm, stateNamesAreStable) {
    EXPECT_EQ(realmStateName(eRealmState::CREATING), "creating");
    EXPECT_EQ(realmStateName(eRealmState::RUNNING), "running");
    EXPECT_EQ(realmStateName(eRealmState::PAUSED), "paused");
    EXPECT_EQ(realmStateName(eRealmState::STOPPING), "stopping");
    EXPECT_EQ(realmStateName(eRealmState::STOPPED), "stopped");
    EXPECT_EQ(realmStateName(eRealmState::FAILED), "failed");
}

TEST(Realm, acceptsNormalLifecycle) {
    CRealm realm(1, "research");

    EXPECT_TRUE(realm.transitionTo(eRealmState::CREATING));
    EXPECT_TRUE(realm.transitionTo(eRealmState::RUNNING));
    EXPECT_TRUE(realm.transitionTo(eRealmState::PAUSED));
    EXPECT_TRUE(realm.transitionTo(eRealmState::RUNNING));
    EXPECT_TRUE(realm.transitionTo(eRealmState::STOPPING));
    EXPECT_TRUE(realm.transitionTo(eRealmState::STOPPED));
}

TEST(Realm, acceptsFailureAndRestart) {
    CRealm realm(1, "research");

    EXPECT_TRUE(realm.transitionTo(eRealmState::CREATING));
    EXPECT_TRUE(realm.transitionTo(eRealmState::FAILED));
    EXPECT_TRUE(realm.transitionTo(eRealmState::CREATING));
    EXPECT_TRUE(realm.transitionTo(eRealmState::RUNNING));
    EXPECT_TRUE(realm.transitionTo(eRealmState::FAILED));
}

TEST(Realm, rejectsInvalidTransitionsWithoutChangingState) {
    CRealm     realm(1, "research");

    const auto transition = realm.transitionTo(eRealmState::PAUSED);

    ASSERT_FALSE(transition);
    EXPECT_NE(transition.error().find("stopped"), std::string::npos);
    EXPECT_NE(transition.error().find("paused"), std::string::npos);
    EXPECT_EQ(realm.state(), eRealmState::STOPPED);
}

TEST(Realm, rejectsDuplicateTransition) {
    CRealm     realm(1, "research");

    const auto transition = realm.transitionTo(eRealmState::STOPPED);

    ASSERT_FALSE(transition);
    EXPECT_NE(transition.error().find("already stopped"), std::string::npos);
}

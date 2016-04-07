/**
 * @file
 *
 * @brief
 *
 * @copyright BSD License (see doc/COPYING or http://www.libelektra.org)
 */

#include <kdbthread.hpp>

#include <kdbprivate.h>

#include <gtest/gtest.h>

using namespace kdb;

struct test_contextual_update : ::testing::Test
{
	test_contextual_update ()
	: ks (), gc (), c (gc), i (ks, c, Key ("/ignore/id", KEY_META, "default", "my", KEY_END)),
	  x (ks, c, Key ("/%id%/key", KEY_META, "default", "33", KEY_END)){};

	KeySet ks;
	Coordinator gc;
	ThreadContext c;
	ThreadValue<std::string> i;
	ThreadValue<int> x;
};

TEST_F (test_contextual_update, activate)
{
	ASSERT_EQ (x.getName (), "/%/key");
	ASSERT_TRUE (ks.lookup ("/%/key"));
	c.template activate (i);
	ASSERT_EQ (x.getName (), "/my/key");
	ASSERT_TRUE (ks.lookup ("/my/key"));
}

TEST_F (test_contextual_update, changeKey)
{
	ks.append (Key ("/other/key", KEY_VALUE, "88", KEY_END));
	i = "other";
	c.template activate (i);
	ASSERT_EQ (x.getName (), "/other/key");
	ASSERT_TRUE (ks.lookup ("/other/key"));
	ASSERT_EQ (x, 88);
	ASSERT_EQ (ks.lookup ("/other/key").getString (), "88");

	ks.lookup ("/other/key").setString ("100");

	c.syncLayers ();
	ASSERT_EQ (x, 88) << "should not influence cache";
	ASSERT_EQ (x.getName (), "/other/key");
	ASSERT_EQ (ks.lookup ("/other/key").getString (), "100");

	x.syncCache ();
	ASSERT_EQ (x.getName (), "/other/key");
	ASSERT_EQ (ks.lookup ("/other/key").getString (), "100");
	ASSERT_EQ (x, 100) << "cache should be updated";
}

TEST_F (test_contextual_update, syncCache)
{
	ks.append (Key ("/%/key", KEY_VALUE, "111", KEY_END));

	x.syncCache ();
	ASSERT_EQ (x.getName (), "/%/key");
	ASSERT_EQ (ks.lookup ("/%/key").getString (), "111");
	ASSERT_EQ (x, 111) << "reevaluated context, should have found new key";
}

TEST_F (test_contextual_update, notifyAllEvents)
{

	ks.append (Key ("/%/key", KEY_VALUE, "133", KEY_END));

	c.notifyAllEvents ();
	ASSERT_EQ (x.getName (), "/%/key");
	ASSERT_EQ (x, 33) << "should not be changed (optimization)";
	ASSERT_EQ (ks.lookup ("/%/key").getString (), "133") << "nothing done, so its not changed";
}

TEST_F (test_contextual_update, notifyAllEventsChange)
{
	ks.append (Key ("/other/key", KEY_VALUE, "133", KEY_END));

	i = "other";
	c.template activate (i);
	ASSERT_EQ (x.getName (), "/other/key");
	ASSERT_EQ (x, 133);
	ASSERT_EQ (ks.lookup ("/other/key").getString (), "133") << "nothing done, so its not changed";
}

TEST_F (test_contextual_update, notifyKeySetUpdate)
{
	ks.append (Key ("/%/key", KEY_VALUE, "144", KEY_END));

	c.notifyKeySetUpdate ();
	ASSERT_EQ (x.getName (), "/%/key");
	ASSERT_EQ (x, 144) << "reevaluated context, should have found new key";
	ASSERT_EQ (ks.lookup ("/%/key").getString (), "144");
}
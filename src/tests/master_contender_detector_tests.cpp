/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <zookeeper.h>

#include <gmock/gmock.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "master/contender.hpp"
#include "master/detector.hpp"
#include "master/master.hpp"

#include "messages/messages.hpp"

#include "slave/slave.hpp"

#include "tests/isolator.hpp"
#include "tests/mesos.hpp"
#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using namespace zookeeper;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;
using process::UPID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;


class MasterContenderDetectorTest : public MesosTest {};


TEST_F(MasterContenderDetectorTest, File)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Write "master" to a file and use the "file://" mechanism to
  // create a master detector for the slave. Still requires a master
  // detector for the master first.
  slave::Flags flags = CreateSlaveFlags();

  const string& path = path::join(flags.work_dir, "master");
  ASSERT_SOME(os::write(path, stringify(master.get())));

  Try<MasterDetector*> detector =
    MasterDetector::create("file://" + path);

  ASSERT_SOME(detector);

  StartSlave(detector.get(), flags);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  driver.stop();
  driver.join();

  Shutdown();
}


TEST(BasicMasterContenderDetectorTest, Contender)
{
  PID<Master> master;
  master.ip = 10000000;
  master.port = 10000;

  MasterContender* contender = new StandaloneMasterContender();

  contender->initialize(master);

  Future<Future<Nothing> > contended = contender->contend();
  AWAIT_READY(contended);

  Future<Nothing> lostCandidacy = contended.get();

  // The candidacy is never lost.
  EXPECT_TRUE(lostCandidacy.isPending());

  delete contender;

  // Deleting the contender also withdraws the previous candidacy.
  AWAIT_READY(lostCandidacy);
}


TEST(BasicMasterContenderDetectorTest, Detector)
{
  PID<Master> master;
  master.ip = 10000000;
  master.port = 10000;

  StandaloneMasterDetector detector;

  Future<Result<UPID> > detected = detector.detect();

  // No one has appointed the leader so we are pending.
  EXPECT_TRUE(detected.isPending());

  detector.appoint(master);

  AWAIT_READY(detected);
}


#ifdef MESOS_HAS_JAVA
class ZooKeeperMasterContenderDetectorTest : public ZooKeeperTest {};


// A single contender gets elected automatically.
TEST_F(ZooKeeperMasterContenderDetectorTest, MasterContender)
{
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  Owned<zookeeper::Group> group(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));

  ZooKeeperMasterContender* contender = new ZooKeeperMasterContender(group);

  PID<Master> master;
  master.ip = 10000000;
  master.port = 10000;

  contender->initialize(master);
  Future<Future<Nothing> > contended = contender->contend();
  AWAIT_READY(contended);

  ZooKeeperMasterDetector detector(url.get());

  Future<Result<UPID> > leader = detector.detect();
  EXPECT_SOME_EQ(master, leader.get());
  Future<Nothing> lostCandidacy = contended.get();
  leader = detector.detect(leader.get());

  Future<Option<int64_t> > sessionId = group.get()->session();
  AWAIT_READY(sessionId);
  server->expireSession(sessionId.get().get());

  // Session expiration causes candidacy to be lost and the
  // Future<Nothing> to be fulfilled.
  AWAIT_READY(lostCandidacy);
  AWAIT_READY(leader);
  EXPECT_NONE(leader.get());
}


// Two contenders, the first wins. Kill the first, then the second
// is elected.
TEST_F(ZooKeeperMasterContenderDetectorTest, MasterContenders)
{
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  ZooKeeperMasterContender* contender1 =
    new ZooKeeperMasterContender(url.get());

  PID<Master> master1;
  master1.ip = 10000000;
  master1.port = 10000;

  contender1->initialize(master1);

  Future<Future<Nothing> > contended1 = contender1->contend();
  AWAIT_READY(contended1);

  ZooKeeperMasterDetector detector1(url.get());

  Future<Result<UPID> > leader1 = detector1.detect();
  AWAIT_READY(leader1);
  EXPECT_SOME_EQ(master1, leader1.get());

  ZooKeeperMasterContender contender2(url.get());

  PID<Master> master2;
  master2.ip = 10000001;
  master2.port = 10001;

  contender2.initialize(master2);

  Future<Future<Nothing> > contended2 = contender2.contend();
  AWAIT_READY(contended2);

  ZooKeeperMasterDetector detector2(url.get());
  Future<Result<UPID> > leader2 = detector2.detect();
  AWAIT_READY(leader2);
  EXPECT_SOME_EQ(master1, leader2.get());

  LOG(INFO) << "Killing the leading master";

  // Destroying detector1 (below) causes leadership change.
  delete contender1;

  Future<Result<UPID> > leader3 = detector2.detect(master1);
  AWAIT_READY(leader3);
  EXPECT_SOME_EQ(master2, leader3.get());
}


// Master contention and detection fail when the network is down, it
// recovers when the network is back up.
TEST_F(ZooKeeperMasterContenderDetectorTest, ContenderDetectorShutdownNetwork)
{
  Clock::pause();

  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  ZooKeeperMasterContender contender(url.get());

  PID<Master> master;
  master.ip = 10000000;
  master.port = 10000;

  contender.initialize(master);

  Future<Future<Nothing> > contended = contender.contend();
  AWAIT_READY(contended);
  Future<Nothing> lostCandidacy = contended.get();

  ZooKeeperMasterDetector detector(url.get());

  Future<Result<UPID> > leader = detector.detect();
  EXPECT_SOME_EQ(master, leader.get());

  leader = detector.detect(leader.get());

  // Shut down ZooKeeper and expect things to fail after the timeout.
  server->shutdownNetwork();

  Clock::advance(std::max(
      MASTER_DETECTOR_ZK_SESSION_TIMEOUT,
      MASTER_CONTENDER_ZK_SESSION_TIMEOUT));
  Clock::settle();

  AWAIT_EXPECT_FAILED(lostCandidacy);
  AWAIT_READY(leader);
  EXPECT_ERROR(leader.get());

  // Retry.
  contended = contender.contend();
  leader = detector.detect(leader.get());

  // Things will not change until the contender reconnects.
  Clock::advance(Minutes(1));
  Clock::settle();
  EXPECT_TRUE(contended.isPending());
  EXPECT_TRUE(leader.isPending());

  server->startNetwork();
  AWAIT_READY(contended);
  AWAIT_READY(leader);
}


// Tests that detectors and contenders fail when we reach our
// ZooKeeper session timeout. This is to enforce that we manually
// expire the session when we do not get reconnected within the
// timeout.
TEST_F(ZooKeeperMasterContenderDetectorTest, MasterDetectorTimedoutSession)
{
  // Use an arbitrary timeout value.
  Duration sessionTimeout(Seconds(5));

  Try<zookeeper::URL> url = zookeeper::URL::parse(
        "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  Owned<zookeeper::Group> leaderGroup(new Group(url.get(), sessionTimeout));

  // First we bring up three master contender/detector:
  //   1. A leading contender.
  //   2. A non-leading contender.
  //   3. A non-contender (detector).

  // 1. Simulate a leading contender.
  ZooKeeperMasterContender leaderContender(leaderGroup);

  PID<Master> leader;
  leader.ip = 10000000;
  leader.port = 10000;

  leaderContender.initialize(leader);

  Future<Future<Nothing> > contended = leaderContender.contend();
  AWAIT_READY(contended);

  ZooKeeperMasterDetector leaderDetector(leaderGroup);

  Future<Result<UPID> > detected = leaderDetector.detect();
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // 2. Simulate a non-leading contender.
  Owned<zookeeper::Group> followerGroup(new Group(url.get(), sessionTimeout));
  ZooKeeperMasterContender followerContender(followerGroup);

  PID<Master> follower;
  follower.ip = 10000001;
  follower.port = 10001;

  followerContender.initialize(follower);

  contended = followerContender.contend();
  AWAIT_READY(contended);

  ZooKeeperMasterDetector followerDetector(followerGroup);

  detected = followerDetector.detect();
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // 3. Simulate a non-contender.
  Owned<zookeeper::Group> nonContenderGroup(
      new Group(url.get(), sessionTimeout));
  ZooKeeperMasterDetector nonContenderDetector(nonContenderGroup);

  detected = nonContenderDetector.detect();

  EXPECT_SOME_EQ(leader, detected.get());

  // Expecting the reconnecting event after we shut down the ZK.
  Future<Nothing> leaderReconnecting = FUTURE_DISPATCH(
      leaderGroup->process->self(),
      &GroupProcess::reconnecting);

  Future<Nothing> followerReconnecting = FUTURE_DISPATCH(
      followerGroup->process->self(),
      &GroupProcess::reconnecting);

  Future<Nothing> nonContenderReconnecting = FUTURE_DISPATCH(
      nonContenderGroup->process->self(),
      &GroupProcess::reconnecting);

  server->shutdownNetwork();

  AWAIT_READY(leaderReconnecting);
  AWAIT_READY(followerReconnecting);
  AWAIT_READY(nonContenderReconnecting);

  // Now the detectors re-detect.
  Future<Result<UPID> > leaderNoMasterDetected = leaderDetector.detect(leader);
  Future<Result<UPID> > followerNoMasterDetected =
    followerDetector.detect(leader);
  Future<Result<UPID> > nonContenderNoMasterDetected =
    nonContenderDetector.detect(leader);

  Clock::pause();

  // We may need to advance multiple times because we could have
  // advanced the clock before the timer in Group starts.
  while (leaderNoMasterDetected.isPending() ||
         followerNoMasterDetected.isPending() ||
         nonContenderNoMasterDetected.isPending()) {
    Clock::advance(sessionTimeout);
    Clock::settle();
  }

  AWAIT_READY(leaderNoMasterDetected);
  EXPECT_ERROR(leaderNoMasterDetected.get());
  AWAIT_READY(followerNoMasterDetected);
  EXPECT_ERROR(followerNoMasterDetected.get());
  AWAIT_READY(nonContenderNoMasterDetected);
  EXPECT_ERROR(nonContenderNoMasterDetected.get());

  Clock::resume();
}


// Tests whether a leading master correctly detects a new master when
// its ZooKeeper session is expired (the follower becomes the new
// leader).
TEST_F(ZooKeeperMasterContenderDetectorTest,
       MasterDetectorExpireMasterZKSession)
{
  // Simulate a leading master.
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  PID<Master> leader;
  leader.ip = 10000000;
  leader.port = 10000;

  // Create the group instance so we can expire its session.
  Owned<zookeeper::Group> group(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));

  ZooKeeperMasterContender leaderContender(group);

  leaderContender.initialize(leader);

  Future<Future<Nothing> > leaderContended = leaderContender.contend();
  AWAIT_READY(leaderContended);

  Future<Nothing> leaderLostLeadership = leaderContended.get();

  ZooKeeperMasterDetector leaderDetector(url.get());

  Future<Result<UPID> > detected = leaderDetector.detect();
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // Keep detecting.
  Future<Result<UPID> > newLeaderDetected =
    leaderDetector.detect(detected.get());

  // Simulate a following master.
  PID<Master> follower;
  follower.ip = 10000001;
  follower.port = 10001;

  ZooKeeperMasterDetector followerDetector(url.get());
  ZooKeeperMasterContender followerContender(url.get());
  followerContender.initialize(follower);

  Future<Future<Nothing> > followerContended = followerContender.contend();
  AWAIT_READY(followerContended);

  LOG(INFO) << "The follower now is detecting the leader";
  detected = followerDetector.detect(None());
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // Now expire the leader's zk session.
  Future<Option<int64_t> > session = group->session();
  AWAIT_READY(session);
  EXPECT_SOME(session.get());

  LOG(INFO) << "Now expire the ZK session: " << std::hex << session.get().get();

  server->expireSession(session.get().get());

  AWAIT_READY(leaderLostLeadership);

  // Wait for session expiration and ensure the former leader detects
  // a new leader.
  AWAIT_READY(newLeaderDetected);
  EXPECT_SOME(newLeaderDetected.get());
  EXPECT_EQ(follower, newLeaderDetected.get().get());
}


// Tests whether a slave correctly DOES NOT disconnect from the
// master when its ZooKeeper session is expired, but the master still
// stays the leader when the slave re-connects with the ZooKeeper.
TEST_F(ZooKeeperMasterContenderDetectorTest, MasterDetectorExpireSlaveZKSession)
{
  // Simulate a leading master.
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  PID<Master> master;
  master.ip = 10000000;
  master.port = 10000;

  ZooKeeperMasterContender masterContender(url.get());
  masterContender.initialize(master);

  Future<Future<Nothing> > leaderContended = masterContender.contend();
  AWAIT_READY(leaderContended);

  // Simulate a slave.
  Owned<zookeeper::Group> group(
        new Group(url.get(), MASTER_DETECTOR_ZK_SESSION_TIMEOUT));

  ZooKeeperMasterDetector slaveDetector(group);

  Future<Result<UPID> > detected = slaveDetector.detect();
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(master, detected.get());

  detected = slaveDetector.detect(master);

  // Now expire the slave's zk session.
  Future<Option<int64_t> > session = group->session();
  AWAIT_READY(session);

  Future<Nothing> connected = FUTURE_DISPATCH(
      group->process->self(),
      &GroupProcess::connected);

  server->expireSession(session.get().get());

  // When connected is called, the leader has already expired and
  // reconnected.
  AWAIT_READY(connected);

  // Still pending because there is no leader change.
  EXPECT_TRUE(detected.isPending());
}


// Tests whether a slave correctly detects the new master when its
// ZooKeeper session is expired and a new master is elected before the
// slave reconnects with ZooKeeper.
TEST_F(ZooKeeperMasterContenderDetectorTest,
       MasterDetectorExpireSlaveZKSessionNewMaster)
{
  Try<zookeeper::URL> url = zookeeper::URL::parse(
        "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  // Simulate a leading master.
  Owned<zookeeper::Group> leaderGroup(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));

  // 1. Simulate a leading contender.
  ZooKeeperMasterContender leaderContender(leaderGroup);
  ZooKeeperMasterDetector leaderDetector(leaderGroup);

  PID<Master> leader;
  leader.ip = 10000000;
  leader.port = 10000;

  leaderContender.initialize(leader);

  Future<Future<Nothing> > contended = leaderContender.contend();
  AWAIT_READY(contended);

  Future<Result<UPID> > detected = leaderDetector.detect(None());
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // 2. Simulate a non-leading contender.
  Owned<zookeeper::Group> followerGroup(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));
  ZooKeeperMasterContender followerContender(followerGroup);
  ZooKeeperMasterDetector followerDetector(followerGroup);

  PID<Master> follower;
  follower.ip = 10000001;
  follower.port = 10001;

  followerContender.initialize(follower);

  contended = followerContender.contend();
  AWAIT_READY(contended);

  detected = followerDetector.detect(None());
  EXPECT_SOME_EQ(leader, detected.get());

  // 3. Simulate a non-contender.
  Owned<zookeeper::Group> nonContenderGroup(
      new Group(url.get(), MASTER_DETECTOR_ZK_SESSION_TIMEOUT));
  ZooKeeperMasterDetector nonContenderDetector(nonContenderGroup);

  detected = nonContenderDetector.detect();

  EXPECT_SOME_EQ(leader, detected.get());

  detected = nonContenderDetector.detect(leader);

  // Now expire the slave's and leading master's zk sessions.
  // NOTE: Here we assume that slave stays disconnected from the ZK
  // when the leading master loses its session.
  Future<Option<int64_t> > slaveSession = nonContenderGroup->session();
  AWAIT_READY(slaveSession);

  Future<Option<int64_t> > masterSession = leaderGroup->session();
  AWAIT_READY(masterSession);

  server->expireSession(slaveSession.get().get());
  server->expireSession(masterSession.get().get());

  // Wait for session expiration and ensure a new master is detected.
  AWAIT_READY(detected);

  EXPECT_SOME_EQ(follower, detected.get());
}

#endif // MESOS_HAS_JAVA

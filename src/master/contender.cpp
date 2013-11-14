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

#include <process/defer.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/lambda.hpp>

#include "master/contender.hpp"
#include "master/master.hpp"

#include "zookeeper/contender.hpp"
#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"
#include "zookeeper/url.hpp"

using std::string;

using namespace process;
using namespace zookeeper;

namespace mesos {
namespace internal {

using namespace master;

const Duration MASTER_CONTENDER_ZK_SESSION_TIMEOUT = Seconds(10);


class ZooKeeperMasterContenderProcess
  : public Process<ZooKeeperMasterContenderProcess>
{
public:
  ZooKeeperMasterContenderProcess(const zookeeper::URL& url);
  ZooKeeperMasterContenderProcess(Owned<zookeeper::Group> group);
  ~ZooKeeperMasterContenderProcess();

  void initialize(const PID<Master>& master);

  // MasterContender implementation.
  virtual Future<Future<Nothing> > contend();

private:
  Owned<zookeeper::Group> group;
  LeaderContender* contender;
  PID<Master> master;
};


Try<MasterContender*> MasterContender::create(const string& zk)
{
  if (zk == "") {
    return new StandaloneMasterContender();
  } else if (strings::startsWith(zk, "zk://")) {
    Try<zookeeper::URL> url = URL::parse(zk);
    if (url.isError()) {
      return Try<MasterContender*>::error(url.error());
    }
    if (url.get().path == "/") {
      return Try<MasterContender*>::error(
          "Expecting a (chroot) path for ZooKeeper ('/' is not supported)");
    }
    return new ZooKeeperMasterContender(url.get());
  } else if (strings::startsWith(zk, "file://")) {
    const string& path = zk.substr(7);
    const Try<string> read = os::read(path);
    if (read.isError()) {
      return Error("Failed to read from file at '" + path + "'");
    }

    return create(strings::trim(read.get()));
  }

  return Try<MasterContender*>::error("Failed to parse '" + zk + "'");
}


MasterContender::~MasterContender() {}


StandaloneMasterContender::~StandaloneMasterContender()
{
  if (promise != NULL) {
    promise->set(Nothing()); // Leadership lost.
    delete promise;
  }
}


void StandaloneMasterContender::initialize(
    const PID<master::Master>& master)
{
  // We don't really need to store the master in this basic
  // implementation so we just restore an 'initialized' flag to make
  // sure it is called.
  initialized = true;
}

Future<Future<Nothing> > StandaloneMasterContender::contend()
{
  CHECK(initialized) << "Initialize the contender first";

  if (promise != NULL) {
    LOG(INFO) << "Withdrawing the previous membership before recontending";
    promise->set(Nothing());
    delete promise;
  }

  // Directly return a future that is always pending because it
  // represents a membership/leadership that is not going to be lost
  // until we 'withdraw'.
  promise = new Promise<Nothing>();
  return promise->future();
}


ZooKeeperMasterContender::ZooKeeperMasterContender(const URL& url)
{
  process = new ZooKeeperMasterContenderProcess(url);
  spawn(process);
}


ZooKeeperMasterContender::ZooKeeperMasterContender(Owned<Group> group)
{
  process = new ZooKeeperMasterContenderProcess(group);
  spawn(process);
}


ZooKeeperMasterContender::~ZooKeeperMasterContender()
{
  terminate(process);
  process::wait(process);
  delete process;
}


void ZooKeeperMasterContender::initialize(
    const PID<master::Master>& master)
{
  process->initialize(master);
}


Future<Future<Nothing> > ZooKeeperMasterContender::contend()
{
  return dispatch(process, &ZooKeeperMasterContenderProcess::contend);
}


ZooKeeperMasterContenderProcess::ZooKeeperMasterContenderProcess(
    const URL& url)
  : group(new Group(url, MASTER_CONTENDER_ZK_SESSION_TIMEOUT)),
    contender(NULL) {}


ZooKeeperMasterContenderProcess::ZooKeeperMasterContenderProcess(
    Owned<Group> _group)
  : group(_group),
    contender(NULL) {}


ZooKeeperMasterContenderProcess::~ZooKeeperMasterContenderProcess()
{
  delete contender;
}

void ZooKeeperMasterContenderProcess::initialize(
    const PID<Master>& _master)
{
  master = _master;
}


Future<Future<Nothing> > ZooKeeperMasterContenderProcess::contend()
{
  CHECK(master) << "Initialize the contender first";

  if (contender != NULL) {
    LOG(INFO) << "Withdrawing the previous membership before recontending";
    delete contender;
  }

  contender = new LeaderContender(group.get(), master);
  return contender->contend();
}

} // namespace internal {
} // namespace mesos {

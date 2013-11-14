#include <set>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>

#include <stout/check.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>

#include "zookeeper/contender.hpp"
#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"

using namespace process;

using std::set;
using std::string;

namespace zookeeper {

class LeaderContenderProcess : public Process<LeaderContenderProcess>
{
public:
  LeaderContenderProcess(Group* group, const std::string& data);
  virtual ~LeaderContenderProcess();

  // LeaderContender implementation.
  Future<Future<Nothing> > contend();
  Future<bool> withdraw();

protected:
  virtual void finalize();

private:
  // Invoked when we have joined the group (or failed to do so).
  void joined();

  // Invoked when the group memberships have changed.
  void watched(const Future<set<Group::Membership> >& memberships);

  // Invoked when the group membership is cancelled.
  void cancelled(const Future<bool>& successful);

  // Helper for setting error and failing pending promises.
  void fail(const string& message);

  // Helper for cancelling the Group membership.
  void cancel();

  Group* group;
  const string data;

  // The contender's state transitions from contending -> watching ->
  // withdrawing or contending -> withdrawing. Each state is
  // identified by the corresponding Option<Promise> being assigned.
  // Note that these Option<Promise>s are never reset to None once it
  // is assigned.

  // Holds the promise for the future for contend().
  Option<Promise<Future<Nothing> >*> contending;

  // Holds the promise for the inner future enclosed by contend()'s
  // result which is satisfied when the contender's candidacy is
  // lost.
  Option<Promise<Nothing>*> watching;

  // Holds the promise for the future for withdraw().
  Option<Promise<bool>*> withdrawing;

  // Stores the result for joined().
  Future<Group::Membership> candidacy;
};


LeaderContenderProcess::LeaderContenderProcess(
    Group* _group,
    const string& _data)
  : group(_group),
    data(_data) {}


LeaderContenderProcess::~LeaderContenderProcess()
{
  if (contending.isSome()) {
    delete contending.get();
    contending = None();
  }

  if (watching.isSome()) {
    delete watching.get();
    watching = None();
  }

  if (withdrawing.isSome()) {
    delete withdrawing.get();
    withdrawing = None();
  }
}


void LeaderContenderProcess::finalize()
{
  // We do not wait for the result here because the Group keeps
  // retrying (even after the contender is destroyed) until it
  // either succeeds or its session times out. In either case the
  // old membership is eventually cancelled.
  // There is a tricky situation where the contender terminates after
  // it has contended but before it is notified of the obtained
  // membership. In this case the membership is not cancelled during
  // contender destruction. The client thus should use withdraw() to
  // wait for the membership to be first obtained and then cancelled.
  cancel();
}


Future<Future<Nothing> > LeaderContenderProcess::contend()
{
  CHECK(contending.isNone()) << "Cannot contend more than once";

  LOG(INFO) << "Joining the ZK group with data: '" << data << "'";
  candidacy = group->join(data);
  candidacy
    .onAny(defer(self(), &Self::joined));

  // Okay, we wait and see what unfolds.
  contending = new Promise<Future<Nothing> >();
  return contending.get()->future();
}


Future<bool> LeaderContenderProcess::withdraw()
{
  CHECK_SOME(contending)
    << "Can only withdraw after the contender has contended";

  if (withdrawing.isSome()) {
    // Repeated calls to withdraw get the same result.
    return withdrawing.get();
  }

  withdrawing = new Promise<bool>();

  if (candidacy.isPending()) {
    // If we have not obtained the candidacy yet, we withdraw after
    // it is obtained.
    LOG(INFO) << "Withdraw requested before the candidacy is obtained; will "
              << "withdraw after it happens";
    candidacy.onAny(defer(self(), &Self::cancel));
  } else if (candidacy.isReady()) {
    cancel();
  } else {
    CHECK(candidacy.isFailed()) << "Not expecting candidacy to be discarded";

    // We have failed to obtain the candidacy so we do not need to
    // cancel it.
    return false;
  }

  return withdrawing.get()->future();
}


void LeaderContenderProcess::cancel()
{
  if (!candidacy.isReady()) {
    // Nothing to cancel.
    return;
  }

  LOG(INFO) << "Now cancelling the membership: " << candidacy.get().id();

  group->cancel(candidacy.get())
    .onAny(defer(self(), &Self::cancelled, lambda::_1));
}


void LeaderContenderProcess::cancelled(const Future<bool>& successful)
{
  CHECK(candidacy.isReady());
  LOG(INFO) << "Membership cancelled: " << candidacy.get().id();

  CHECK_SOME(withdrawing);
  withdrawing.get()->set(successful);
}


void LeaderContenderProcess::joined()
{
  if (candidacy.isFailed()) {
    fail(candidacy.failure());
    return;
  }

  CHECK(candidacy.isReady()) << "Not expecting Group to discard the future";

  if (withdrawing.isSome()) {
    LOG(INFO) << "Joined group after the contender started withdrawing";
    return;
  }

  LOG(INFO) << "New candidate (id='" << candidacy.get().id() << "', data='"
            << data << "') has entered the contest for leadership";

  // Transition to 'watching' state.
  CHECK(watching.isNone());
  watching = new Promise<Nothing>();

  // Notify the client.
  CHECK(contending.isSome());
  if (contending.get()->set(watching.get()->future())) {
    // Continue to watch that our membership is not removed (if the
    // client still cares about it).
    group->watch()
      .onAny(defer(self(), &Self::watched, lambda::_1));
  }
}


void LeaderContenderProcess::watched(
    const Future<set<Group::Membership> >& memberships)
{
  CHECK_SOME(contending);
  CHECK(contending.get()->future().isReady())
    << "'Contending' must be ready before 'watching'";

  if (withdrawing.isSome()) {
    LOG(INFO)
      << "Group memberships changed after the contender started withdrawing";
    return;
  }

  // Fail all operations.
  if (memberships.isFailed()) {
    fail(memberships.failure());
    return;
  }

  CHECK(memberships.isReady()) << "Not expecting Group to discard the future";

  CHECK_SOME(watching);
  CHECK(candidacy.isReady());

  if (memberships.get().count(candidacy.get()) == 0) {
    // We had joined the group but our membership is gone.
    LOG(INFO) << "Lost candidacy: " << candidacy.get().id();
    watching.get()->set(Nothing());
  } else {
    // Continue to watch that our membership is not removed.
    group->watch(memberships.get())
      .onAny(defer(self(), &Self::watched, lambda::_1));
  }
}


void LeaderContenderProcess::fail(const string& message)
{
  if (contending.isSome()) {
    contending.get()->fail(message);
  }

  if (watching.isSome()) {
    watching.get()->fail(message);
  }

  if (withdrawing.isSome()) {
    withdrawing.get()->fail(message);
  }
}


LeaderContender::LeaderContender(Group* group, const string& data)
{
  process = new LeaderContenderProcess(group, data);
  spawn(process);
}


LeaderContender::~LeaderContender()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Future<Nothing> > LeaderContender::contend()
{
  return dispatch(process, &LeaderContenderProcess::contend);
}


Future<bool> LeaderContender::withdraw()
{
  return dispatch(process, &LeaderContenderProcess::withdraw);
}

} // namespace zookeeper {

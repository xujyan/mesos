#ifndef __ZOOKEEPER_GROUP_HPP__
#define __ZOOKEEPER_GROUP_HPP__

#include <map>
#include <set>

#include "process/future.hpp"
#include "process/timer.hpp"

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

#include "zookeeper/authentication.hpp"
#include "zookeeper/url.hpp"

// Forward declarations.
class Watcher;
class ZooKeeper;

namespace zookeeper {

// Forward declaration.
class GroupProcess;

// Represents a distributed group managed by ZooKeeper. A group is
// associated with a specific ZooKeeper path, and members are
// represented by ephemeral sequential nodes.
class Group
{
public:
  // Represents a group membership. Note that we order memberships by
  // membership id (that is, an older membership is ordered before a
  // younger membership). In addition, we do not use the "cancelled"
  // future to compare memberships so that two memberships created
  // from different Group instances will still be considered the same.
  struct Membership
  {
    bool operator == (const Membership& that) const
    {
      return sequence == that.sequence;
    }

    bool operator != (const Membership& that) const
    {
      return sequence != that.sequence;
    }

    bool operator < (const Membership& that) const
    {
      return sequence < that.sequence;
    }

    bool operator <= (const Membership& that) const
    {
      return sequence <= that.sequence;
    }

    bool operator > (const Membership& that) const
    {
      return sequence > that.sequence;
    }

    bool operator >= (const Membership& that) const
    {
      return sequence >= that.sequence;
    }

    uint64_t id() const
    {
      return sequence;
    }

    // Returns a future that is only satisfied once this membership
    // has been cancelled. In which case, the value of the future is
    // true if you own this membership and cancelled it by invoking
    // Group::cancel. Otherwise, the value of the future is false (and
    // could signify cancellation due to a sesssion expiration or
    // operator error).
    process::Future<bool> cancelled() const
    {
      return cancelled_;
    }

  private:
    friend class GroupProcess; // Creates and manages memberships.

    Membership(uint64_t _sequence, const process::Future<bool>& cancelled)
      : sequence(_sequence), cancelled_(cancelled) {}

    uint64_t sequence;
    process::Future<bool> cancelled_;
  };

  // Constructs this group using the specified ZooKeeper servers (list
  // of host:port) with the given timeout at the specified znode.
  Group(const std::string& servers,
        const Duration& timeout,
        const std::string& znode,
        const Option<Authentication>& auth = None());
  Group(const URL& url,
        const Duration& timeout);

  ~Group();

  // Returns the result of trying to join a "group" in ZooKeeper. If
  // succesful, an "owned" membership will be returned whose
  // retrievable data will be a copy of the specified parameter. A
  // membership is not "renewed" in the event of a ZooKeeper session
  // expiration. Instead, a client should watch the group memberships
  // and rejoin the group as appropriate.
  process::Future<Membership> join(const std::string& data);

  // Returns the result of trying to cancel a membership. Note that
  // only memberships that are "owned" (see join) can be canceled.
  process::Future<bool> cancel(const Membership& membership);

  // Returns the result of trying to fetch the data associated with a
  // group membership.
  process::Future<std::string> data(const Membership& membership);

  // Returns a future that gets set when the group memberships differ
  // from the "expected" memberships specified.
  process::Future<std::set<Membership> > watch(
      const std::set<Membership>& expected = std::set<Membership>());

  // Returns the current ZooKeeper session associated with this group,
  // or none if no session currently exists.
  process::Future<Option<int64_t> > session();

  // Made public for testing purposes.
  GroupProcess* process;
};


class GroupProcess : public process::Process<GroupProcess>
{
public:
  GroupProcess(const std::string& servers,
               const Duration& timeout,
               const std::string& znode,
               const Option<Authentication>& auth);

  GroupProcess(const URL& url,
               const Duration& timeout);

  virtual ~GroupProcess();

  virtual void initialize();

  static const Duration RETRY_INTERVAL;

  // Group implementation.
  process::Future<Group::Membership> join(const std::string& data);
  process::Future<bool> cancel(const Group::Membership& membership);
  process::Future<std::string> data(const Group::Membership& membership);
  process::Future<std::set<Group::Membership> > watch(
      const std::set<Group::Membership>& expected);
  process::Future<Option<int64_t> > session();

  // ZooKeeper events.
  void connected(bool reconnect);
  void reconnecting();
  void expired();
  void updated(const std::string& path);
  void created(const std::string& path);
  void deleted(const std::string& path);

private:
  Result<Group::Membership> doJoin(const std::string& data);
  Result<bool> doCancel(const Group::Membership& membership);
  Result<std::string> doData(const Group::Membership& membership);

  // Attempts to cache the current set of memberships.
  bool cache();

  // Updates any pending watches.
  void update();

  // Synchronizes pending operations with ZooKeeper and also attempts
  // to cache the current set of memberships if necessary.
  bool sync();

  // Generic retry method. This mechanism is "generic" in the sense
  // that it is not specific to any particular operation, but rather
  // attempts to perform all pending operations (including caching
  // memberships if necessary).
  void retry(const Duration& duration);

  // Fails all pending operations.
  void abort();

  void timedout(const int64_t& sessionId);

  Option<std::string> error; // Potential non-retryable error.

  const std::string servers;
  const Duration timeout;
  const std::string znode;

  Option<Authentication> auth; // ZooKeeper authentication.

  const ACL_vector acl; // Default ACL to use.

  Watcher* watcher;
  ZooKeeper* zk;

  enum State { // ZooKeeper connection state.
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
  } state;

  struct Join
  {
    Join(const std::string& _data) : data(_data) {}
    std::string data;
    process::Promise<Group::Membership> promise;
  };

  struct Cancel
  {
    Cancel(const Group::Membership& _membership)
      : membership(_membership) {}
    Group::Membership membership;
    process::Promise<bool> promise;
  };

  struct Data
  {
    Data(const Group::Membership& _membership)
      : membership(_membership) {}
    Group::Membership membership;
    process::Promise<std::string> promise;
  };

  struct Watch
  {
    Watch(const std::set<Group::Membership>& _expected)
      : expected(_expected) {}
    std::set<Group::Membership> expected;
    process::Promise<std::set<Group::Membership> > promise;
  };

  struct {
    std::queue<Join*> joins;
    std::queue<Cancel*> cancels;
    std::queue<Data*> datas;
    std::queue<Watch*> watches;
  } pending;

  bool retrying;

  // Expected ZooKeeper sequence numbers (either owned/created by this
  // group instance or not) and the promise we associate with their
  // "cancellation" (i.e., no longer part of the group).
  std::map<uint64_t, process::Promise<bool>*> owned;
  std::map<uint64_t, process::Promise<bool>*> unowned;

  // Cache of owned + unowned, where 'None' represents an invalid
  // cache and 'Some' represents a valid cache.
  Option<std::set<Group::Membership> > memberships;

  // The timer that determines whether we should quit waiting for the
  // connection to be restored.
  Option<process::Timer> timer;
};

} // namespace zookeeper {

#endif // __ZOOKEEPER_GROUP_HPP__

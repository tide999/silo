#ifndef _NDB_TXN_H_
#define _NDB_TXN_H_

#include <malloc.h>
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>

#include <map>
#include <iostream>
#include <vector>
#include <string>
#include <utility>
#include <stdexcept>
#include <limits>
#include <type_traits>
#include <tuple>

#include <unordered_map>

#include "amd64.h"
#include "btree.h"
#include "core.h"
#include "counter.h"
#include "macros.h"
#include "varkey.h"
#include "util.h"
#include "static_assert.h"
#include "rcu.h"
#include "thread.h"
#include "spinlock.h"
#include "small_unordered_map.h"
#include "static_unordered_map.h"
#include "static_vector.h"
#include "prefetch.h"
#include "keyrange.h"
#include "tuple.h"
#include "scopedperf.hh"
#include "marked_ptr.h"
#include "ndb_type_traits.h"

// forward decl
template <template <typename, typename> class Transaction, typename P>
  class base_txn_btree;

class transaction_unusable_exception {};
class transaction_read_only_exception {};

// XXX: hacky
extern std::string (*g_proto_version_str)(uint64_t v);

template <typename T, bool enable>
struct static_container {
  inline ALWAYS_INLINE void
  assign(const T &t)
  {
  }
  inline ALWAYS_INLINE T *
  get()
  {
    return nullptr;
  }
  inline ALWAYS_INLINE const T *
  get() const
  {
    return nullptr;
  }
};

// T must have no-arg ctor and be assignable
template <typename T>
struct static_container<T, true> {
  inline void
  assign(const T &t)
  {
    this->t = t;
  }
  inline T *
  get()
  {
    return &t;
  }
  inline const T *
  get() const
  {
    return &t;
  }
private:
  T t;
};

// base class with very simple definitions- nothing too exciting yet
class transaction_base : private util::noncopyable {
  template <template <typename, typename> class T, typename P>
    friend class base_txn_btree;
public:

  typedef dbtuple::tid_t tid_t;
  typedef dbtuple::size_type size_type;
  typedef dbtuple::string_type string_type;

  // TXN_EMBRYO - the transaction object has been allocated but has not
  // done any operations yet
  enum txn_state { TXN_EMBRYO, TXN_ACTIVE, TXN_COMMITED, TXN_ABRT, };

  enum {
    // use the low-level scan protocol for checking scan consistency,
    // instead of keeping track of absent ranges
    TXN_FLAG_LOW_LEVEL_SCAN = 0x1,

    // true to mark a read-only transaction- if a txn marked read-only
    // does a write, a transaction_read_only_exception is thrown and the
    // txn is aborted
    TXN_FLAG_READ_ONLY = 0x2,

    // XXX: more flags in the future, things like consistency levels
  };

#define ABORT_REASONS(x) \
    x(ABORT_REASON_USER) \
    x(ABORT_REASON_UNSTABLE_READ) \
    x(ABORT_REASON_FUTURE_TID_READ) \
    x(ABORT_REASON_NODE_SCAN_WRITE_VERSION_CHANGED) \
    x(ABORT_REASON_NODE_SCAN_READ_VERSION_CHANGED) \
    x(ABORT_REASON_WRITE_NODE_INTERFERENCE) \
    x(ABORT_REASON_INSERT_NODE_INTERFERENCE) \
    x(ABORT_REASON_READ_NODE_INTEREFERENCE) \
    x(ABORT_REASON_READ_ABSENCE_INTEREFERENCE)

  enum abort_reason {
#define ENUM_X(x) x,
    ABORT_REASONS(ENUM_X)
#undef ENUM_X
  };

  static const char *
  AbortReasonStr(abort_reason reason)
  {
    switch (reason) {
#define CASE_X(x) case x: return #x;
    ABORT_REASONS(CASE_X)
#undef CASE_X
    default:
      break;
    }
    ALWAYS_ASSERT(false);
    return 0;
  }

  inline transaction_base(uint64_t flags)
    : state(TXN_EMBRYO), flags(flags) {}

protected:
#define EVENT_COUNTER_DEF_X(x) \
  static event_counter g_ ## x ## _ctr;
  ABORT_REASONS(EVENT_COUNTER_DEF_X)
#undef EVENT_COUNTER_DEF_X

  static event_counter *
  AbortReasonCounter(abort_reason reason)
  {
    switch (reason) {
#define EVENT_COUNTER_CASE_X(x) case x: return &g_ ## x ## _ctr;
    ABORT_REASONS(EVENT_COUNTER_CASE_X)
#undef EVENT_COUNTER_CASE_X
    default:
      break;
    }
    ALWAYS_ASSERT(false);
    return 0;
  }

public:

  // only fires during invariant checking
  inline void
  ensure_active()
  {
    if (state == TXN_EMBRYO)
      state = TXN_ACTIVE;
    INVARIANT(state == TXN_ACTIVE);
  }

  inline uint64_t
  get_flags() const
  {
    return flags;
  }

protected:

  // the read set is a mapping from (tuple -> tid_read).
  // "write_set" is used to indicate if this read tuple
  // also belongs in the write set.
  struct read_record_t {
    constexpr read_record_t() : tuple(), t() {}
    constexpr read_record_t(const dbtuple *tuple, tid_t t)
      : tuple(tuple), t(t) {}
    inline const dbtuple *
    get_tuple() const
    {
      return tuple;
    }
    inline tid_t
    get_tid() const
    {
      return t;
    }
  private:
    const dbtuple *tuple;
    tid_t t;
  };

  friend std::ostream &
  operator<<(std::ostream &o, const read_record_t &r);

  // the write set is a mapping from (tuple -> value_to_write).
  template <typename ValueType, typename ValueInfoType, bool StableInputs>
  struct basic_write_record_t {
    typedef ValueType value_type;
    typedef ValueInfoType value_info_type;
    enum { FLAGS_INSERT = 0x1 };

    basic_write_record_t() = default;
    basic_write_record_t(dbtuple *tuple,
                         const string_type &k,
                         const ValueType *r,
                         typename private_::typeutil<ValueInfoType>::func_param_type vinfo,
                         btree *btr,
                         bool insert)
      : tuple(tuple),
        k(StableInputs ? &k : nullptr),
        r(r),
        vinfo(vinfo),
        btr(btr)
    {
      if (!StableInputs) {
        // optimized away at compile time
        key_container.assign(k);
        if (r)
          value_container.assign(*r);
      }
      this->btr.set_flags(insert ? FLAGS_INSERT : 0);
    }
    inline dbtuple *
    get_tuple()
    {
      return tuple;
    }
    inline const dbtuple *
    get_tuple() const
    {
      return tuple;
    }
    inline bool
    is_insert() const
    {
      // don't need the mask
      return btr.get_flags();
    }
    inline btree *
    get_btree() const
    {
      return btr.get();
    }
    inline const string_type &
    get_key() const
    {
      if (StableInputs)
        return *k;
      else
        return *key_container.get();
    }
    inline const ValueType *
    get_value() const
    {
      if (StableInputs)
        return r;
      else
        return r ? value_container.get() : nullptr;
    }
    inline typename private_::typeutil<ValueInfoType>::func_param_type
    get_value_info() const
    {
      return vinfo;
    }
  private:
    dbtuple *tuple;
    const string_type *k;
    const ValueType *r;
    ValueInfoType vinfo;
    marked_ptr<btree> btr; // first bit for inserted

    // for configurations which don't guarantee stable put strings

    // XXX: un-necessary, because key_writer guarantees us stable string
    static_container<std::string, !StableInputs> key_container;

    // necessary regardless of key_writer
    static_container<ValueType, !StableInputs> value_container;
  };

  template <typename ValueType, typename ValueInfoType, bool StableInputs>
  friend std::ostream &
  operator<<(
      std::ostream &o,
      const basic_write_record_t<ValueType, ValueInfoType, StableInputs> &r);

  // the absent set is a mapping from (btree_node -> version_number).
  struct absent_record_t { uint64_t version; };

  friend std::ostream &
  operator<<(std::ostream &o, const absent_record_t &r);

  struct dbtuple_write_info {
    enum {
      FLAGS_LOCKED = 0x1,
      FLAGS_INSERT = 0x1 << 1,
    };
    inline dbtuple_write_info() {}
    inline dbtuple_write_info(dbtuple *tuple)
      : tuple(tuple)
    {}
    inline dbtuple_write_info(dbtuple *tuple, bool insert)
      : tuple(tuple)
    {
      this->tuple.set_flags(insert ? (FLAGS_LOCKED | FLAGS_INSERT) : 0);
    }
    inline dbtuple *
    get_tuple()
    {
      return tuple.get();
    }
    inline const dbtuple *
    get_tuple() const
    {
      return tuple.get();
    }
    inline ALWAYS_INLINE void
    mark_locked()
    {
      INVARIANT(!is_locked());
      tuple.or_flags(FLAGS_LOCKED);
      INVARIANT(is_locked());
    }
    inline ALWAYS_INLINE bool
    is_locked() const
    {
      return tuple.get_flags() & FLAGS_LOCKED;
    }
    inline ALWAYS_INLINE bool
    is_insert() const
    {
      return tuple.get_flags() & FLAGS_INSERT;
    }
    // for sorting- we want the tuples marked "inserted"
    // to come first in the sort order [this simplifies the
    // programming when acquiring locks and dealing with duplicates]
    inline ALWAYS_INLINE
    bool operator<(const dbtuple_write_info &o) const
    {
      return tuple < o.tuple || (tuple == o.tuple && !is_insert() < !o.is_insert());
    }
    marked_ptr<dbtuple> tuple;
  };

  static event_counter g_evt_read_logical_deleted_node_search;
  static event_counter g_evt_read_logical_deleted_node_scan;
  static event_counter g_evt_dbtuple_write_search_failed;
  static event_counter g_evt_dbtuple_write_insert_failed;

  static event_counter evt_local_search_lookups;
  static event_counter evt_local_search_write_set_hits;
  static event_counter evt_dbtuple_latest_replacement;

  CLASS_STATIC_COUNTER_DECL(scopedperf::tsc_ctr, g_txn_commit_probe0, g_txn_commit_probe0_cg);
  CLASS_STATIC_COUNTER_DECL(scopedperf::tsc_ctr, g_txn_commit_probe1, g_txn_commit_probe1_cg);
  CLASS_STATIC_COUNTER_DECL(scopedperf::tsc_ctr, g_txn_commit_probe2, g_txn_commit_probe2_cg);
  CLASS_STATIC_COUNTER_DECL(scopedperf::tsc_ctr, g_txn_commit_probe3, g_txn_commit_probe3_cg);
  CLASS_STATIC_COUNTER_DECL(scopedperf::tsc_ctr, g_txn_commit_probe4, g_txn_commit_probe4_cg);
  CLASS_STATIC_COUNTER_DECL(scopedperf::tsc_ctr, g_txn_commit_probe5, g_txn_commit_probe5_cg);
  CLASS_STATIC_COUNTER_DECL(scopedperf::tsc_ctr, g_txn_commit_probe6, g_txn_commit_probe6_cg);

  txn_state state;
  abort_reason reason;
  const uint64_t flags;
};

// type specializations
namespace private_ {
  template <>
  struct is_trivially_destructible<transaction_base::read_record_t> {
    static const bool value = true;
  };

  template <typename R, typename VI>
  struct is_trivially_destructible<transaction_base::basic_write_record_t<R, VI, true>> {
    static const bool value = is_trivially_destructible<VI>::value;
  };

  template <typename R, typename VI>
  struct is_trivially_destructible<transaction_base::basic_write_record_t<R, VI, false>> {
    static const bool value =
      is_trivially_destructible<R>::value && is_trivially_destructible<VI>::value;
  };

  template <>
  struct is_trivially_destructible<transaction_base::absent_record_t> {
    static const bool value = true;
  };

  template <>
  struct is_trivially_destructible<transaction_base::dbtuple_write_info> {
    static const bool value = true;
  };
}

inline ALWAYS_INLINE std::ostream &
operator<<(std::ostream &o, const transaction_base::read_record_t &r)
{
  o << "[tuple=" << util::hexify(r.get_tuple())
    << ", tid_read=" << g_proto_version_str(r.get_tid())
    << "]";
  return o;
}

template <typename ValueType, typename ValueInfoType, bool StableInputs>
inline ALWAYS_INLINE std::ostream &
operator<<(
    std::ostream &o,
    const transaction_base::basic_write_record_t<ValueType, ValueInfoType, StableInputs> &r)
{
  o << "[tuple=" << util::hexify(r.get_tuple())
    << ", key=" << util::hexify(r.get_key())
    << ", value=" << util::hexify(r.get_value())
    << ", insert=" << r.is_insert()
    << "]";
  return o;
}

inline ALWAYS_INLINE std::ostream &
operator<<(std::ostream &o, const transaction_base::absent_record_t &r)
{
  o << "[v=" << r.version << "]";
  return o;
}

struct default_transaction_traits {
  static const size_t read_set_expected_size = SMALL_SIZE_MAP;
  static const size_t absent_set_expected_size = EXTRA_SMALL_SIZE_MAP;
  static const size_t write_set_expected_size = SMALL_SIZE_MAP;
  static const bool stable_input_memory = false;
  static const bool hard_expected_sizes = false; // true if the expected sizes are hard maximums
  static const bool read_own_writes = true; // if we read a key which we previous put(), are we guaranteed
                                            // to read our latest (uncommited) values? this comes at a
                                            // performance penality [you should not need this behavior to
                                            // write txns, since you *know* the values you inserted]

  typedef util::default_string_allocator StringAllocator;
};

template <template <typename, typename> class Protocol,
          typename P,
          typename Traits>
class transaction : public transaction_base {
  friend class base_txn_btree<Protocol, P>;
  friend Protocol<P, Traits>;

public:

  // KeyWriter is expected to implement:
  // [1-arg constructor]
  //   KeyWriter(const Key *)
  // [fully materialize]
  //   template <typename StringAllocator>
  //   const std::string * fully_materialize(bool, StringAllocator &)

  // ValueWriter is expected to implement:
  // [1-arg constructor]
  //   ValueWriter(const Value *, ValueInfo)
  // [compute new size from old value]
  //   size_t compute_needed(const uint8_t *, size_t)
  // [fully materialize]
  //   template <typename StringAllocator>
  //   const std::string * fully_materialize(bool, StringAllocator &)
  // [perform write]
  //   void operator()(uint8_t *, size_t)
  //
  // ValueWriter does not have to be move/copy constructable. The value passed
  // into the ValueWriter constructor is guaranteed to be valid throughout the
  // lifetime of a ValueWriter instance.

  // KeyReader Interface
  //
  // KeyReader is a simple transformation from (const std::string &) => const Key &.
  // The input is guaranteed to be stable, so it has a simple interface:
  //
  //   const Key &operator()(const std::string &)
  //
  // The KeyReader is expect to preserve the following property: After a call
  // to operator(), but before the next, the returned value is guaranteed to be
  // valid and remain stable.

  // ValueReader Interface
  //
  // ValueReader is a more complex transformation from (const uint8_t *, size_t) => Value &.
  // The input is not guaranteed to be stable, so it has a more complex interface:
  //
  //   template <typename StringAllocator>
  //   bool operator()(const uint8_t *, size_t, StringAllocator &)
  //
  // This interface returns false if there was not enough buffer space to
  // finish the read, true otherwise.  Note that this interface returning true
  // does NOT mean that a read was stable, but it just means there were enough
  // bytes in the buffer to perform the tentative read.
  //
  // Note that ValueReader also exposes a dup interface
  //
  //   template <typename StringAllocator>
  //   void dup(const Value &, StringAllocator &)
  //
  // ValueReader also exposes a means to fetch results:
  //
  //   Value &results()
  //
  // The ValueReader is expected to preserve the following property: After a
  // call to operator(), if it returns true, then the value returned from
  // results() should remain valid and stable until the next call to
  // operator().

  typedef typename P::Key key_type;
  typedef typename P::Value value_type;
  typedef typename P::ValueInfo value_info_type;

  typedef typename P::KeyWriter key_writer_type;
  typedef typename P::ValueWriter value_writer_type;

  //typedef typename P::KeyReader key_reader_type;
  //typedef typename P::SingleValueReader single_value_reader_type;
  //typedef typename P::ValueReader value_reader_type;

  typedef Traits traits_type;
  typedef typename Traits::StringAllocator string_allocator_type;

protected:
  // data structures

  inline ALWAYS_INLINE Protocol<P, Traits> *
  cast()
  {
    return static_cast<Protocol<P, Traits> *>(this);
  }

  inline ALWAYS_INLINE const Protocol<P, Traits> *
  cast() const
  {
    return static_cast<const Protocol<P, Traits> *>(this);
  }

  // XXX: we have baked in b-tree into the protocol- other indexes are possible
  // but we would need to abstract it away. we don't bother for now.

  typedef
    basic_write_record_t<
      value_type, value_info_type, traits_type::stable_input_memory>
    write_record_t;

#ifdef USE_SMALL_CONTAINER_OPT

  // small types
  typedef small_vector<
    read_record_t,
    traits_type::read_set_expected_size> read_set_map_small;
  typedef small_vector<
    write_record_t,
    traits_type::write_set_expected_size> write_set_map_small;
  typedef small_unordered_map<
    const btree::node_opaque_t *, absent_record_t,
    traits_type::absent_set_expected_size> absent_set_map_small;

  // static types
  typedef static_vector<
    read_record_t,
    traits_type::read_set_expected_size> read_set_map_static;
  typedef static_vector<
    write_record_t,
    traits_type::write_set_expected_size> write_set_map_static;
  typedef static_unordered_map<
    const btree::node_opaque_t *, absent_record_t,
    traits_type::absent_set_expected_size> absent_set_map_static;

  // use static types if the expected sizes are guarantees
  typedef
    typename std::conditional<
      traits_type::hard_expected_sizes,
      read_set_map_static, read_set_map_small>::type read_set_map;
  typedef
    typename std::conditional<
      traits_type::hard_expected_sizes,
      write_set_map_static, write_set_map_small>::type write_set_map;
  typedef
    typename std::conditional<
      traits_type::hard_expected_sizes,
      absent_set_map_static, absent_set_map_small>::type absent_set_map;

#else
  typedef std::vector<read_record_t> read_set_map;
  typedef std::vector<write_record_t> write_set_map;
  typedef std::vector<absent_record_t> absent_set_map;
#endif

  // small type
  typedef
    typename util::vec<
      dbtuple_write_info, traits_type::write_set_expected_size>::type
    dbtuple_write_info_vec_small;

  // static type
  typedef
    static_vector<
      dbtuple_write_info, traits_type::write_set_expected_size>
    dbtuple_write_info_vec_static;

  // chosen type
  typedef
    typename std::conditional<
      traits_type::hard_expected_sizes,
      dbtuple_write_info_vec_static, dbtuple_write_info_vec_small>::type
    dbtuple_write_info_vec;

  static inline bool
  sorted_dbtuples_contains(
      const dbtuple_write_info_vec &dbtuples,
      const dbtuple *tuple)
  {
    // XXX: skip binary search for small-sized dbtuples?
    return std::binary_search(dbtuples.begin(), dbtuples.end(),
        dbtuple_write_info(const_cast<dbtuple *>(tuple)));
  }

public:

  inline transaction(uint64_t flags, string_allocator_type &sa);
  inline ~transaction();

  // returns TRUE on successful commit, FALSE on abort
  // if doThrow, signals success by returning true, and
  // failure by throwing an abort exception
  bool commit(bool doThrow = false);

  // abort() always succeeds
  inline void
  abort()
  {
    abort_impl(ABORT_REASON_USER);
  }

  void dump_debug_info() const;

#ifdef DIE_ON_ABORT
  void
  abort_trap(abort_reason reason)
  {
    AbortReasonCounter(reason)->inc();
    this->reason = reason; // for dump_debug_info() to see
    dump_debug_info();
    ::abort();
  }
#else
  inline ALWAYS_INLINE void
  abort_trap(abort_reason reason)
  {
    AbortReasonCounter(reason)->inc();
  }
#endif

  std::map<std::string, uint64_t> get_txn_counters() const;

  inline bool
  is_read_only() const
  {
    return get_flags() & TXN_FLAG_READ_ONLY;
  }

  // for debugging purposes only
  inline const read_set_map &
  get_read_set() const
  {
    return read_set;
  }

  inline const write_set_map &
  get_write_set() const
  {
    return write_set;
  }

  inline const absent_set_map &
  get_absent_set() const
  {
    return absent_set;
  }

protected:
  inline void abort_impl(abort_reason r);

  // low-level API for txn_btree

  // try to insert a new "tentative" tuple into the underlying
  // btree associated with the given context.
  //
  // if return.first is not null, then this function will
  //   1) mutate the transaction such that the absent_set is aware of any
  //      mutating changes made to the underlying btree.
  //   2) add the new tuple to the write_set
  //
  // if return.second is true, then this txn should abort, because a conflict
  // was detected w/ the absent_set.
  //
  // if return.first is not null, the returned tuple is locked()!
  //
  // if the return.first is null, then this function has no side effects.
  //
  // NOTE: !ret.first => !ret.second
  std::pair< dbtuple *, bool >
  try_insert_new_tuple(
      btree &btr,
      const key_type &key,
      const value_type *value,
      typename private_::typeutil<value_info_type>::func_param_type vinfo);

  // reads the contents of tuple into v
  // within this transaction context
  template <typename ValueReader>
  bool
  do_tuple_read(const dbtuple *tuple, ValueReader &value_reader);

  void
  do_node_read(const btree::node_opaque_t *n, uint64_t version);

public:
  // expected public overrides
  bool can_overwrite_record_tid(tid_t prev, tid_t cur) const;

  /**
   * XXX: document
   */
  std::pair<bool, tid_t> consistent_snapshot_tid() const;

  tid_t null_entry_tid() const;

  inline string_allocator_type &
  string_allocator()
  {
    return *sa;
  }

protected:
  // expected protected overrides

  /**
   * create a new, unique TID for a txn. at the point which gen_commit_tid(),
   * it still has not been decided whether or not this txn will commit
   * successfully
   */
  tid_t gen_commit_tid(const dbtuple_write_info_vec &write_tuples);

  bool can_read_tid(tid_t t) const;

  // For GC handlers- note that on_dbtuple_spill() is called
  // with the lock on ln held, to simplify GC code
  //
  // Is also called within an RCU read region
  void on_dbtuple_spill(dbtuple *tuple);

  // Called when the latest value written to ln is an empty
  // (delete) marker. The protocol can then decide how to schedule
  // the logical node for actual deletion
  void on_logical_delete(dbtuple *tuple);

  // if gen_commit_tid() is called, then on_tid_finish() will be called
  // with the commit tid. before on_tid_finish() is called, state is updated
  // with the resolution (commited, aborted) of this txn
  void on_tid_finish(tid_t commit_tid);

protected:
  inline void clear();

  // SLOW accessor methods- used for invariant checking

  typename read_set_map::iterator
  find_read_set(const dbtuple *tuple)
  {
    // linear scan- returns the *first* entry found
    // (a tuple can exist in the read_set more than once)
    typename read_set_map::iterator it     = read_set.begin();
    typename read_set_map::iterator it_end = read_set.end();
    for (; it != it_end; ++it)
      if (it->get_tuple() == tuple)
        break;
    return it;
  }

  inline typename read_set_map::const_iterator
  find_read_set(const dbtuple *tuple) const
  {
    return const_cast<transaction *>(this)->find_read_set(tuple);
  }

  typename write_set_map::iterator
  find_write_set(dbtuple *tuple)
  {
    // linear scan- returns the *first* entry found
    // (a tuple can exist in the write_set more than once)
    typename write_set_map::iterator it     = write_set.begin();
    typename write_set_map::iterator it_end = write_set.end();
    for (; it != it_end; ++it)
      if (it->get_tuple() == tuple)
        break;
    return it;
  }

  inline typename write_set_map::const_iterator
  find_write_set(const dbtuple *tuple) const
  {
    return const_cast<transaction *>(this)->find_write_set(tuple);
  }

  read_set_map read_set;
  write_set_map write_set;
  absent_set_map absent_set;

  string_allocator_type *sa;
};

class transaction_abort_exception : public std::exception {
public:
  transaction_abort_exception(transaction_base::abort_reason r)
    : r(r) {}
  inline transaction_base::abort_reason
  get_reason() const
  {
    return r;
  }
  virtual const char *
  what() const throw()
  {
    return transaction_base::AbortReasonStr(r);
  }
private:
  transaction_base::abort_reason r;
};

// XXX(stephentu): stupid hacks
template <template <typename, typename> class Transaction>
struct txn_epoch_sync {
  // block until the next epoch
  static inline void sync() {}
  // finish any async jobs
  static inline void finish() {}
};

#endif /* _NDB_TXN_H_ */

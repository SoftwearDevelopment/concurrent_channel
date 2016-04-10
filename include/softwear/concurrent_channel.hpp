#ifndef SOFTWEAR_CONCURRENT_PIPE_HEADER
#define SOFTWEAR_CONCURRENT_PIPE_HEADER

#include <cstdint>
#include <cmath>
#include <utility>
#include <atomic>
#include <thread>
#include <chrono>
#include <exception>
#include <type_traits>
#include <algorithm>
#include <iterator>

#include "concurrentqueue.h"

namespace softwear {

template<typename T, typename Traits>
class concurrent_channel;

namespace concurrent_channel_ {

/// Parent of all softwear::concurrent_channel related exceptions
class exception : public std::exception {
  virtual const char* what() const noexcept {
    return "softwear::concurrent_channel had some error";
  };
};

/// Thrown when attempting to perform an action on a closed channel;
/// in multi threaded environments that can happen even if
/// you checked with is_open(), due to race conditions.
class closed : public exception {
  virtual const char* what() const noexcept {
    return "Can not call close() or any enqueue method on "
        "a closed queue.";
  };
};

class invalid_dereference : public exception {
  virtual const char* what() const noexcept {
    return "Tried to dereference a non-dereferencable channel iterator";
  };
};

/// As specified in the concurrentqueue library, enqueue only
/// fails if memory allocation fails, or if memory
/// allocation is disabled.
/// Concurrent queue returns false in either case, we throw
/// this.
class lacking_memory : public exception {
  virtual const char* what() const noexcept {
    return "Allocation fails or Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE is 0";
  };
};

/// Traits for the concurrent_channel and the underlying
/// concurrent_queue.
/// You can just use concurrent_queue
struct default_traits : moodycamel::ConcurrentQueueDefaultTraits {
  /// The time in microseconds each blocked dequeue will
  /// wait between polls
  static const size_t WAIT_POLL_FREQ = 1000;

  /// How many items a concurrent_queue may contain.
  /// If this is zero, the channel may contain any number of items.
  static const size_t DEFAULT_CAPACITY = 0;
};

/// Token used to accelerate enqueueing; there are
/// enqueue() overloads that take this token; those will
/// operate faster than their counterparts with out token
/// for many calls.
///
/// A token may only be used by a thread at a time, but it
/// can be passed between threads.
class producer_token : public moodycamel::ProducerToken {
public:
  typedef moodycamel::ProducerToken super;

  template<typename T, typename Traits>
  producer_token(concurrent_channel<T, Traits> &queue) : super(queue.intern) {}
};

/// Token used to accelerate dequeueing; there are
/// dequeue() overloads that take this token; those will
/// operate faster than their counterparts with out token
/// for many calls.
///
/// A token may only be used by a thread at a time, but it
/// can be passed between threads.
class consumer_token : public moodycamel::ConsumerToken {
public:
  typedef moodycamel::ConsumerToken super;

  template<typename T, typename Traits>
  consumer_token(concurrent_channel<T, Traits> &queue) : super(queue.intern) {}
};


namespace detail {


template<typename Chan>
class channel_input_iterator {
public:
  typedef channel_input_iterator<Chan> this_type;

  typedef typename Chan::value_type value_type;
  typedef value_type& reference;
  typedef typename std::decay<value_type>::type* pointer;
  typedef std::input_iterator_tag iterator_category;

  friend Chan;

private:
  Chan &c;

  bool is_end = false; // This is how we implement the reference end()
  static this_type make_end(Chan &c) {
    channel_input_iterator r{c};
    r.is_end = true;
    return r;
  }

  // TODO: This will fail for types that have no default
  // constructor. Can we do better somehow?
  bool current_value = false;
  value_type cache{};

  consumer_token tok;

  void update_cache() {
    if (!current_value) {
      is_end = !c.dequeue(tok, cache);
      current_value = true;
    }
  }

public:
  channel_input_iterator(Chan &c) : c{c}, tok{c} {}

  channel_input_iterator(const this_type& o)
      : c{o.c}, is_end{o.is_end}, tok{c} {}
  this_type& operator=(const this_type& o) {
    c = o.c;
    is_end = o.is_end;
    tok = consumer_token{c};
    current_value = false;
  }

  this_type& operator++() { current_value = false; return *this; }

private:
  channel_input_iterator(Chan &c, bool is_end, value_type &&cache)
      : c{c}, is_end{is_end}, current_value{true}, cache{std::move(cache)},
        tok{c} {}
public:
  this_type operator++(int) {
    this_type ol{c, is_end, std::move(cache)};
    ++(*this);
    return ol;
  }

  reference operator*() {
    // Try getting an element; if the caller tested for
    // end() update_cache will do nothing and we can just
    // return the cached element; otherwise this might throw
    // an error
    update_cache();
    if (is_end) throw invalid_dereference();
    return cache;
  }
  pointer operator->() { return &(**this); }

  bool operator==(const this_type& o) {
    if (&c != &o.c) return false; // Different channels
    if (o.current_value) return false; // Other queue is definitely not == end()
    // Make sure we definitely have an element cached since
    // the queue might become closed between the caller
    // testing for end and actually dereferencing it.
    update_cache();
    return is_end;
  }
  template<typename T>
  bool operator!=(const T &o) { return !(*this == o); }

  void swap(this_type &o) {
    std::swap(c, o.c);
    std::swap(is_end, o.is_end);
    std::swap(current_value, o.current_value);
    std::swap(cache, o.cache);
    std::swap(tok, o.tok);
  }
};

template<typename Chan>
class channel_output_iterator {
public:
  typedef channel_output_iterator<Chan> this_type;

  typedef typename Chan::value_type value_type;
  typedef value_type& reference;
  typedef typename std::decay<value_type>::type* pointer;
  typedef std::output_iterator_tag iterator_category;

  friend Chan;

private:
  Chan &c;

  bool is_end_ = false; // This is how we implement the reference end()
  static this_type make_end(Chan &c) {
    channel_output_iterator r{c};
    r.is_end_ = true;
    return r;
  }
  bool is_end() { return is_end_ || c.closed(); }

  producer_token tok{c};
public:
  channel_output_iterator(Chan &c) : c{c}, tok{c}, proxy{*this} {}

  channel_output_iterator(const this_type& o) : c{o.c}, is_end_{o.is_end_} {}
  this_type& operator=(const this_type& o) {
    c = o.c;
    is_end_ == o.is_end_;
    tok = producer_token{c};
  }

  class proxy_t {
    this_type &it;

    proxy_t(this_type &it) : it{it} {}
    friend this_type;
  public:
    void operator=(const value_type &v) {
      it.c.enqueue(it.tok, v);
    }

    void operator=(value_type &&v) {
      it.c.enqueue(it.tok, std::move(v));
    }
  } proxy{*this};

  this_type& operator++() { return *this; }
  this_type operator++(int) { return ++(*this); }

  proxy_t& operator*() { return proxy; }

  bool operator==(const this_type& o) {
    return is_end() && o.is_end() && &c == &o.c;
  }
  template<typename T>
  bool operator!=(const T &o) { return !(*this == o); }

  void swap(this_type &o) {
    std::swap(c, o.c);
    std::swap(is_end_, o.is_end_);
    std::swap(tok, o.tok);
  }
};

// As stolen from
// https://github.com/cameron314/concurrentqueue/issues/46#issuecomment-205961910
template<typename It>
class input_iterator_ref_wrapper {
  It& ref;

public:
  typedef input_iterator_ref_wrapper<It> this_type;

  typedef decltype(*ref) value_type;
  typedef value_type& reference;
  typedef typename std::decay<value_type>::type* pointer;
  typedef std::input_iterator_tag iterator_category;

  input_iterator_ref_wrapper(It& ref) : ref(ref) { }

  this_type& operator++() { ++ref; return *this; }
  auto operator++(int) -> decltype(ref++) { return ref++; }

  value_type operator*() const { return *ref; }
  typename std::decay<value_type>::type* operator->() const { return &(*ref); }

  bool operator==(const It& o) { return ref == o; }
  bool operator==(const this_type& o) { return ref == o.ref; }

  template<typename T>
  bool operator!=(const T &o) { return !(*this == o); }

  void swap(this_type &o) { std::swap(ref, o.ref); }
};

} // namespace detail

} // namespace concurrent_channel_



/// Simple, thread safe shell-pipe like abstraction for queues.
///
/// Beyond a basic concurrent queue functionality, this
/// provides flow control and end-of-transmission signaling
/// between threads, specifically:
/// * Support for a simple capacity limit
/// * Support for enqueue methods that block if the channel is
///   at capacity
/// * Support for dequeue methods that block if no data is
///   available in the channel
/// * Support for a close() method and an eof() check to
///   allow provider threads to signal that there is no more
///   data to process.
///
/// It is mostly useful for simple setups doing very
/// specific things;
/// Personally, I implemented this for an application that
/// red data via network, ran some heavy computation on the
/// data then sent it to another network host:
/// I started a couple of IO threads for loading, a couple
/// to send the processed data and a couple for the actual
/// processing, with a channel between each step. If fetching
/// or the data processing where slow, the other threads
/// would implicitly wait for them. If the processing or
/// fetching where to fast, they would automatically stop
/// and wait for the other threads, so no thread can fill up
/// all the ram.
/// After the last fetching thread is done, it signals that
/// the job is done to processing; processing in turn can
/// forward the signal to the uploader, so in the management
/// thread very little effort is needed: It needs to set up
/// the channels and start the threads, then it just need to
/// join() all the threads and exit as soon as all the
/// threads are don.
///
/// In this implementation, polling is used to wait; this is
/// OK, if the queues are expected to mostly have data, if
/// latencies are not a big problem or if the cpu time can
/// be expended to use very short polling intervals. (It
/// usually is OK, except you know it is not).
/// Polling is used because it was easier to implement; it
/// also has the nice side effect of rendering this channel
/// implementation lock-free (you decide how much benefit
/// this actually brings).
/// A locking/signaling implementation should be provided in
/// the future.
///
/// This is based on https://github.com/cameron314/concurrentqueue,
/// which implements a lock-free queue.
template<typename T, typename Traits = concurrent_channel_::default_traits>
class concurrent_channel {
private:
  typedef concurrent_channel<T, Traits> this_type;
  typedef moodycamel::ConcurrentQueue<T, Traits> queue_t;

  /// The underlying queue
  queue_t intern;

public:

  typedef T value_type;
  typedef Traits traits_type;

  /// For convenient access: same as concurrent_channel_::producer_token
  typedef concurrent_channel_::producer_token producer_token;
  /// For convenient access: same as concurrent_channel_::consumer_token
  typedef concurrent_channel_::consumer_token consumer_token;
  /// For convenient access: same as concurrent_channel_::closed
  typedef concurrent_channel_::closed closed;
  /// For convenient access: same as concurrent_channel_::lacking_memory
  typedef concurrent_channel_::lacking_memory lacking_memory;

  friend producer_token;
  friend consumer_token;

private:
  template<typename O> using atomic = std::atomic<O>;

  /// Maximum (approximate) number of elements that may be
  /// contained in the queue
  atomic<size_t> capacity_approx_{Traits::DEFAULT_CAPACITY};

  /// How often waiting loops will check for changes in
  /// micro seconds. If this is 0, the threads will not wait
  /// at all
  atomic<uint_fast32_t> poll_freq_{Traits::WAIT_POLL_FREQ};

  /// Number of blocking batch operations in progress.
  /// This is used since batch operations reserve and
  /// release enqueue slots multiple times, so we need a way
  /// to mark the queue as still not EOF while these are
  /// running.
  atomic<size_t> enqueue_ops{0};

  // Number of elements in the process of being inserted
  // NOTE: If this is zero, it also means, that no enqueues
  // are in progress
  atomic<size_t> enqueue_slots_used{0};

  /// Whether this channel is closed
  atomic<bool> closed_{ false };

  /// Sleep used in wait_dequeue_*
  /// TODO: Get rid of the busy waits (could be backed by
  /// BlockingConcurrentQueue but it would have to support
  /// interruping waiting dequeue)
  void busy_wait_sleep() {
    if ( poll_freq() == 0) return;
    std::this_thread::sleep_for(
      std::chrono::microseconds(
        poll_freq() ));
  }

  struct enqueue_op_ {
    this_type *chan;

    enqueue_op_(this_type &chan_) : chan(&chan_) {
      if (chan) chan->enqueue_ops++;
    }

    ~enqueue_op_() {
      if (chan) chan->enqueue_ops--;
    }

    enqueue_op_(const enqueue_op_&) = delete;
    enqueue_op_& operator =(const enqueue_op_&) = delete;

    enqueue_op_(enqueue_op_ &&o) { *this = std::move(o); }
    enqueue_op_& operator =(enqueue_op_ &&o) {
      chan = o.chan;
      o.chan = nullptr;
      return *this;
    }
  };

  enqueue_op_ enqueue_op() {
    if (!is_open()) throw closed();
    enqueue_op_ o{*this};
    if (!is_open()) throw closed(); // Might have been closed in the meantime
    return o;
  }

  struct reservation {
    this_type *chan;
    size_t no;

    reservation(this_type &chan_, size_t no) : chan(&chan_), no(no) {
      if (chan) chan->enqueue_slots_used += no;
    }

    ~reservation() {
      if (chan) chan->enqueue_slots_used -= no;
    }

    reservation(const reservation&) = delete;
    reservation& operator=(const reservation&) = delete;

    reservation(reservation &&o) { *this = std::move(o); }
    reservation& operator=(reservation &&o) {
      chan = o.chan;
      no = o.no;
      o.no = 0;
      return *this;
    }

    operator bool() {
      return no != 0;
    }

    bool operator !() {
      return no == 0;
    }
  };

  /// How many elements can be reserved right now
  /// Can be negative if there are too many enqueue_slots in
  /// use
  ssize_t reservable_approx() {
    // TODO: This needs replacing with a proper scheduler
    return (ssize_t)capacity_approx()/2 - (size_approx() + enqueue_slots_used);
  }

  /// Tries to reserve between one and count enqueue slots
  /// Returns an empty (no = 0) reservation if it fails.
  /// @throws closed – In case the channel is closed
  reservation try_reserve(size_t count=1) {
    ssize_t avail = std::min<ssize_t>(reservable_approx(), count);
    if (avail <= 0) return {*this, 0};

    reservation r{*this, (size_t)avail};

    // Check whether our reservation collided with another
    // thread (then we failed)
    if (reservable_approx() < (ssize_t)-(count)) return {*this, 0};

    // Got the reservation
    return r;
  }

  // Waits until at least one enqueue slot can be reserved;
  // will reserve at most count slots
  /// @throws closed – In case the channel is closed
  reservation wait_reserve(size_t count=1) {
    while (true) {
      auto r = try_reserve(count);
      if (r) return r;
      busy_wait_sleep();
    }
  }

public:

  concurrent_channel(
      size_t capacity_approx = Traits::DEFAULT_CAPACITY
    , size_t poll_freq = Traits::WAIT_POLL_FREQ)
    : capacity_approx_(capacity_approx), poll_freq_(poll_freq) {}

  // TODO: Add constructors correctly sizing the ConcurrentQueue so we can avoid malloc.

  /// Move constructor; NOT THREAD SAFE
  concurrent_channel(this_type &&otr) = default;
  /// Move assignment; NOT THREAD SAFE
  this_type& operator=(this_type &&otr) = default;

  concurrent_channel(const this_type &otr) = delete;
  this_type& operator=(const this_type &otr) = delete;

  /// Enqueue the copy of one item
  /// Will block until there are enough free slots in the queue.
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
	inline void enqueue(T const &item) {
    auto o = enqueue_op();
    auto r = wait_reserve();
    if (!intern.enqueue(item)) throw lacking_memory{};
	}

  /// Enqueue one item by moving it into the queue
  /// Will block until there are enough free slots in the queue.
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
	inline void enqueue(T &&item) {
    auto o = enqueue_op();
    auto r = wait_reserve();
    if (!intern.enqueue( std::forward<T>(item) ))  throw lacking_memory{};
	}

  /// Enqueue one item by copy using a producer_token.
  /// Will block until there are enough free slots in the queue.
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
	inline void enqueue(producer_token const &token, T const &item) {
    auto o = enqueue_op();
    auto r = wait_reserve();
    if (!intern.enqueue(token, item)) throw lacking_memory{};
	}

  /// Enqueue one item by moving it into the queue using
  /// a producer token.
  /// Will block until there are enough free slots in the queue.
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
	inline void enqueue(producer_token const &token, T &&item) {
    auto o = enqueue_op();
    auto r = wait_reserve();
    if (!intern.enqueue(token, std::forward<T>(item))) throw lacking_memory{};
	}

  /// Enqueue multiple items.
  /// Will block until all elements are in the queue, adding
  /// as many elements as will fit at a time.
  ///
  /// Note: Use std::make_move_iterator if the elements
  /// should be moved instead of copied.
  ///
  /// Thread-safe.
  ///
  /// @throws closed
  /// @throws lacking_memory
	template<typename It>
	void enqueue_bulk(It itemFirst, size_t count) {
    auto o = enqueue_op();

    concurrent_channel_::detail::input_iterator_ref_wrapper<It> i{itemFirst};

    size_t done = 0;
    while (done < count) {
      auto r = wait_reserve(count - done);
      done += r.no;
      if (!intern.template enqueue_bulk<decltype(i)>(i, r.no))
        throw lacking_memory{};
    };
	}

  /// Enqueue multiple items with an explicit producer
  /// token.
  /// Will block until all elements are in the queue, adding
  /// as many elements as will fit at a time.
  ///
  /// Note: Use std::make_move_iterator if the elements
  /// should be moved instead of copied.
  ///
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
	template<typename It>
	void enqueue_bulk(producer_token const& tok, It itemFirst, size_t count) {
    // TODO: Implent this code only once for both enqueue_bulk
    auto o = enqueue_op();

    concurrent_channel_::detail::input_iterator_ref_wrapper<It> i{itemFirst};

    size_t done = 0;
    while (done < count) {
      auto r = wait_reserve(count - done);
      done += r.no;
      if (!intern.template enqueue_bulk<decltype(i)>(tok, i, r.no))
        throw lacking_memory{};
    };
	}

  /// Enqueue the copy of one item
  /// Will only add the element if there is sufficient
  /// capacity.
  ///
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
  /// @returns Whether the element was added.
	inline bool try_enqueue(T const& item) {
    auto o = enqueue_op();
    auto r = try_reserve();
    if (!r) return false;
    if (!intern.enqueue(item)) throw lacking_memory{};
    return true;
	}

  /// Enqueue an item moving it into the channel.
  /// Will only add the element if there is sufficient
  /// capacity.
  ///
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
  /// @returns Whether the element was added.
	inline bool try_enqueue(T &&item) {
    auto o = enqueue_op();
    auto r = try_reserve();
    if (!r) return false;
    if (!intern.enqueue(std::forward<T>(item))) throw lacking_memory{};
    return true;
	}

  /// Enqueue the copy of an item with an explicit producer
  /// token.
  /// Will only add the element if there is sufficient
  /// capacity.
  ///
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
  /// @returns Whether the element was added.
	inline bool try_enqueue(producer_token const &tok, T const &item) {
    auto o = enqueue_op();
    auto r = try_reserve();
    if (!r) return false;
    if (!intern.enqueue(tok, item)) throw lacking_memory{};
    return true;
	}

  /// Enqueue an item moving it into the queue with an
  /// explicit producer token.
  /// Will only add the element if there is sufficient
  /// capacity.
  ///
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
  /// @returns Whether the element was added.
	inline bool try_enqueue(producer_token const &tok, T &&item) {
    auto o = enqueue_op();
    auto r = try_reserve();
    if (!r) return false;
    if (!intern.enqueue(tok, std::forward<T>(item))) throw lacking_memory{};
    return true;
	}

  /// Enqueue multiple items.
  /// Will enqueue items until the channel is full.
  ///
  /// Note: Use std::make_move_iterator if the elements
  /// should be moved instead of copied.
  ///
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
  /// @returns How many elements have been enqueued
	template<typename It>
	size_t try_enqueue_bulk(It itemFirst, size_t count) {
    auto o = enqueue_op();
    auto r = try_reserve(count);
    if (!r) return 0;
		if (!intern.template enqueue_bulk<It>(itemFirst, r.no))
      throw lacking_memory{};
    return r.no;
	}

  /// Enqueue multiple items using an explicit producer
  /// token.
  /// Will enqueue items until the channel is full or until
  /// Traits::MAX_SUBQUEUE_SIZE.
  ///
  /// Note: Use std::make_move_iterator if the elements
  /// should be moved instead of copied.
  ///
  /// Thread-safe.
  /// @throws closed
  /// @throws lacking_memory
  /// @returns How many elements have been enqueued
	template<typename It>
	size_t try_enqueue_bulk(producer_token const &tok, It itemFirst, size_t count) {
    auto o = enqueue_op();
    auto r = try_reserve(count);
    if (!r) return 0;
		if (!intern.template enqueue_bulk<It>(tok, itemFirst, r.no)) throw lacking_memory{};
    return r.no;
	}

  /// Dequeue a single item.
  /// Will wait until an item is available.
  ///
  /// Only returns false if the eof()==true.
	template<typename U>
	bool dequeue(U &item) {
    for (;;) {
      if (eof()) return false;
      if (try_dequeue<U>( item )) return true;
      busy_wait_sleep();
    }
	}

  /// Dequeue a single item using an explicit consumer
  /// token.
  /// Will wait until an item is available.
  ///
  /// Only returns false if the eof()==true.
	template<typename U>
	bool dequeue(consumer_token &token, U &item) {
    for (;;) {
      if (eof()) return false;
      if (try_dequeue<U>( token, item )) return true;
      busy_wait_sleep();
    }
	}

  /// Dequeue multiple items.
  /// Will wait until at least one item is available.
  /// Only returns 0 items if eof()==true, or if max==0.
  ///
  /// @returns The number of items dequeued.
	template<typename It>
	size_t dequeue_bulk(It itemFirst, size_t max) {
    if (max == 0) return 0;
    for (;;) {
      if (eof()) return 0;
      size_t count = try_dequeue_bulk<It>(itemFirst, max);
      if (count > 0) return count;
      busy_wait_sleep();
    }
	}

  /// Dequeue multiple items using an explicit consumer token.
  /// Will wait until at least one item is available.
  /// Only returns 0 items if eof()==true, or if max==0.
  ///
  /// @returns The number of items dequeued.
	template<typename It>
	size_t dequeue_bulk(consumer_token &token, It itemFirst, size_t max) {
    if (max == 0) return 0;
    for (;;) {
      if (eof()) return 0;
      size_t count = try_dequeue_bulk<It>(token, itemFirst, max);
      if (count > 0) return count;
      busy_wait_sleep();
    }
	}

  /// Dequeue an item.
  /// Will only return an item if one is available.
  /// When using this function, you manually need to check
  /// for eof().
  ///
  /// @returns The number of items dequeued.
	template<typename U>
	bool try_dequeue(U& item) {
    return intern.template try_dequeue<U>( item );
	}

  /// Dequeue an item with an explicit consumer token.
  /// Will only return an item if one is available.
  /// When using this function, you manually need to check
  /// for eof().
  ///
  /// @returns The number of items dequeued.
	template<typename U>
	bool try_dequeue(consumer_token& token, U& item) {
    return intern.template try_dequeue<U>( token, item );
	}

  /// Dequeue multiple items.
  /// Will return zero items if there are no items
  /// currently.
  /// When using this function, you manually need to check
  /// for eof().
  ///
  /// @returns The number of elements enqueued
	template<typename It>
	size_t try_dequeue_bulk(It itemFirst, size_t max) {
    return intern.template try_dequeue_bulk<It>( itemFirst, max );
	}

  /// Dequeue multiple items using an explicit consumer
  /// token.
  /// Will return zero items if there are no items
  /// currently.
  /// When using this function, you manually need to check
  /// for eof().
  ///
  /// @returns The number of elements enqueued
	template<typename It>
	size_t try_dequeue_bulk(consumer_token& token, It itemFirst, size_t max) {
    return intern.template try_dequeue_bulk<It>( token, itemFirst, max );
	}

private:
  typedef concurrent_channel_::detail::channel_input_iterator<this_type> RIt;
  typedef concurrent_channel_::detail::channel_output_iterator<this_type> WIt;

public:
  /// Iterator that can be used to read from the channel.
  ///
  /// *(++i) on the iterator is equivalent to a call to
  /// dequeue
  ///
  /// Comparing the iterator with end() or
  /// dereferencing it will dequeue one element and store it
  /// in the iterator. If you don't use it afterwards, it
  /// will be lost.
  /// This can happen easily for instance when using a for-in
  /// loop, since the for-in loop will compare with end()
  /// implicitly before running the code block.
  /// This also means that a comparison with end() might
  /// block.
  ///
  /// The iterators are invalidated by moving the queue
  /// into a different container.
  RIt begin() {
    return RIt{*this};
  }

  /// End of the iterator that can be used for reading.
  RIt end() {
    return RIt::make_end(*this);
  }

  /// Range for enqueueing elements.
  ///
  /// In general, using end() can't be relied on; take the
  /// following code for instance:
  /// `i == end(); *i = something;` is the same as `c.eof(); c.enqueue(i)`.
  /// This code checks whether the channel has ended, but
  /// even if that check succeeds, the channel may still be
  /// closed between the check and calling enqueue, so the
  /// assignment could throw closed.
  /// Any sequence of calls other than `assingment;
  /// increment; assignment; ...` results in undefined
  /// behaviour.
  ///
  /// The iterators are invalidated by moving the queue
  /// into a different container.
  class iwrite_t {
    this_type &c;

    iwrite_t(this_type &c) : c{c} {}
    friend this_type;
  public:
    WIt begin() {
      return WIt{c};
    }

    WIt end() {
      return WIt::make_end(c);
    }
  } iwrite{*this};


public:
  /// Estimate the number of elements in this channel.
  ///
  /// The estimate is only accurate if the queue is
  /// stabilized during the call; i.e. no concurrent
  /// operations are running.
  ///
  /// Thread-safe.
	inline size_t size_approx() const {
    return intern.size_approx();
	}

  /// Checks whether the underlying atomics are lock free.
  ///
  /// Thread-safe.
	static bool is_lock_free() {
		return queue_t::is_lock_free()
        && atomic<bool>{}.is_lock_free()
        && atomic<size_t>{}.is_lock_free()
        && atomic<uint_fast32_t>{}.is_lock_free();
	}

public:

  /// The approximate capacity of the queue. This is not for
  /// optimization, instead this is a tool for flow control:
  /// In cases where there are more elements written to the
  /// queue than are written, this can make sure that not
  /// many more than the capacity_approx() elements can be
  /// added to the queue.
  ///
  /// The approximate capacity should be a lot greater than
  /// the average number of elements added in a batch.
  ///
  /// Specifically the capacity should *always* be greater
  /// than the number of threads enqueueing in parallel and
  /// at least 10.
  ///
  /// Internally, size_approx() is used to check whether
  /// more elements can be added; hence capacity_approx()
  /// has the same limitations in terms of correctness:
  /// When batches are added concurrently to the queue the
  /// number of available sots may be estimated incorrectly
  /// and a insertion may be allowed even though there is
  /// enough space or the other way around.
  ///
  /// In practice the queue stays at a sensible size in all
  /// of my tests, though if you are using the queue in
  /// intense workloads, you should test whether this
  /// creates any problems.
  ///
  /// Thread-safe.
  size_t capacity_approx(size_t val) { return capacity_approx_ = val; }
  size_t capacity_approx() { return capacity_approx_; }

  /// This channel implements all waiting options using polling.
  /// poll_freq indicates how often waiting threads will
  /// poll for changes in micro seconds. A value of 0 will
  /// cause the thread not to wait and poll as often as
  /// possible. WARNING: This usually uses 100% cpu.
  size_t poll_freq(size_t val) { return poll_freq_ = val; }
  size_t poll_freq() { return poll_freq_; }

  /// Whether further dequeues will deliver any more
  /// elements.
  /// This is true if the queue is_open() or if it is closed
  /// but there are more elements to be red.
  /// Thread safe!
  bool eof() const {
    return closed_
        && enqueue_slots_used == 0
        && enqueue_ops == 0
        && this->size_approx() == 0;
  }

  /// Whether it is permitted to write to the queue
  /// Thread safe!
  bool is_open() const { return !closed_; }

  /// Close the queue. It is not permitted to enqueue
  /// elements after the queue is closed.
  /// If the queue is already closed, no change will be
  /// applied.
  /// Thread safe!
  void close() {
    closed_ = true;
  }

  /// This method will return when EOF is reached
  /// Thread safe!
  void wait_for_eof() {
    while (!eof()) busy_wait_sleep();
  }
};

}

#endif

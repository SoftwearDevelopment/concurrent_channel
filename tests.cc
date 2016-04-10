#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <random>
#include <array>
#include <iterator>
#include <algorithm>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>

#include "softwear/concurrent_channel.hpp"

using std::move;
using std::forward;
using std::atomic;
using std::to_string;

using softwear::concurrent_channel;

uint64_t milli_epoch() {
  using namespace std::chrono;
  steady_clock::duration d{ steady_clock::now().time_since_epoch() };
  return duration_cast<milliseconds>(d).count();
}

// RANDOM NUMBERS //////////////////////////////////////////


static thread_local std::random_device rnd_device;

/// std::uniform_int_distribution for integral values an
/// std::uniform_real_distribution for floating point
template<typename T>
using uniform_numeric_distribution = typename
    std::conditional< std::is_floating_point<T>::value,
        std::uniform_real_distribution<T>,
        std::uniform_int_distribution<T>
    >::type;

template<typename T>
struct auto_seeded : T {
  T *super = (T*)this;
  auto_seeded() {
    super->seed(rnd_device());
  };
};

thread_local auto_seeded<std::mt19937> rng;

template<typename T>
T random(const T min, const T max) {
  uniform_numeric_distribution<T> dist{ min, max };
  return dist(rng);
}

template<typename T>
T random() {
  return random(std::numeric_limits<T>::min(),
                std::numeric_limits<T>::max());
}

/// Returns true with the given likelihood
template<typename N=float>
bool decide(N likelihood=0.5) {
  return random<N>(0, 1) < likelihood;
}


// COLLISION DETECTION /////////////////////////////////////


/// A set that can contain the same element multiple times
/// and keeps track of the amount per element.
///
/// No support for move semantics.
///
/// The set is made concurrent by simply allowing just one
/// element to be added
template<typename T>
class concurrent_counting_set : public std::unordered_map<T, size_t> {
  typedef std::unordered_map<T, size_t> super_t;
  super_t *super = (super_t*)this;
  atomic<size_t> uniques{0}, total{0};

  std::mutex mtx;
public:
  size_t count_uniques() { return uniques; }
  size_t count_total() { return total; }

  /// Insert an element into the set and count how often the
  /// element is now in the set.
  /// The count returned after an element has been inserted
  /// for the first time is 1.
  size_t insert(const T &k) {
    std::lock_guard<std::mutex> lock(mtx);

    auto r = super->insert({k, 0});
    size_t &count = r.first->second;
    count++;

    total++;
    if (count == 1) uniques++;

    return count;
  }
};


// THREADING ///////////////////////////////////////////////


/// Very thin abstraction over a vector of threads, that
/// just takes a function and the number of threads to
/// start.
/// Useful for starting many workers.
///
/// Other than std::thread, this will *not* store decay
/// copies of all references passed.
struct many_threads : std::vector<std::thread> {
  template<typename F, typename... Args>
  many_threads(size_t n, F f, Args&&... args) {
    reserve(n);
    auto lam = [f, &args...]() { return f( forward<Args>(args)... ); };
    for (; n > 0; n--) emplace_back(lam);
  }

  /// Join every thread in this group
  void join() {
    for (auto &t : *this) t.join();
  }
};


// TEST FRAMEWORK //////////////////////////////////////////

class null_stream : public std::ostream {
  class null_buffer_t : public std::streambuf {
  public:
    using std::streambuf::streambuf;
    int overflow(int c) { return c; }
  };

public:
  null_buffer_t null_buf;
  null_stream() : std::ostream(&null_buf) {}
};

null_stream cnull;

#ifdef ASSERT
#undef ASSERT
#endif

#define ASSERT(errc, check)                                   \
  ([&]() -> std::ostream& {                                   \
    if (check)                                                \
      return cnull;                                           \
    else                                                      \
      return std::cerr                                        \
        << "[ERROR] Assertion failure #" << (++(errc))        \
        << " in " << __FILE__ << " : " << __LINE__ << ": `" << #check << "` "; \
  })()

#define ASSERT_OPERATOR_(errc, a, op, b) \
  ([&]() -> std::ostream& {              \
    auto &&v1 = (a);                     \
    auto &&v2 = (b);                     \
    return ASSERT(errc, v1 op v2)        \
      << " where " << #a << " = " << v1 << "; " << #b << " = " << v2 << " "; \
  })()

#define ASSERT_EQ(errc, a, b)  ASSERT_OPERATOR_(errc, (a), ==, (b))
#define ASSERT_NEQ(errc, a, b) ASSERT_OPERATOR_(errc, (a), !=, (b))
#define ASSERT_LE(errc, a, b)  ASSERT_OPERATOR_(errc, (a), <=, (b))

#define ASSERT_THROW(errc, exception, statement) \
  ASSERT(errc, ([&](){                           \
    try {                                                   \
      { statement ; }                                       \
      return false;                                         \
    } catch (exception) {                                   \
      return true;                                          \
    }                                                       \
  })() ) << "; `" << #statement << "` should have thrown `" \
         << #exception << "`"

// Tests ///////////////////////////////////////////////////

// (single, bulk) x (copy, move) x (not, consumer token) x (try, wait)
//
// * waiting modes wait indefinitely for at least one element
// * use multiple capacities for waiting modes
// * can change capacity on the fly, existing data stays in
// * non blocking modes never block
// * waiting modes exit on eof()

atomic<uint64_t> test_packet_counter{0};

class test_packet {
public:
  /// The UUID of this packet
  uint64_t id;

  /// Semaphore for the numbers or copies or moves;
  /// will set to the expected number of moves copies so we
  /// can detect a mismatch when extracting the packet
  int moves = 0, copies = 0,
      expected_moves = 0, expected_copies = 0;

  test_packet() noexcept : id(test_packet_counter++) {}
  test_packet(uint64_t id) noexcept : id(id) {}

  test_packet(test_packet &&p) noexcept { *this = move(p); }
  test_packet(const test_packet &p) noexcept { *this = p; }

  test_packet& operator=(test_packet &&p) noexcept {
    id = p.id;
    moves = p.moves + 1;
    copies = p.copies;
    expected_moves = p.expected_moves;
    expected_copies = p.expected_copies;
    return *this;
  };

  test_packet& operator=(const test_packet &p) noexcept {
    id = p.id;
    moves = p.moves;
    copies = p.copies + 1;
    expected_moves = p.expected_moves;
    expected_copies = p.expected_copies;
    return *this;
  };
};


typedef concurrent_counting_set<uint64_t> pkg_tracker;
typedef concurrent_channel<test_packet> chan;


void producer(chan &c, pkg_tracker &pkgc, atomic<size_t> &errc,
              atomic<size_t> &pkg_sent, size_t test_mag) {

  chan::producer_token tok{c};
  std::vector<test_packet> bulk_buf;
  bulk_buf.reserve(4000);

  // TODO: Test non blocking enqueue

  try { // Wait for the pipe closed signal
    while (true) {
      ASSERT_LE(errc, c.size_approx(), c.capacity_approx()*2)
        << "Number of elements in the channel should not "
        << "exceed it's capacity by more than factor two." << std::endl;

      if (pkg_sent > test_mag) c.close();

      // TODO: Make sure each kind has been run N times
      bool bulk = decide(0.1);
      bool itr  = decide(0.05);
      int  bulk_size = decide(0.01) ? 0 // 1% empty
                                    : random<int>(1, bulk_buf.capacity());
      bool use_move = decide(0.01);
      bool use_tok = decide(0.6);
      bool use_try = decide(0.05);

      if (itr) { // ITERATOR

        // TODO: Reuse code from bulk
        bulk_buf.clear(); // Init the bulk of test packages
        bulk_buf.resize(bulk_size);

        for (auto &p : bulk_buf) {
          p.expected_moves = 1; // Accounting for the move out of the queue
          if (use_move) p.expected_moves  = 2; // here too
          else          p.expected_copies = 1;
        }

        auto i = c.iwrite.begin();
        for (auto &p : bulk_buf) {
          if (use_move) *(++i) = std::move(p);
          else          *(++i) = p;

          ASSERT_LE(errc, pkgc.insert(p.id), (size_t)2)
            << "Assumed producer would be the first or second to insert packet '"
            << p.id << "'. Collision?" << std::endl;

          pkg_sent++;
        }

      } else if (bulk) { // BATCH MODE TEST

        bulk_buf.clear(); // Init the bulk of test packages
        bulk_buf.resize(bulk_size);

        for (auto &p : bulk_buf) {
          p.expected_moves = 1; // Accounting for the move out of the queue
          if (use_move) p.expected_moves  = 2; // here too
          else          p.expected_copies = 1;
        }

        if (!use_try){ // BLOCKING
          auto cit = bulk_buf.begin();
          auto mit = make_move_iterator(cit);

          if ( use_move &&  use_tok) c.enqueue_bulk(tok, mit, bulk_buf.size());
          if ( use_move && !use_tok) c.enqueue_bulk(     mit, bulk_buf.size());
          if (!use_move &&  use_tok) c.enqueue_bulk(tok, cit, bulk_buf.size());
          if (!use_move && !use_tok) c.enqueue_bulk(     cit, bulk_buf.size());

          for (auto &p : bulk_buf) {
            ASSERT_LE(errc, pkgc.insert(p.id), (size_t)2)
              << "Assumed producer would be the first or second to insert packet '"
              << p.id << "'. Collision?" << std::endl;
          }

        } else { // NON BLOCKING
          size_t done = 0;

          while (done < bulk_buf.size()) {
            size_t todo = bulk_buf.size() - done;

            auto cit = bulk_buf.begin() + done;
            auto mit = make_move_iterator(cit);

            size_t bs;
            if ( use_move &&  use_tok) bs = c.try_enqueue_bulk(tok, mit, todo);
            if ( use_move && !use_tok) bs = c.try_enqueue_bulk(     mit, todo);
            if (!use_move &&  use_tok) bs = c.try_enqueue_bulk(tok, cit, todo);
            if (!use_move && !use_tok) bs = c.try_enqueue_bulk(     cit, todo);
            done += bs;

            // Insert all the packages we just enqueued; we
            // need to do this here, since with each new
            // try_enqueue_bulk there is a potential for the
            // stream to become closed by another thread.
            auto p = cit;
            for (size_t i=0; i<bs; i++, p++) {
              ASSERT_LE(errc, pkgc.insert(p->id), (size_t)2)
                << "Assumed producer would be the first or second "
                << "to insert packet '" << p->id << "'. Collision?" << std::endl;
            }

          }
        }

        pkg_sent += bulk_size;
      } else { // SINGLE MODE TEST

        test_packet p;

        p.expected_moves = 1; // Accounting for the move out of the queue
        if (use_move) p.expected_moves  = 2; // here too
        else          p.expected_copies = 1;

        if (!use_try){ // BLOCKING
          if ( use_move &&  use_tok) c.enqueue(tok, move(p));
          if ( use_move && !use_tok) c.enqueue(     move(p));
          if (!use_move &&  use_tok) c.enqueue(tok, p);
          if (!use_move && !use_tok) c.enqueue(     p);

        } else { // NON BLOCKING

          bool suc = false;
          while (!suc) {
            // TODO: Can we check whether they're really
            // returning immediately?
            if ( use_move &&  use_tok) suc = c.try_enqueue(tok, move(p));
            if ( use_move && !use_tok) suc = c.try_enqueue(     move(p));
            if (!use_move &&  use_tok) suc = c.try_enqueue(tok, p);
            if (!use_move && !use_tok) suc = c.try_enqueue(     p);
          }
        }

        ASSERT_LE(errc, pkgc.insert(p.id), (size_t)2)
          << "Assumed producer would be the first or second to insert packet '"
          << p.id << "'. Collision?" << std::endl;

        pkg_sent++;
      }
    }
  } catch (chan::closed &e) {
    // pass; business as usual; this is just the signal to
    // quit
  }

  // Test that the channel really is closed and enforces
  // that!

  ASSERT(errc, !c.is_open())
    << "Some producer should have closed the channel." << std::endl;

  test_packet p;

  ASSERT_THROW(errc, chan::closed, c.enqueue(     p));
  ASSERT_THROW(errc, chan::closed, c.enqueue(     move(p)));
  ASSERT_THROW(errc, chan::closed, c.enqueue(tok, p));
  ASSERT_THROW(errc, chan::closed, c.enqueue(tok, move(p)));
  ASSERT_THROW(errc, chan::closed, c.try_enqueue(     p));
  ASSERT_THROW(errc, chan::closed, c.try_enqueue(     move(p)));
  ASSERT_THROW(errc, chan::closed, c.try_enqueue(tok, p));
  ASSERT_THROW(errc, chan::closed, c.try_enqueue(tok, move(p)));

  size_t bulk_size = 5;
  bulk_buf.clear(); // Init the bulk of test packages
  bulk_buf.resize(bulk_size);

  auto cit = bulk_buf.begin();
  std::move_iterator<decltype(cit)> mit(cit);

  ASSERT_THROW(errc, chan::closed, c.enqueue_bulk(     cit, bulk_size));
  ASSERT_THROW(errc, chan::closed, c.enqueue_bulk(     mit, bulk_size));
  ASSERT_THROW(errc, chan::closed, c.enqueue_bulk(tok, cit, bulk_size));
  ASSERT_THROW(errc, chan::closed, c.enqueue_bulk(tok, cit, bulk_size));
  ASSERT_THROW(errc, chan::closed, c.try_enqueue_bulk(     cit, bulk_size));
  ASSERT_THROW(errc, chan::closed, c.try_enqueue_bulk(     mit, bulk_size));
  ASSERT_THROW(errc, chan::closed, c.try_enqueue_bulk(tok, cit, bulk_size));
  ASSERT_THROW(errc, chan::closed, c.try_enqueue_bulk(tok, cit, bulk_size));
}


void consumer(chan &c, pkg_tracker &pkgc, atomic<size_t> &errc,
              atomic<size_t> &pkg_recvd) {
  chan::consumer_token tok{c};

  std::vector<test_packet> bulk_buf;
  bulk_buf.reserve(4000);

  test_packet zero_packet;
  zero_packet.id = 0;

  // TODO: Test other kinds of dequeueing

  bool eof = false;
  while (true) {

    // Randomize //

    bool bulk = decide(0.1);
    bool itr  = decide(0.05);

    size_t bulk_size;
    if      (!bulk && !itr)        bulk_size = 1;
    else if (decide(0.01) && !itr) bulk_size = 0;
    else                           bulk_size = random<int>(1, bulk_buf.capacity());

    bool use_tok = decide(0.7);
    bool use_try = decide(0.05);

    // Dequeue //

    // using zero_packet here to avoid unnecessary rng.
    bulk_buf.resize(bulk_size, zero_packet);

    if (itr) { // ITERATOR DEQUEUE

      size_t no=0;
      for (auto &p : c) {
        p.expected_moves++; // The iterator needs one additional move
        bulk_buf[no] = move(p);

        // Implicitly limiting the bulk_size to at least 1 here
        no++;
        if (no >= bulk_size) break;
      }
      bulk_buf.resize(no);
      eof = no == 0;

    } else if (!use_try) {
      if (!bulk) {

        // SINGLE BLOCKiNG
        if (use_tok) eof = !c.dequeue(tok, bulk_buf[0]);
        else         eof = !c.dequeue(     bulk_buf[0]);

      } else  {

        // BULK BLOCKING
        size_t no;
        if (use_tok) no = c.dequeue_bulk(tok, bulk_buf.begin(), bulk_buf.size());
        else         no = c.dequeue_bulk(     bulk_buf.begin(), bulk_buf.size());
        bulk_buf.resize(no);
        eof = bulk_size > 0 ? no == 0 : c.eof();

      }
    } else {

      if (!bulk) {

        // SINGLE NON-BLOCKING
        eof = c.eof();
        bool suc;
        if (use_tok) suc = c.dequeue(tok, bulk_buf[0]);
        else         suc = c.dequeue(     bulk_buf[0]);
        bulk_buf.resize(suc ? 1 : 0);

      } else  {

        // BULK NON-BLOCKING
        eof = c.eof();
        size_t no;
        if (use_tok)
          no = c.try_dequeue_bulk(tok, bulk_buf.begin(), bulk_buf.size());
        else
          no = c.try_dequeue_bulk(     bulk_buf.begin(), bulk_buf.size());
        bulk_buf.resize(no);

      }
    }

    if (eof) return;

    // test //

    pkg_recvd += bulk_buf.size();

    for (auto &p : bulk_buf) {
      ASSERT_EQ(errc, p.copies, p.expected_copies) << "Packet was copied more "
        << "or less times than expected." << std::endl;

      ASSERT_EQ(errc, p.moves, p.expected_moves) << "Packet was moved more "
        << "or less times than expected." << std::endl;

      ASSERT_LE(errc, pkgc.insert(p.id), (size_t)2)
        << "Assumed consumer would be the first or second to insert packet '"
        << p.id << "'. Collision?" << std::endl;
    }
  }
}


int main(int argc, char **argv) {

  if (argc >= 2 && std::string(argv[1]) == "--help") {
    std::cerr << "USAGE: ./test "
         "[PRODUCER_THREADS] [CONSUMER_THREADS] "
         "[PACKAGE_NUMBER] [CHANNEL_CAPACITY]"
         "\n\n./test 2 3 10000"
         "\n Would start 2 producer threads, 3 consumer tests and "
         "would quit testing after 10k test packages sent through "
         "the channel.\n";
    return 0;
  }

  size_t producer_no = 25;
  size_t consumer_no = 10;

  // Test for around 20 million transferred packets (+1M
  // packets at most, when accounting for bulk sends).
  // This should put the memory requirement for storing the
  // packets somewhere <1GB; if we want more we should start
  // using a sliding window of stored UUIDs.
  size_t test_mag = 20000000;

  // (parse cli args)
  if (argc >= 2) producer_no = strtoul(argv[1], NULL, 10);
  if (argc >= 3) consumer_no = strtoul(argv[2], NULL, 10);
  if (argc >= 4) test_mag = strtoul(argv[3], NULL, 10);


  // DECLARE ALL DATA (AVOIDING GLOBAL STATE ///////////////
  // The actual pipe we're testing

  // TODO: Test move assignment & construction
  // TODO: Add pure, single threaded unit tests
  chan c;
  c.capacity_approx(producer_no);
  if (argc >= 5) c.capacity_approx(strtoul(argv[4], NULL, 10));

  // Producer & consumer add to this once each; the producer
  // expects to be the first who adds a package, the
  // consumer expects to be the second. This way we can
  // detect duplicate send/received packages.
  pkg_tracker pkgc;

  // We don't exit on errors; we just count them and print them
  atomic<size_t> errc{0};

  // Counter for how many packets have been sent and
  // received; this is used to check that all packets have
  // been received and to stop the test after we tested 20M
  atomic<size_t> pkg_sent{0}, pkg_recvd{0};

  std::cerr
    << "\n[INFO] Starting tests; printing statistics while doing so. "
    << "Keep in mind that we are searching for race conditions, running "
    << "with many threads. All the statistics are not stabilized until "
    << "the program exists."
    << "\n"
    << "\n[INFO] [CLOSED] If this is displayed, the pipe has been closed"
    << "\n[INFO] [EOF] If this is displayed, the pipe has been closed and "
    <<           "all consumers are done reading from it"
    << "\n[INFO] W  – The number of packets produced by now"
    << "\n[INFO] R  - The number of packets consumed by now"
    << "\n[INFO] Er - The number of errors encountered"
    << "\n[INFO] ChanSize - Amount of packages in the channel"
    << "\n[INFO] ChanCap - Capacity/maximum number of packages in the channel"
    << "\n[INFO] TrackUniq - Number of uniq packets registered"
    << "\n[INFO] TrackTotal - Total number packets registered"
    << "\n[INFO] W R and TrackedUniq should be roughly the same; TrackTotal "
    << "should be TrackedUniq*2, Er should stay zero.\n\n";

  auto report = [&]() {
    std::cerr << "[INFO]"
      << (!c.is_open() ? " [CLOSED]" : "")
      << (c.eof() ? " [EOF]" : "")
      << "\t" << std::setprecision(3) << (pkg_recvd / (double)test_mag) * 100
      << "\tW=" << pkg_sent
      << "\tR=" << pkg_recvd
      << "\tEr=" << errc
      << "\tChanSize=" << c.size_approx()
      << "\tChanCap=" << c.capacity_approx()
      << "\tTrackUniq=" << pkgc.count_uniques()
      << "\tTrackTotal=" << pkgc.count_total()
      << std::endl;
  };

  // RUN & FINISH THE TESTS ////////////////////////////////


  many_threads tpro{producer_no, producer, c, pkgc, errc, pkg_sent, test_mag};
  many_threads tcon{consumer_no, consumer, c, pkgc, errc, pkg_recvd};

  // Observer Thread
  atomic<bool> stop_observer{false};
  std::thread observer{[&]() {
    size_t old_pkg_recvd = pkg_recvd, old_pkg_sent = pkg_sent;
    uint64_t lastcheck = milli_epoch();

    while (!stop_observer) {

      if (milli_epoch() > lastcheck + 15000) {
        if (old_pkg_recvd == pkg_recvd) {
          ASSERT(errc, false)
            << "Number of received packages has not changed since more than "
            << "15 seconds. Are we stalled? Fatal error..." << std::endl;
          exit(21);
        }
        if (old_pkg_sent == pkg_sent) {
          ASSERT(errc, false)
            << "Number of sent packages has not changed since more than "
            << "15 seconds. Are we stalled? Fatal error..." << std::endl;
          exit(21);
        }

        old_pkg_recvd = pkg_recvd;
        old_pkg_sent = pkg_sent;
        lastcheck = milli_epoch();
      }

      report();

      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
  }};

  tpro.join();
  tcon.join();
  stop_observer = true;
  observer.join();


  // SHUTDOWN & FINAL STATE CHECKS /////////////////////////

  ASSERT(errc, c.eof()) << "Consumers should have drained the pipe\n";

  for (auto &pair : pkgc) {
    ASSERT_EQ(errc, pair.second, (uint64_t)2)
      << "Packet #" << pair.first << " should have been inserted "
      << "twice: Once by the consumer and once by the producer.`\n";
  }

  ASSERT_EQ(errc, pkg_sent, pkg_recvd) << " We should have received "
    << "exactly as many packages as we sent" << std::endl;

  // Final report
  report();

  if (errc == 0) {
    std::cerr << "Exiting without errors; test succeeded.\n";
    return 0;
  } else {
    std::cerr << "Test FAILED. Exiting with Errors..\n";
    return 20;
  }
}

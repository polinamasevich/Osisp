#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t VARIANT_ID = 19;
constexpr uint32_t STUDENT_ID = 73;
constexpr uint32_t MAX_STAGES = 8;
constexpr uint32_t MAX_WORKERS = 32;

constexpr uint32_t DEFAULT_SEED = 1000 + STUDENT_ID * 97;

LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_qpcStart{};
volatile LONG g_stop = 0;
CRITICAL_SECTION g_logCs;
bool g_verbose = true;

LONGLONG nowUs() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (c.QuadPart - g_qpcStart.QuadPart) * 1000000LL / g_qpcFreq.QuadPart;
}

void printWinError(const char *function) {
  DWORD code = GetLastError();
  char text[512]{};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text,
                 sizeof(text), nullptr);
  std::fprintf(stderr, "ERROR %s GetLastError=%lu: %s\n", function, code, text);
}

void logf(bool important, const char *role, const char *fmt, ...) {
  if (!g_verbose && !important)
    return;
  char body[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  EnterCriticalSection(&g_logCs);
  std::printf("[%06lld ms] %-9s | TID=%-6lu | %s\n", nowUs() / 1000, role, GetCurrentThreadId(),
              body);
  std::fflush(stdout);
  LeaveCriticalSection(&g_logCs);
}

void busyWaitUs(LONGLONG us) {
  LONGLONG t0 = nowUs();
  while (nowUs() - t0 < us) {
    if (g_stop)
      return;
    YieldProcessor();
  }
}

struct Rng64 {
  uint64_t s;
  explicit Rng64(uint64_t seed) : s(seed ? seed : 1) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

struct Request {
  uint32_t id = 0;
  LONGLONG createdAtUs = 0;
  LONGLONG planUs[MAX_STAGES] = {};
  LONG stageDone[MAX_STAGES] = {};
  LONGLONG waitUs = 0;
  LONGLONG procUs = 0;
  LONG rejected = 0;
  LONG collected = 0;
};

struct Invariants {
  LONG queueOverflow = 0;
  LONG batchIncomplete = 0;
  LONG stageCounterBad = 0;
  LONG balanceBad = 0;
  LONG residualBad = 0;
  LONG barrierTimeouts = 0;
  LONG total() const {
    return queueOverflow + batchIncomplete + stageCounterBad + balanceBad + residualBad +
           barrierTimeouts;
  }
};

struct Batch {
  std::vector<uint32_t> items;
  LONGLONG readyAtUs = 0;
  LONG processed = 0;
  uint32_t round = 0;
};

enum PopResult { POP_OK, POP_ENDED, POP_STOP };

class BoundedQueue {
public:
  BoundedQueue() = default;
  ~BoundedQueue() {
    if (init_)
      DeleteCriticalSection(&cs_);
  }
  BoundedQueue(const BoundedQueue &) = delete;
  BoundedQueue &operator=(const BoundedQueue &) = delete;

  bool init(size_t capacity, Invariants *inv) {
    capacity_ = capacity;
    inv_ = inv;
    InitializeCriticalSection(&cs_);
    InitializeConditionVariable(&notEmpty_);
    InitializeConditionVariable(&notFull_);
    init_ = true;
    return true;
  }

  bool push(Batch &&batch, uint32_t timeoutMs) {
    LONGLONG deadline = nowUs() + static_cast<LONGLONG>(timeoutMs) * 1000;
    EnterCriticalSection(&cs_);
    while (items_.size() >= capacity_) {
      if (g_stop) {
        LeaveCriticalSection(&cs_);
        return false;
      }
      LONGLONG left = deadline - nowUs();
      if (left <= 0) {
        LeaveCriticalSection(&cs_);
        return false;
      }
      DWORD wait = static_cast<DWORD>(std::min<LONGLONG>(20, (left + 999) / 1000));
      SleepConditionVariableCS(&notFull_, &cs_, wait);
    }
    batch.readyAtUs = nowUs();
    batch.processed = 0;
    items_.push_back(std::move(batch));
    if (items_.size() > capacity_)
      InterlockedIncrement(&inv_->queueOverflow);
    WakeConditionVariable(&notEmpty_);
    LeaveCriticalSection(&cs_);
    return true;
  }

  PopResult pop(Batch &out) {
    EnterCriticalSection(&cs_);
    while (items_.empty()) {
      if (InterlockedCompareExchange(&upstreamDone_, 0, 0)) {
        LeaveCriticalSection(&cs_);
        return POP_ENDED;
      }
      if (g_stop) {
        LeaveCriticalSection(&cs_);
        return POP_STOP;
      }
      SleepConditionVariableCS(&notEmpty_, &cs_, 20);
    }
    out = std::move(items_.front());
    items_.pop_front();
    WakeConditionVariable(&notFull_);
    LeaveCriticalSection(&cs_);
    return POP_OK;
  }

  void markUpstreamDone() {
    InterlockedExchange(&upstreamDone_, 1);
    wakeAll();
  }

  void wakeAll() {
    EnterCriticalSection(&cs_);
    WakeAllConditionVariable(&notEmpty_);
    WakeAllConditionVariable(&notFull_);
    LeaveCriticalSection(&cs_);
  }

  size_t size() {
    EnterCriticalSection(&cs_);
    size_t s = items_.size();
    LeaveCriticalSection(&cs_);
    return s;
  }

private:
  std::deque<Batch> items_;
  CRITICAL_SECTION cs_{};
  CONDITION_VARIABLE notEmpty_{};
  CONDITION_VARIABLE notFull_{};
  volatile LONG upstreamDone_ = 0;
  size_t capacity_ = 0;
  bool init_ = false;
  Invariants *inv_ = nullptr;
};

enum Mode { MODE_SINGLE, MODE_BATCH };

struct Config {
  Mode mode = MODE_BATCH;
  uint32_t requests = 500;
  uint32_t stages = 3;
  uint32_t workers = 4;
  uint32_t batchSize = 4;
  uint32_t capacity = 16;
  uint32_t seed = DEFAULT_SEED;
  uint32_t procMaxMs = 5;
  uint32_t pushTimeoutMs = 500;
  uint32_t barrierTimeoutMs = 5000;
  uint32_t maxRuntimeMs = 60000;
  bool quiet = false;
  std::string csvPath;
};

struct Context {
  Config cfg;
  Invariants inv;
  std::vector<Request> requests;
  std::vector<std::unique_ptr<BoundedQueue>> queues;
  volatile LONG completedTotal = 0;
  volatile LONG rejectedTotal = 0;
  volatile LONG pushTimeouts = 0;
};

struct StageState {
  size_t index = 0;
  Context *ctx = nullptr;

  CRITICAL_SECTION cs{};
  CONDITION_VARIABLE cvBatch{};
  SYNCHRONIZATION_BARRIER barrier{};
  bool barrierValid = false;
  Batch *current = nullptr;
  uint32_t currentRound = 0;
  volatile LONG claimCounter = 0;
  volatile LONG departCounter = 0;
  volatile LONG stopStage = 0;
  volatile LONG exited = 0;

  volatile LONGLONG busyUs = 0;
  volatile LONGLONG barrierWaitUs = 0;
  volatile LONG itemsProcessed = 0;
  volatile LONG batchesForwarded = 0;
};

void usage() {
  std::printf(
      "Lab4 variant %u: queuing system with synchronization barrier\n"
      "\n"
      "Usage: lab4.exe [options]\n"
      "  --mode single|batch   transfer mode (default: batch)\n"
      "  --requests N          number of requests (default: 500; individual: 865)\n"
      "  --stages N            pipeline stages, 1..%u (default: 3)\n"
      "  --workers N           worker threads per stage, 1..%u (default: 4)\n"
      "  --batch-size N        requests per batch in batch mode (default: 4)\n"
      "  --capacity N          bounded queue capacity in batches (default: 16)\n"
      "  --seed N              RNG seed (default: %u = 1000 + 73*97)\n"
      "  --proc-max-ms N       max processing time per request/stage (default: 5)\n"
      "  --push-timeout-ms N   push timeout before rejection (default: 500)\n"
      "  --barrier-timeout-ms N barrier wait timeout (default: 5000)\n"
      "  --max-runtime-ms N    watchdog limit for the whole run (default: 60000)\n"
      "  --quiet               disable verbose journal (errors still logged)\n"
      "  --csv PATH            append one summary row to CSV file PATH\n"
      "  --help                show this text\n",
      VARIANT_ID, MAX_STAGES, MAX_WORKERS, DEFAULT_SEED);
}

bool parseIntArg(int argc, char **argv, int &i, uint32_t &out) {
  if (i + 1 >= argc)
    return false;
  char *end = nullptr;
  unsigned long v = std::strtoul(argv[++i], &end, 10);
  if (!end || *end != '\0')
    return false;
  out = static_cast<uint32_t>(v);
  return true;
}

bool parseArgs(int argc, char **argv, Config &cfg) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage();
      std::exit(0);
    } else if (a == "--mode") {
      if (i + 1 >= argc)
        return false;
      std::string v = argv[++i];
      if (v == "single")
        cfg.mode = MODE_SINGLE;
      else if (v == "batch")
        cfg.mode = MODE_BATCH;
      else
        return false;
    } else if (a == "--requests") {
      if (!parseIntArg(argc, argv, i, cfg.requests)) return false;
    } else if (a == "--stages") {
      if (!parseIntArg(argc, argv, i, cfg.stages)) return false;
    } else if (a == "--workers") {
      if (!parseIntArg(argc, argv, i, cfg.workers)) return false;
    } else if (a == "--batch-size") {
      if (!parseIntArg(argc, argv, i, cfg.batchSize)) return false;
    } else if (a == "--capacity") {
      if (!parseIntArg(argc, argv, i, cfg.capacity)) return false;
    } else if (a == "--seed") {
      if (!parseIntArg(argc, argv, i, cfg.seed)) return false;
    } else if (a == "--proc-max-ms") {
      if (!parseIntArg(argc, argv, i, cfg.procMaxMs)) return false;
    } else if (a == "--push-timeout-ms") {
      if (!parseIntArg(argc, argv, i, cfg.pushTimeoutMs)) return false;
    } else if (a == "--barrier-timeout-ms") {
      if (!parseIntArg(argc, argv, i, cfg.barrierTimeoutMs)) return false;
    } else if (a == "--max-runtime-ms") {
      if (!parseIntArg(argc, argv, i, cfg.maxRuntimeMs)) return false;
    } else if (a == "--quiet") {
      cfg.quiet = true;
    } else if (a == "--csv") {
      if (i + 1 >= argc)
        return false;
      cfg.csvPath = argv[++i];
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", a.c_str());
      return false;
    }
  }
  if (cfg.requests < 1 || cfg.requests > 1000000 || cfg.stages < 1 || cfg.stages > MAX_STAGES ||
      cfg.workers < 1 || cfg.workers > MAX_WORKERS || cfg.batchSize < 1 ||
      cfg.batchSize > 1024 || cfg.capacity < 1 || cfg.capacity > 100000 || cfg.procMaxMs < 1 ||
      cfg.procMaxMs > 10000 || cfg.pushTimeoutMs < 50 || cfg.barrierTimeoutMs < 500 ||
      cfg.maxRuntimeMs < 1000) {
    std::fprintf(stderr, "Argument out of range\n");
    return false;
  }
  return true;
}

void rejectBatch(Context &ctx, const Batch &b, const char *role, const char *why) {
  for (uint32_t idx : b.items) {
    Request &req = ctx.requests[idx];
    if (InterlockedIncrement(&req.rejected) == 1)
      InterlockedIncrement(&ctx.rejectedTotal);
  }
  InterlockedIncrement(&ctx.pushTimeouts);
  logf(true, role, "REJECTED ITEMS=%zu REASON=%s", b.items.size(), why);
}

void processRequest(Context &ctx, StageState &st, uint32_t reqIdx, const char *role) {
  Request &req = ctx.requests[reqIdx];
  logf(false, role, "REQ=%u STAGE=%zu STATE=WORKING", req.id, st.index + 1);
  LONGLONG t0 = nowUs();
  busyWaitUs(req.planUs[st.index]);
  LONGLONG dt = nowUs() - t0;
  InterlockedExchangeAdd64(&req.procUs, dt);
  InterlockedExchangeAdd64(&st.busyUs, dt);

  if (InterlockedIncrement(&req.stageDone[st.index]) != 1)
    InterlockedIncrement(&ctx.inv.stageCounterBad);
  InterlockedIncrement(&st.itemsProcessed);
  logf(false, role, "REQ=%u STAGE=%zu STATE=DONE PROC_US=%lld", req.id, st.index + 1, dt);
}

void singleWorkerLoop(Context &ctx, StageState &st, const char *role) {
  BoundedQueue &qin = *ctx.queues[st.index];
  BoundedQueue &qout = *ctx.queues[st.index + 1];
  for (;;) {
    Batch b;
    if (qin.pop(b) != POP_OK)
      break;
    LONGLONG wait = nowUs() - b.readyAtUs;
    for (uint32_t idx : b.items) {
      InterlockedExchangeAdd64(&ctx.requests[idx].waitUs, wait);
      processRequest(ctx, st, idx, role);
    }
    if (qout.push(std::move(b), ctx.cfg.pushTimeoutMs)) {
      InterlockedIncrement(&st.batchesForwarded);
    } else {
      rejectBatch(ctx, b, role, "PUSH_TIMEOUT");
    }
  }
}

void batchWorkerLoop(Context &ctx, StageState &st, uint32_t workerIndex, const char *role) {
  BoundedQueue &qin = *ctx.queues[st.index];
  BoundedQueue &qout = *ctx.queues[st.index + 1];
  uint32_t myLastRound = 0;
  for (;;) {
    Batch *b = nullptr;
    uint32_t round = 0;
    if (workerIndex == 0) {
      EnterCriticalSection(&st.cs);
      while (!st.stopStage && st.current != nullptr)
        SleepConditionVariableCS(&st.cvBatch, &st.cs, 100);
      LeaveCriticalSection(&st.cs);
      Batch local;
      PopResult r = qin.pop(local);
      if (r != POP_OK) {
        EnterCriticalSection(&st.cs);
        st.stopStage = 1;
        WakeAllConditionVariable(&st.cvBatch);
        LeaveCriticalSection(&st.cs);
        break;
      }
      b = new Batch(std::move(local));
      EnterCriticalSection(&st.cs);
      st.current = b;
      st.currentRound++;
      round = st.currentRound;
      b->round = round;
      st.claimCounter = 0;
      st.departCounter = 0;
      WakeAllConditionVariable(&st.cvBatch);
      LeaveCriticalSection(&st.cs);
      logf(false, role, "BATCH=%u STAGE=%zu STATE=ACCEPTED ITEMS=%zu", round, st.index + 1,
           b->items.size());
    } else {
      EnterCriticalSection(&st.cs);
      while (!st.stopStage && (st.current == nullptr || st.currentRound == myLastRound))
        SleepConditionVariableCS(&st.cvBatch, &st.cs, 100);
      if (st.stopStage) {
        LeaveCriticalSection(&st.cs);
        break;
      }
      b = st.current;
      round = st.currentRound;
      LeaveCriticalSection(&st.cs);
    }
    myLastRound = round;

    LONG slot = InterlockedIncrement(&st.claimCounter) - 1;
    if (slot >= 0 && static_cast<size_t>(slot) < b->items.size()) {
      uint32_t idx = b->items[static_cast<size_t>(slot)];
      InterlockedExchangeAdd64(&ctx.requests[idx].waitUs, nowUs() - b->readyAtUs);
      processRequest(ctx, st, idx, role);
      InterlockedIncrement(&b->processed);
    }

    LONGLONG t0 = nowUs();
    EnterSynchronizationBarrier(&st.barrier, 0);
    InterlockedExchangeAdd64(&st.barrierWaitUs, nowUs() - t0);

    if (InterlockedIncrement(&st.departCounter) == static_cast<LONG>(ctx.cfg.workers)) {

      if (b->processed != static_cast<LONG>(b->items.size()))
        InterlockedIncrement(&ctx.inv.batchIncomplete);
      if (qout.push(std::move(*b), ctx.cfg.pushTimeoutMs)) {
        InterlockedIncrement(&st.batchesForwarded);
      } else {
        rejectBatch(ctx, *b, role, "PUSH_TIMEOUT");
      }
      EnterCriticalSection(&st.cs);
      st.current = nullptr;
      WakeAllConditionVariable(&st.cvBatch);
      LeaveCriticalSection(&st.cs);
      delete b;
    }
  }
}

struct WorkerArg {
  Context *ctx = nullptr;
  StageState *stage = nullptr;
  uint32_t workerIndex = 0;
  char role[24] = {};
};

DWORD WINAPI stageWorkerProc(LPVOID p) {
  WorkerArg *a = static_cast<WorkerArg *>(p);
  Context &ctx = *a->ctx;
  StageState &st = *a->stage;
  logf(false, a->role, "WORKER STG%zu-W%u | STATE=STARTED", st.index + 1, a->workerIndex);
  if (ctx.cfg.mode == MODE_SINGLE)
    singleWorkerLoop(ctx, st, a->role);
  else
    batchWorkerLoop(ctx, st, a->workerIndex, a->role);
  logf(false, a->role, "WORKER STG%zu-W%u | STATE=FINISHED", st.index + 1, a->workerIndex);

  if (InterlockedIncrement(&st.exited) == static_cast<LONG>(ctx.cfg.workers))
    ctx.queues[st.index + 1]->markUpstreamDone();
  return 0;
}

DWORD WINAPI generatorProc(LPVOID p) {
  Context &ctx = *static_cast<Context *>(p);
  const char *role = "GEN";
  Rng64 rng(ctx.cfg.seed);
  logf(false, role, "STATE=STARTED REQUESTS=%u MODE=%s SEED=%u", ctx.cfg.requests,
       ctx.cfg.mode == MODE_BATCH ? "batch" : "single", ctx.cfg.seed);
  std::vector<uint32_t> pending;
  pending.reserve(ctx.cfg.batchSize);
  auto flush = [&]() {
    if (pending.empty())
      return;
    Batch b;
    b.items.swap(pending);
    for (uint32_t idx : b.items)
      logf(false, role, "REQ=%u STATE=CREATED", ctx.requests[idx].id);
    size_t n = b.items.size();
    if (ctx.queues[0]->push(std::move(b), ctx.cfg.pushTimeoutMs)) {
      logf(false, role, "ENQUEUED ITEMS=%zu QUEUE=1", n);
    } else {
      rejectBatch(ctx, b, role, "PUSH_TIMEOUT");
    }
  };
  for (uint32_t i = 0; i < ctx.cfg.requests; ++i) {
    if (g_stop)
      break;
    Request &req = ctx.requests[i];
    req.id = i;
    req.createdAtUs = nowUs();
    for (size_t s = 0; s < ctx.cfg.stages; ++s)
      req.planUs[s] = static_cast<LONGLONG>(1 + rng.below(ctx.cfg.procMaxMs)) * 1000;
    pending.push_back(i);
    if (ctx.cfg.mode == MODE_SINGLE || pending.size() >= ctx.cfg.batchSize)
      flush();
  }
  if (ctx.cfg.mode == MODE_BATCH)
    flush();
  ctx.queues[0]->markUpstreamDone();
  logf(false, role, "STATE=FINISHED");
  return 0;
}

DWORD WINAPI collectorProc(LPVOID p) {
  Context &ctx = *static_cast<Context *>(p);
  const char *role = "COL";
  BoundedQueue &q = *ctx.queues[ctx.cfg.stages];
  logf(false, role, "STATE=STARTED");
  for (;;) {
    Batch b;
    if (q.pop(b) != POP_OK)
      break;
    for (uint32_t idx : b.items) {
      Request &req = ctx.requests[idx];

      bool full = true;
      for (size_t s = 0; s < ctx.cfg.stages; ++s)
        if (req.stageDone[s] != 1)
          full = false;
      if (!full) {
        InterlockedIncrement(&ctx.inv.stageCounterBad);
        logf(true, role, "REQ=%u INVARIANT=STAGE_PATH_BROKEN", req.id);
      }
      InterlockedIncrement(&req.collected);
      InterlockedIncrement(&ctx.completedTotal);
      logf(false, role, "REQ=%u STATE=COLLECTED WAIT_MS=%.2f PROC_MS=%.2f", req.id,
           req.waitUs / 1000.0, req.procUs / 1000.0);
    }
  }
  logf(false, role, "STATE=FINISHED COMPLETED=%ld", ctx.completedTotal);
  return 0;
}

double meanCollectedMs(const Context &ctx, bool wait) {
  LONGLONG sum = 0;
  LONG n = 0;
  for (const Request &r : ctx.requests) {
    if (r.collected) {
      sum += wait ? r.waitUs : r.procUs;
      ++n;
    }
  }
  return n ? static_cast<double>(sum) / n / 1000.0 : 0.0;
}

void printReport(const Context &ctx, const std::vector<StageState> &stages, LONGLONG wallUs,
                 bool timedOut) {
  const Config &cfg = ctx.cfg;
  LONGLONG barrierTotalUs = 0;
  for (const StageState &st : stages)
    barrierTotalUs += st.barrierWaitUs;

  std::printf("\n==== LAB4 VARIANT %u REPORT ====\n", VARIANT_ID);
  std::printf("STUDENT_ID=%u MODE=%s REQUESTS=%u STAGES=%u WORKERS=%u BATCH_SIZE=%u CAPACITY=%u "
              "SEED=%u PROC_MAX_MS=%u\n",
              STUDENT_ID, cfg.mode == MODE_BATCH ? "batch" : "single", cfg.requests, cfg.stages,
              cfg.workers, cfg.batchSize, cfg.capacity, cfg.seed, cfg.procMaxMs);
  std::printf("WALL_MS=%.1f%s\n", wallUs / 1000.0, timedOut ? " (TIMEOUT)" : "");
  std::printf("CREATED=%u COMPLETED=%ld REJECTED=%ld PUSH_TIMEOUTS=%ld\n", cfg.requests,
              ctx.completedTotal, ctx.rejectedTotal, ctx.pushTimeouts);
  std::printf("MEAN_WAIT_MS=%.2f MEAN_PROC_MS=%.2f\n", meanCollectedMs(ctx, true),
              meanCollectedMs(ctx, false));
  std::printf("STAGE ITEMS BATCHES BUSY_PCT PROC_TOTAL_MS BARRIER_WAIT_MS\n");
  for (const StageState &st : stages) {
    double busyPct =
        wallUs > 0 ? static_cast<double>(st.busyUs) * 100.0 / (wallUs * cfg.workers) : 0.0;
    std::printf("%5zu %5ld %7ld %8.1f %13.1f %15.1f\n", st.index + 1, st.itemsProcessed,
                st.batchesForwarded, busyPct, st.busyUs / 1000.0, st.barrierWaitUs / 1000.0);
  }
  std::printf("BARRIER_WAIT_TOTAL_MS=%.1f\n", barrierTotalUs / 1000.0);
  std::printf("INVARIANTS queueOverflow=%ld batchIncomplete=%ld stageCounterBad=%ld "
              "balanceBad=%ld residualBad=%ld barrierTimeouts=%ld\n",
              ctx.inv.queueOverflow, ctx.inv.batchIncomplete, ctx.inv.stageCounterBad,
              ctx.inv.balanceBad, ctx.inv.residualBad, ctx.inv.barrierTimeouts);
  std::printf("VIOLATIONS_TOTAL=%ld\n", ctx.inv.total());
}

void writeCsvRow(const std::string &path, const Context &ctx,
                 const std::vector<StageState> &stages, LONGLONG wallUs) {
  FILE *f = std::fopen(path.c_str(), "a");
  if (!f) {
    std::fprintf(stderr, "Cannot open CSV file %s\n", path.c_str());
    return;
  }
  LONGLONG barrierTotalUs = 0;
  for (const StageState &st : stages)
    barrierTotalUs += st.barrierWaitUs;
  if (std::ftell(f) == 0)
    std::fprintf(f, "mode,requests,stages,workers,batch_size,capacity,seed,proc_max_ms,wall_ms,"
                    "completed,rejected,mean_wait_ms,mean_proc_ms,barrier_wait_total_ms,"
                    "violations\n");
  std::fprintf(f, "%s,%u,%u,%u,%u,%u,%u,%u,%.1f,%ld,%ld,%.2f,%.2f,%.1f,%ld\n",
               ctx.cfg.mode == MODE_BATCH ? "batch" : "single", ctx.cfg.requests, ctx.cfg.stages,
               ctx.cfg.workers, ctx.cfg.batchSize, ctx.cfg.capacity, ctx.cfg.seed,
               ctx.cfg.procMaxMs, wallUs / 1000.0, ctx.completedTotal, ctx.rejectedTotal,
               meanCollectedMs(ctx, true), meanCollectedMs(ctx, false), barrierTotalUs / 1000.0,
               ctx.inv.total());
  std::fclose(f);
}

}

int main(int argc, char **argv) {
  QueryPerformanceFrequency(&g_qpcFreq);
  QueryPerformanceCounter(&g_qpcStart);
  InitializeCriticalSection(&g_logCs);

  Config cfg;
  if (!parseArgs(argc, argv, cfg)) {
    usage();
    return 2;
  }
  g_verbose = !cfg.quiet;

  static Context ctx;
  ctx.cfg = cfg;
  ctx.requests.resize(cfg.requests);
  for (size_t q = 0; q <= cfg.stages; ++q) {
    auto queue = std::make_unique<BoundedQueue>();
    if (!queue->init(cfg.capacity, &ctx.inv)) {
      printWinError("queue init");
      return 2;
    }
    ctx.queues.push_back(std::move(queue));
  }

  std::vector<StageState> stages(cfg.stages);
  for (size_t i = 0; i < cfg.stages; ++i) {
    stages[i].index = i;
    stages[i].ctx = &ctx;
    if (cfg.mode == MODE_BATCH) {
      InitializeCriticalSection(&stages[i].cs);
      InitializeConditionVariable(&stages[i].cvBatch);
      if (!InitializeSynchronizationBarrier(&stages[i].barrier, static_cast<LONG>(cfg.workers),
                                            -1)) {
        printWinError("InitializeSynchronizationBarrier");
        return 2;
      }
      stages[i].barrierValid = true;
    }
  }

  std::vector<WorkerArg> args;
  args.reserve(cfg.stages * cfg.workers);
  std::vector<HANDLE> threads;
  bool createFailed = false;

  auto spawn = [&](LPTHREAD_START_ROUTINE proc, LPVOID param, const char *what) {
    if (createFailed)
      return;
    HANDLE h = CreateThread(nullptr, 0, proc, param, 0, nullptr);
    if (!h) {
      printWinError(what);
      createFailed = true;
      return;
    }
    threads.push_back(h);
  };

  for (size_t i = 0; i < cfg.stages && !createFailed; ++i) {
    for (uint32_t w = 0; w < cfg.workers; ++w) {
      args.emplace_back();
      WorkerArg &a = args.back();
      a.ctx = &ctx;
      a.stage = &stages[i];
      a.workerIndex = w;
      snprintf(a.role, sizeof(a.role), "STG%zu-W%u", i + 1, w);
      spawn(stageWorkerProc, &a, "CreateThread(worker)");
    }
  }
  spawn(collectorProc, &ctx, "CreateThread(collector)");
  spawn(generatorProc, &ctx, "CreateThread(generator)");

  if (createFailed)
    InterlockedExchange(&g_stop, 1);

  LONGLONG deadline = nowUs() + static_cast<LONGLONG>(cfg.maxRuntimeMs) * 1000;
  bool timedOut = false;
  for (HANDLE h : threads) {
    LONGLONG rem = deadline - nowUs();
    if (rem <= 0) {
      timedOut = true;
      break;
    }
    DWORD wr = WaitForSingleObject(h, static_cast<DWORD>(rem / 1000));
    if (wr != WAIT_OBJECT_0) {
      timedOut = true;
      break;
    }
  }
  if (timedOut) {
    logf(true, "MAIN", "MAX RUNTIME EXCEEDED, forcing shutdown");
    InterlockedExchange(&g_stop, 1);
    for (auto &q : ctx.queues)
      q->wakeAll();
    for (HANDLE h : threads)
      WaitForSingleObject(h, 10000);
  }
  LONGLONG wallUs = nowUs();

  for (size_t q = 0; q <= cfg.stages; ++q)
    if (ctx.queues[q]->size() != 0)
      InterlockedIncrement(&ctx.inv.residualBad);
  for (size_t i = 0; i < cfg.stages; ++i)
    if (stages[i].current != nullptr)
      InterlockedIncrement(&ctx.inv.residualBad);

  if (!timedOut && !createFailed &&
      ctx.completedTotal + ctx.rejectedTotal != static_cast<LONG>(cfg.requests))
    InterlockedIncrement(&ctx.inv.balanceBad);

  printReport(ctx, stages, wallUs, timedOut);
  if (!cfg.csvPath.empty())
    writeCsvRow(cfg.csvPath, ctx, stages, wallUs);

  for (HANDLE h : threads)
    CloseHandle(h);
  for (StageState &st : stages) {
    if (st.barrierValid)
      DeleteSynchronizationBarrier(&st.barrier);
    if (cfg.mode == MODE_BATCH)
      DeleteCriticalSection(&st.cs);
  }
  ctx.queues.clear();
  DeleteCriticalSection(&g_logCs);

  if (timedOut || createFailed)
    return 3;
  return ctx.inv.total() == 0 ? 0 : 1;
}

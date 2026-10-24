#define _WIN32_WINNT 0x0602
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>

static SYNCHRONIZATION_BARRIER g_bar;
static volatile LONG g_round = 0;
static volatile LONG g_arrived[4] = {};
static volatile LONG g_trueCount = 0;
static volatile LONG g_violations = 0;
static const int ROUNDS = 2000;

static DWORD WINAPI worker(LPVOID p) {
  int idx = static_cast<int>(reinterpret_cast<intptr_t>(p));
  for (int round = 0; round < ROUNDS; ++round) {
    InterlockedExchange(&g_arrived[idx], round + 1);
    BOOL ok = EnterSynchronizationBarrier(&g_bar, 0);
    if (ok)
      InterlockedIncrement(&g_trueCount);
    for (int i = 0; i < 4; ++i) {
      if (InterlockedCompareExchange(&g_arrived[i], 0, 0) < round + 1)
        InterlockedIncrement(&g_violations);
    }
    if (round == 0)
      std::printf("thread %d round 0 ok=%d err=%lu\n", idx, ok, GetLastError());
  }
  return 0;
}

int main(int argc, char **argv) {
  LONG spin = -1;
  if (argc > 1)
    spin = std::atoi(argv[1]);
  if (!InitializeSynchronizationBarrier(&g_bar, 4, spin)) {
    std::printf("init failed err=%lu spin=%ld\n", GetLastError(), (long)spin);
    return 1;
  }
  std::printf("init ok spin=%ld\n", (long)spin);
  HANDLE h[4];
  for (int i = 0; i < 4; ++i)
    h[i] = CreateThread(nullptr, 0, worker, reinterpret_cast<LPVOID>(static_cast<intptr_t>(i)), 0,
                        nullptr);
  DWORD wr = WaitForMultipleObjects(4, h, TRUE, 60000);
  std::printf("wait=%lu (0=ok) rounds=%d trueCount=%ld (expect %d) violations=%ld\n", wr, ROUNDS,
              g_trueCount, 4 * ROUNDS, g_violations);
  DeleteSynchronizationBarrier(&g_bar);
  return 0;
}

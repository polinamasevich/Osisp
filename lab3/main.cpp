#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

constexpr uint32_t VARIANT_ID = 19;
constexpr uint32_t STUDENT_ID = 73;
constexpr size_t BUFFER_COUNT = 8;
constexpr size_t MAX_PAYLOAD = 4096;
constexpr uint32_t DATA_MESSAGE = 1;
constexpr uint32_t SHARED_MAGIC = 0x4F534950;

enum BufferState : LONG { FREE, WRITING, READY, READING };

struct Buffer {
  LONG state;
  uint32_t version;
  uint32_t messageType;
  uint32_t sourcePid;
  uint32_t requestId;
  uint32_t payloadSize;
  uint32_t checksum;
  unsigned char payload[MAX_PAYLOAD];
};

struct SharedData {
  uint32_t magic;
  uint32_t messageCount;
  uint32_t payloadSize;
  uint32_t studentId;
  uint32_t crashConsumerAfter;
  volatile LONG producerDone;
  Buffer buffers[BUFFER_COUNT];
};

class Handle {
public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() {
    reset();
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&other) noexcept : value_(other.release()) {}

  HANDLE get() const {
    return value_;
  }
  explicit operator bool() const {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  HANDLE release() {
    HANDLE value = value_;
    value_ = nullptr;
    return value;
  }
  void reset(HANDLE value = nullptr) {
    if (*this)
      CloseHandle(value_);
    value_ = value;
  }

private:
  HANDLE value_ = nullptr;
};

void printWinError(const char *function) {
  DWORD code = GetLastError();
  char text[512]{};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text,
                 sizeof(text), nullptr);
  std::cerr << "ERROR " << function << " GetLastError=" << code << ": " << text << '\n';
}

std::wstring widenAscii(const char *text) {
  std::wstring result;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
    if (*p > 0x7F)
      return {};
    result.push_back(static_cast<wchar_t>(*p));
  }
  return result;
}

bool validRunId(const std::wstring &runId) {
  return !runId.empty() && runId.size() <= 48 &&
         std::all_of(runId.begin(), runId.end(), [](wchar_t c) {
           return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
         });
}

std::wstring objectName(const std::wstring &runId, const wchar_t *suffix) {
  return L"Local\\OSISP3_" + runId + suffix;
}

uint32_t hashPayload(const unsigned char *data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 16777619u;
  return hash;
}

void logEvent(const std::wstring &runId, const char *role, const char *event, uint32_t requestId,
              uint32_t size) {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  std::cout << '[' << time.wHour << ':' << time.wMinute << ':' << time.wSecond << '.'
            << time.wMilliseconds << "] " << role << " PID=" << GetCurrentProcessId()
            << " VARIANT=" << VARIANT_ID << " RUN_ID=";
  std::wcout << runId;
  std::cout << ' ' << event << " requestId=" << requestId << " size=" << size << '\n';
}

bool lockMutex(HANDLE mutex) {
  DWORD result = WaitForSingleObject(mutex, INFINITE);
  if (result == WAIT_OBJECT_0)
    return true;
  if (result == WAIT_ABANDONED) {
    std::cerr << "ERROR Mutex owner terminated; shared state may be inconsistent\n";
    ReleaseMutex(mutex);
    return false;
  }
  printWinError("WaitForSingleObject(mutex)");
  return false;
}

void unlockMutex(HANDLE mutex) {
  if (!ReleaseMutex(mutex))
    printWinError("ReleaseMutex");
}

int findBuffer(const SharedData *shared, BufferState state) {
  for (size_t i = 0; i < BUFFER_COUNT; ++i)
    if (shared->buffers[i].state == state)
      return static_cast<int>(i);
  return -1;
}

bool openChildObjects(const std::wstring &runId, Handle &mapping, Handle &mutex, Handle &freeSlots,
                      Handle &readySlots, Handle &doneEvent) {
  mapping.reset(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, objectName(runId, L"_map").c_str()));
  mutex.reset(
    OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, objectName(runId, L"_mutex").c_str()));
  freeSlots.reset(OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE,
                                 objectName(runId, L"_free").c_str()));
  readySlots.reset(OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE,
                                  objectName(runId, L"_ready").c_str()));
  doneEvent.reset(
    OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, objectName(runId, L"_done").c_str()));
  if (mapping && mutex && freeSlots && readySlots && doneEvent)
    return true;
  printWinError("Open named IPC object");
  return false;
}

bool runProducer(const std::wstring &runId, SharedData *shared, HANDLE mutex, HANDLE freeSlots,
                 HANDLE readySlots, HANDLE doneEvent) {
  std::vector<unsigned char> payload(shared->payloadSize);
  for (uint32_t id = 1; id <= shared->messageCount; ++id) {
    if (WaitForSingleObject(freeSlots, INFINITE) != WAIT_OBJECT_0) {
      printWinError("WaitForSingleObject(free semaphore)");
      return false;
    }
    if (!lockMutex(mutex))
      return false;
    int index = findBuffer(shared, FREE);
    if (index >= 0)
      shared->buffers[index].state = WRITING;
    unlockMutex(mutex);
    if (index < 0) {
      std::cerr << "ERROR free semaphore has no FREE buffer\n";
      return false;
    }

    Buffer &buffer = shared->buffers[index];
    for (size_t j = 0; j < payload.size(); ++j)
      payload[j] = static_cast<unsigned char>((j + id + STUDENT_ID * 31u) & 0xFF);
    buffer.version = 1;
    buffer.messageType = DATA_MESSAGE;
    buffer.sourcePid = GetCurrentProcessId();
    buffer.requestId = id;
    buffer.payloadSize = static_cast<uint32_t>(payload.size());
    std::memcpy(buffer.payload, payload.data(), payload.size());
    buffer.checksum = hashPayload(payload.data(), payload.size());

    if (!lockMutex(mutex))
      return false;
    buffer.state = READY;
    unlockMutex(mutex);
    if (!ReleaseSemaphore(readySlots, 1, nullptr)) {
      printWinError("ReleaseSemaphore(ready)");
      return false;
    }
    if (shared->messageCount <= 20 || id == 1 || id == shared->messageCount || id % 100 == 0)
      logEvent(runId, "PRODUCER", "PUBLISH", id, buffer.payloadSize);
  }

  if (!lockMutex(mutex))
    return false;
  shared->producerDone = 1;
  unlockMutex(mutex);
  if (!SetEvent(doneEvent)) {
    printWinError("SetEvent(producer done)");
    return false;
  }
  logEvent(runId, "PRODUCER", "DONE", shared->messageCount, shared->payloadSize);
  return true;
}

bool runConsumer(const std::wstring &runId, SharedData *shared, HANDLE mutex, HANDLE freeSlots,
                 HANDLE readySlots, HANDLE doneEvent) {
  uint32_t received = 0;
  HANDLE waits[] = {readySlots, doneEvent};
  while (received < shared->messageCount) {
    DWORD result = WaitForMultipleObjects(2, waits, FALSE, 10000);
    if (result == WAIT_OBJECT_0) {
      if (!lockMutex(mutex))
        return false;
      int index = findBuffer(shared, READY);
      if (index >= 0)
        shared->buffers[index].state = READING;
      unlockMutex(mutex);
      if (index < 0) {
        std::cerr << "ERROR ready semaphore has no READY buffer\n";
        return false;
      }

      Buffer &buffer = shared->buffers[index];
      bool valid = buffer.version == 1 && buffer.messageType == DATA_MESSAGE &&
                   buffer.sourcePid != 0 && buffer.payloadSize == shared->payloadSize &&
                   buffer.payloadSize <= MAX_PAYLOAD &&
                   buffer.checksum == hashPayload(buffer.payload, buffer.payloadSize);
      if (!valid)
        std::cerr << "ERROR invalid message requestId=" << buffer.requestId << '\n';
      else {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < buffer.payloadSize; ++i)
          sum += buffer.payload[i];
        (void)sum;
        ++received;
        if (shared->messageCount <= 20 || received == 1 || received == shared->messageCount ||
            received % 100 == 0)
          logEvent(runId, "CONSUMER", "PROCESS", buffer.requestId, buffer.payloadSize);
        if (shared->crashConsumerAfter && received == shared->crashConsumerAfter) {
          std::cerr << "DEMO: Consumer exits abnormally after " << received << " messages\n";
          ExitProcess(99);
        }
      }

      if (!lockMutex(mutex))
        return false;
      buffer.state = FREE;
      unlockMutex(mutex);
      if (!ReleaseSemaphore(freeSlots, 1, nullptr)) {
        printWinError("ReleaseSemaphore(free)");
        return false;
      }
      if (!valid)
        return false;
    } else if (result == WAIT_OBJECT_0 + 1) {
      if (!lockMutex(mutex))
        return false;
      bool pending = findBuffer(shared, READY) >= 0 || findBuffer(shared, WRITING) >= 0 ||
                     findBuffer(shared, READING) >= 0;
      bool producerDone = shared->producerDone != 0;
      unlockMutex(mutex);
      if (!producerDone || pending || received != shared->messageCount) {
        std::cerr << "ERROR producer ended before all buffers were processed\n";
        return false;
      }
      break;
    } else if (result == WAIT_TIMEOUT) {
      std::cerr << "ERROR no data received for 10 seconds; producer may have failed\n";
      return false;
    } else {
      printWinError("WaitForMultipleObjects(consumer)");
      return false;
    }
  }
  return true;
}

int childMain(int argc, char **argv) {
  if (argc != 9) {
    std::cerr << "ERROR invalid child arguments\n";
    return 2;
  }
  std::wstring role = widenAscii(argv[2]);
  std::wstring runId = widenAscii(argv[3]);
  if ((role != L"producer" && role != L"consumer") || !validRunId(runId)) {
    std::cerr << "ERROR invalid role or RUN_ID\n";
    return 2;
  }
  uint32_t count = std::strtoul(argv[4], nullptr, 10);
  uint32_t size = std::strtoul(argv[5], nullptr, 10);
  uint32_t crashAfter = std::strtoul(argv[6], nullptr, 10);
  uint32_t student = std::strtoul(argv[7], nullptr, 10);
  uint32_t variant = std::strtoul(argv[8], nullptr, 10);
  if (!count || !size || size > MAX_PAYLOAD || student != STUDENT_ID || variant != VARIANT_ID) {
    std::cerr << "ERROR child parameters do not match shared configuration\n";
    return 2;
  }

  Handle mapping, mutex, freeSlots, readySlots, doneEvent;
  if (!openChildObjects(runId, mapping, mutex, freeSlots, readySlots, doneEvent))
    return 1;
  SharedData *shared = static_cast<SharedData *>(
    MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedData)));
  if (!shared) {
    printWinError("MapViewOfFile(child)");
    return 1;
  }
  bool headerOk = shared->magic == SHARED_MAGIC && shared->messageCount == count &&
                  shared->payloadSize == size && shared->studentId == student &&
                  shared->crashConsumerAfter == crashAfter;
  if (!headerOk) {
    std::cerr << "ERROR shared memory header mismatch\n";
    UnmapViewOfFile(shared);
    return 1;
  }

  logEvent(runId, role == L"producer" ? "PRODUCER" : "CONSUMER", "START", 0, size);
  bool ok =
    role == L"producer"
      ? runProducer(runId, shared, mutex.get(), freeSlots.get(), readySlots.get(), doneEvent.get())
      : runConsumer(runId, shared, mutex.get(), freeSlots.get(), readySlots.get(), doneEvent.get());
  UnmapViewOfFile(shared);
  return ok ? 0 : 1;
}

bool createChild(const std::wstring &exe, const std::wstring &role, const std::wstring &runId,
                 uint32_t count, uint32_t size, uint32_t crashAfter, PROCESS_INFORMATION &pi) {
  std::wstring command = L"\"" + exe + L"\" --child " + role + L" " + runId + L" " +
                         std::to_wstring(count) + L" " + std::to_wstring(size) + L" " +
                         std::to_wstring(crashAfter) + L" " + std::to_wstring(STUDENT_ID) + L" " +
                         std::to_wstring(VARIANT_ID);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                      &startup, &pi)) {
    printWinError("CreateProcessW");
    return false;
  }
  CloseHandle(pi.hThread);
  return true;
}

void stopAbnormalChild(HANDLE process) {
  if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
    TerminateProcess(process, ERROR_PROCESS_ABORTED);
    WaitForSingleObject(process, 5000);
  }
}

bool runTest(const std::wstring &exe, const std::wstring &runId, uint32_t size, uint32_t crashAfter,
             uint32_t count) {
  std::wstring fullRunId = runId + (size == 64 ? L"_small" : L"_large");
  auto name = [&](const wchar_t *suffix) { return objectName(fullRunId, suffix); };

  SetLastError(ERROR_SUCCESS);
  Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    sizeof(SharedData), name(L"_map").c_str()));
  if (!mapping) {
    printWinError("CreateFileMappingW");
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    std::cerr << "ERROR RUN_ID is already in use; choose another RUN_ID\n";
    return false;
  }

  Handle mutex(CreateMutexW(nullptr, FALSE, name(L"_mutex").c_str()));
  Handle freeSlots(CreateSemaphoreW(nullptr, BUFFER_COUNT, BUFFER_COUNT, name(L"_free").c_str()));
  Handle readySlots(CreateSemaphoreW(nullptr, 0, BUFFER_COUNT, name(L"_ready").c_str()));
  Handle doneEvent(CreateEventW(nullptr, TRUE, FALSE, name(L"_done").c_str()));
  if (!mutex || !freeSlots || !readySlots || !doneEvent) {
    printWinError("Create synchronization objects");
    return false;
  }
  SharedData *shared = static_cast<SharedData *>(
    MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedData)));
  if (!shared) {
    printWinError("MapViewOfFile(supervisor)");
    return false;
  }
  std::memset(shared, 0, sizeof(*shared));
  shared->magic = SHARED_MAGIC;
  shared->messageCount = count;
  shared->payloadSize = size;
  shared->studentId = STUDENT_ID;
  shared->crashConsumerAfter = crashAfter;
  for (Buffer &buffer : shared->buffers)
    buffer.state = FREE;

  std::wcout << L"RUN_ID=" << fullRunId << L" VARIANT=" << VARIANT_ID << L" STUDENT_ID="
             << STUDENT_ID << L" count=" << count << L" payload=" << size << L" bytes\n";
  auto start = std::chrono::steady_clock::now();
  PROCESS_INFORMATION producer{}, consumer{};
  bool producerStarted =
    createChild(exe, L"producer", fullRunId, count, size, crashAfter, producer);
  bool consumerStarted =
    producerStarted && createChild(exe, L"consumer", fullRunId, count, size, crashAfter, consumer);
  bool ok = producerStarted && consumerStarted;
  bool producerDone = false, consumerDone = false;
  auto deadline = start + std::chrono::seconds(120);
  while (ok && (!producerDone || !consumerDone)) {
    HANDLE processes[2]{};
    bool processIsProducer[2]{};
    DWORD processCount = 0;
    if (!producerDone) {
      processes[processCount] = producer.hProcess;
      processIsProducer[processCount++] = true;
    }
    if (!consumerDone) {
      processes[processCount] = consumer.hProcess;
      processIsProducer[processCount++] = false;
    }
    DWORD result = WaitForMultipleObjects(processCount, processes, FALSE, 100);
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + processCount) {
      bool producerExited = processIsProducer[result - WAIT_OBJECT_0];
      HANDLE process = processes[result - WAIT_OBJECT_0];
      DWORD code = 1;
      GetExitCodeProcess(process, &code);
      if (producerExited) {
        producerDone = true;
        if (code != 0) {
          std::cerr << "ERROR Producer exited with code " << code << '\n';
          ok = false;
        }
      } else {
        consumerDone = true;
        if (code != 0) {
          std::cerr << "ERROR Consumer exited with code " << code << '\n';
          ok = false;
        }
      }
    } else if (result == WAIT_TIMEOUT) {
    } else {
      printWinError("WaitForMultipleObjects(supervisor)");
      ok = false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "ERROR process timeout after 120 seconds\n";
      ok = false;
    }
  }
  if (!producerDone)
    stopAbnormalChild(producer.hProcess);
  if (consumerStarted && !consumerDone)
    stopAbnormalChild(consumer.hProcess);

  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::wcout << L"RESULT RUN_ID=" << fullRunId << L" blocks=" << count << L" time=" << seconds
             << L" s rate=" << count / seconds << L" blocks/s status=" << (ok ? L"OK" : L"ERROR")
             << L'\n';
  if (producerStarted)
    CloseHandle(producer.hProcess);
  if (consumerStarted)
    CloseHandle(consumer.hProcess);
  UnmapViewOfFile(shared);
  return ok;
}

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--child")
    return childMain(argc, argv);

  std::wstring runId = L"ST73";
  uint32_t crashAfter = 0;
  uint32_t count = 1000 + STUDENT_ID;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--run" && i + 1 < argc)
      runId = widenAscii(argv[++i]);
    else if (arg == "--crash-consumer-after" && i + 1 < argc)
      crashAfter = std::strtoul(argv[++i], nullptr, 10);
    else if (arg == "--messages" && i + 1 < argc)
      count = std::strtoul(argv[++i], nullptr, 10);
    else {
      std::cerr << "Usage: lab3.exe [--run RUN_ID] [--messages N] "
                   "[--crash-consumer-after N]\n";
      return 2;
    }
  }
  if (!validRunId(runId) || count == 0 || count > 100000 || crashAfter > count) {
    std::cerr << "ERROR invalid RUN_ID or crash position\n";
    return 2;
  }
  logEvent(runId, "SUPERVISOR", "START", 0, 0);
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring exe = path;
  bool smallOk = runTest(exe, runId, 64, crashAfter, count);
  bool largeOk = runTest(exe, runId, 4096, crashAfter, count);
  return smallOk && largeOk ? 0 : 1;
}

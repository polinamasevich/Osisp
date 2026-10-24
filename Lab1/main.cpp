#include <windows.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdint>

using namespace std;

struct ThreadData
{
    int number;
    uint64_t startOffset;
    uint64_t bytesToRead;
    uint64_t checksum;
    DWORD threadId;
    bool success;
};

void printWindowsError(const string& operation)
{
    DWORD errorCode = GetLastError();

    LPSTR messageBuffer = nullptr;

    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        (LPSTR)&messageBuffer,
        0,
        nullptr
    );

    cout << "\n[ОШИБКА]\n";
    cout << "Операция: " << operation << '\n';
    cout << "Код GetLastError(): " << errorCode << '\n';

    if (size > 0 && messageBuffer != nullptr)
    {
        cout << "Описание: " << messageBuffer;
        LocalFree(messageBuffer);
    }
    else
    {
        cout << "Описание ошибки недоступно.\n";
    }
}

double getTimeMilliseconds(LARGE_INTEGER start, LARGE_INTEGER end,
                            LARGE_INTEGER frequency)
{
    return static_cast<double>(end.QuadPart - start.QuadPart)
           * 1000.0
           / static_cast<double>(frequency.QuadPart);
}

bool singleThreadRead(
    const wchar_t* fileName,
    uint64_t fileSize,
    uint64_t& checksum,
    double& elapsedTime)
{
    HANDLE hFile = CreateFileW(
        fileName,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        printWindowsError("CreateFileW (однопоточное чтение)");
        return false;
    }

    const DWORD BUFFER_SIZE = 1024 * 1024;
    vector<unsigned char> buffer(BUFFER_SIZE);

    checksum = 0;

    LARGE_INTEGER frequency;
    LARGE_INTEGER startTime;
    LARGE_INTEGER endTime;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&startTime);

    uint64_t remaining = fileSize;

    while (remaining > 0)
    {
        DWORD bytesToRead = static_cast<DWORD>(
            min<uint64_t>(remaining, BUFFER_SIZE)
        );

        DWORD bytesRead = 0;

        BOOL result = ReadFile(
            hFile,
            buffer.data(),
            bytesToRead,
            &bytesRead,
            nullptr
        );

        if (!result)
        {
            printWindowsError("ReadFile (однопоточное чтение)");
            CloseHandle(hFile);
            return false;
        }

        if (bytesRead == 0)
        {
            cout << "\n[ОШИБКА] Неожиданный конец файла.\n";
            CloseHandle(hFile);
            return false;
        }

        for (DWORD i = 0; i < bytesRead; ++i)
        {
            checksum += buffer[i];
        }

        remaining -= bytesRead;
    }

    QueryPerformanceCounter(&endTime);

    elapsedTime = getTimeMilliseconds(
        startTime,
        endTime,
        frequency
    );

    CloseHandle(hFile);

    return true;
}

DWORD WINAPI workerThread(LPVOID parameter)
{
    ThreadData* data = static_cast<ThreadData*>(parameter);

    data->threadId = GetCurrentThreadId();
    data->checksum = 0;
    data->success = false;

    HANDLE hFile = CreateFileW(
        L"test.bin",
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        printWindowsError("CreateFileW (рабочий поток)");
        return 1;
    }

    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(data->startOffset);

    if (!SetFilePointerEx(
        hFile,
        position,
        nullptr,
        FILE_BEGIN))
    {
        printWindowsError("SetFilePointerEx");
        CloseHandle(hFile);
        return 1;
    }

    const DWORD BUFFER_SIZE = 1024 * 1024;
    vector<unsigned char> buffer(BUFFER_SIZE);

    uint64_t remaining = data->bytesToRead;

    while (remaining > 0)
    {
        DWORD bytesToRead = static_cast<DWORD>(
            min<uint64_t>(remaining, BUFFER_SIZE)
        );

        DWORD bytesRead = 0;

        BOOL result = ReadFile(
            hFile,
            buffer.data(),
            bytesToRead,
            &bytesRead,
            nullptr
        );

        if (!result)
        {
            printWindowsError("ReadFile (рабочий поток)");
            CloseHandle(hFile);
            return 1;
        }

        if (bytesRead == 0)
        {
            cout << "\n[ОШИБКА] Поток №"
                 << data->number
                 << " достиг неожиданного конца файла.\n";

            CloseHandle(hFile);
            return 1;
        }

        for (DWORD i = 0; i < bytesRead; ++i)
        {
            data->checksum += buffer[i];
        }

        remaining -= bytesRead;
    }

    data->success = true;

    CloseHandle(hFile);

    return 0;
}

bool multiThreadRead(
    const wchar_t* fileName,
    uint64_t fileSize,
    int threadCount,
    uint64_t& totalChecksum,
    double& elapsedTime)
{
    vector<HANDLE> threads(threadCount);
    vector<ThreadData> threadData(threadCount);

    uint64_t baseSize = fileSize / threadCount;
    uint64_t remainder = fileSize % threadCount;

    uint64_t currentOffset = 0;

    for (int i = 0; i < threadCount; ++i)
    {
        threadData[i].number = i + 1;
        threadData[i].startOffset = currentOffset;

        threadData[i].bytesToRead =
            baseSize + (i < static_cast<int>(remainder) ? 1 : 0);

        threadData[i].checksum = 0;
        threadData[i].threadId = 0;
        threadData[i].success = false;

        currentOffset += threadData[i].bytesToRead;

        threads[i] = CreateThread(
            nullptr,
            0,
            workerThread,
            &threadData[i],
            0,
            nullptr
        );

        if (threads[i] == nullptr)
        {
            printWindowsError("CreateThread");

            if (i > 0)
            {
                WaitForMultipleObjects(
                    i,
                    threads.data(),
                    TRUE,
                    INFINITE
                );

                for (int j = 0; j < i; ++j)
                {
                    CloseHandle(threads[j]);
                }
            }

            return false;
        }
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER startTime;
    LARGE_INTEGER endTime;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&startTime);

    DWORD waitResult = WaitForMultipleObjects(
        threadCount,
        threads.data(),
        TRUE,
        INFINITE
    );

    QueryPerformanceCounter(&endTime);

    if (waitResult == WAIT_FAILED)
    {
        printWindowsError("WaitForMultipleObjects");

        for (HANDLE thread : threads)
        {
            CloseHandle(thread);
        }

        return false;
    }

    elapsedTime = getTimeMilliseconds(
        startTime,
        endTime,
        frequency
    );

    totalChecksum = 0;

    cout << "\nИнформация о рабочих потоках:\n";

    for (int i = 0; i < threadCount; ++i)
    {
        DWORD exitCode = 0;

        if (!GetExitCodeThread(threads[i], &exitCode))
        {
            printWindowsError("GetExitCodeThread");
        }

        cout << "Поток №" << threadData[i].number
             << " | TID: " << threadData[i].threadId
             << " | Состояние: ";

        if (threadData[i].success &&
            exitCode == 0)
        {
            cout << "завершён";
        }
        else
        {
            cout << "ошибка";
        }

        cout << " | Код завершения: " << exitCode << '\n';

        cout << "  Начало диапазона: "
             << threadData[i].startOffset << '\n';

        cout << "  Размер диапазона: "
             << threadData[i].bytesToRead << " байт\n";

        cout << "  Контрольная сумма: "
             << threadData[i].checksum << "\n\n";

        if (!threadData[i].success || exitCode != 0)
        {
            for (HANDLE thread : threads)
            {
                CloseHandle(thread);
            }

            return false;
        }

        totalChecksum += threadData[i].checksum;
    }

    for (HANDLE thread : threads)
    {
        CloseHandle(thread);
    }

    return true;
}

void runExperiment(
    const wchar_t* fileName,
    uint64_t fileSize,
    int threadCount,
    uint64_t referenceChecksum)
{
    const int RUNS = 3;

    vector<double> times;
    times.reserve(RUNS);

    cout << "Эксперимент: K = " << threadCount << '\n';

    for (int run = 1; run <= RUNS; ++run)
    {
        uint64_t checksum = 0;
        double elapsedTime = 0;

        cout << "\nЗапуск №" << run << '\n';

        bool success = multiThreadRead(
            fileName,
            fileSize,
            threadCount,
            checksum,
            elapsedTime
        );

        if (!success)
        {
            cout << "Эксперимент завершился с ошибкой.\n";
            return;
        }

        cout << fixed << setprecision(3);
        cout << "\nВремя: " << elapsedTime << " мс\n";
        cout << "Общая контрольная сумма: " << checksum << '\n';

        if (checksum == referenceChecksum)
        {
            cout << "Проверка результата: OK\n";
        }
        else
        {
            cout << "Проверка результата: ОШИБКА!\n";
            cout << "Ожидалось: " << referenceChecksum << '\n';
        }

        times.push_back(elapsedTime);
    }

    double average = 0;

    for (double time : times)
    {
        average += time;
    }

    average /= times.size();

    cout << "Среднее время для K = "
         << threadCount
         << ": "
         << fixed
         << setprecision(3)
         << average
         << " мс\n";
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const wchar_t* fileName = L"test.bin";

    HANDLE hFile = CreateFileW(
        fileName,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        printWindowsError("Открытие test.bin");

        cout << "\nУбедитесь, что файл test.bin находится\n";
        cout << "в текущей рабочей папке программы.\n";

        return 1;
    }

    LARGE_INTEGER fileSizeLarge;

    if (!GetFileSizeEx(hFile, &fileSizeLarge))
    {
        printWindowsError("GetFileSizeEx");
        CloseHandle(hFile);
        return 1;
    }

    CloseHandle(hFile);

    uint64_t fileSize =
        static_cast<uint64_t>(fileSizeLarge.QuadPart);

    cout << "Файл: test.bin\n";
    cout << "Размер: " << fileSize << " байт\n";
    cout << "Размер: "
         << fixed
         << setprecision(2)
         << static_cast<double>(fileSize) / (1024.0 * 1024.0)
         << " МБ\n";

    if (fileSize == 0)
    {
        cout << "\n[ОШИБКА] Файл пустой.\n";
        return 1;
    }

    cout << "Эталонное однопоточное чтение\n";

    uint64_t referenceChecksum = 0;
    double singleThreadTime = 0;

    bool singleSuccess = singleThreadRead(
        fileName,
        fileSize,
        referenceChecksum,
        singleThreadTime
    );

    if (!singleSuccess)
    {
        cout << "\nНе удалось выполнить однопоточное чтение.\n";
        return 1;
    }

    cout << fixed << setprecision(3);

    cout << "\nКонтрольная сумма: "
         << referenceChecksum << '\n';

    cout << "Время выполнения: "
         << singleThreadTime
         << " мс\n";

    const int experiments[] = { 1, 2, 4, 8 };

    for (int k : experiments)
    {
        runExperiment(
            fileName,
            fileSize,
            k,
            referenceChecksum
        );
    }

    cout << "Все эксперименты завершены.\n";

    return 0;
}
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdio>

const wchar_t* FILE_NAME = L"records.dat";
const int REQUIRED_EXTENSIONS = 3;

#pragma pack(push, 1)
struct Record
{
    int id;
    double value;
    char name[32];
};
#pragma pack(pop)

void printError(const char* operation)
{
    DWORD errorCode = GetLastError();

    char message[512]{};

    DWORD result = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        message,
        sizeof(message),
        nullptr
    );

    std::cout << "Ошибка: " << operation << std::endl;
    std::cout << "Код ошибки: " << errorCode << std::endl;

    if (result != 0)
        std::cout << "Описание: " << message;
    else
        std::cout << "Описание ошибки недоступно." << std::endl;
}

bool getFileSize(HANDLE hFile, LONGLONG& size)
{
    LARGE_INTEGER fileSize{};

    if (!GetFileSizeEx(hFile, &fileSize))
    {
        printError("GetFileSizeEx");
        return false;
    }

    size = fileSize.QuadPart;
    return true;
}

bool resizeFile(HANDLE hFile, LONGLONG newSize)
{
    LARGE_INTEGER position{};
    position.QuadPart = newSize;

    if (!SetFilePointerEx(
        hFile,
        position,
        nullptr,
        FILE_BEGIN))
    {
        printError("SetFilePointerEx");
        return false;
    }

    if (!SetEndOfFile(hFile))
    {
        printError("SetEndOfFile");
        return false;
    }

    return true;
}

bool createMapping(
    HANDLE hFile,
    LONGLONG fileSize,
    HANDLE& hMapping,
    LPVOID& view)
{
    DWORD sizeHigh =
        static_cast<DWORD>(fileSize >> 32);

    DWORD sizeLow =
        static_cast<DWORD>(fileSize & 0xFFFFFFFFULL);

    hMapping = CreateFileMappingW(
        hFile,
        nullptr,
        PAGE_READWRITE,
        sizeHigh,
        sizeLow,
        nullptr
    );

    if (hMapping == nullptr)
    {
        printError("CreateFileMappingW");
        return false;
    }

    view = MapViewOfFile(
        hMapping,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0,
        0,
        0
    );

    if (view == nullptr)
    {
        printError("MapViewOfFile");
        CloseHandle(hMapping);
        hMapping = nullptr;
        return false;
    }

    return true;
}

bool closeMapping(
    HANDLE& hMapping,
    LPVOID& view)
{
    bool success = true;

    if (view != nullptr)
    {
        if (!FlushViewOfFile(view, 0))
        {
            printError("FlushViewOfFile");
            success = false;
        }

        if (!UnmapViewOfFile(view))
        {
            printError("UnmapViewOfFile");
            success = false;
        }

        view = nullptr;
    }

    if (hMapping != nullptr)
    {
        if (!CloseHandle(hMapping))
        {
            printError("CloseHandle");
            success = false;
        }

        hMapping = nullptr;
    }

    return success;
}

Record createRecord(int id)
{
    Record record{};

    record.id = id;
    record.value = id * 1.5;

    std::snprintf(
        record.name,
        sizeof(record.name),
        "Record_%d",
        id
    );

    return record;
}

bool verifyFile(int expectedRecords)
{
    HANDLE hFile = CreateFileW(
        FILE_NAME,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        printError("CreateFileW при проверке");
        return false;
    }

    LONGLONG fileSize = 0;

    if (!getFileSize(hFile, fileSize))
    {
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMapping = CreateFileMappingW(
        hFile,
        nullptr,
        PAGE_READONLY,
        0,
        0,
        nullptr
    );

    if (hMapping == nullptr)
    {
        printError("CreateFileMappingW при проверке");
        CloseHandle(hFile);
        return false;
    }

    LPVOID view = MapViewOfFile(
        hMapping,
        FILE_MAP_READ,
        0,
        0,
        0
    );

    if (view == nullptr)
    {
        printError("MapViewOfFile при проверке");
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return false;
    }

    int recordsInFile =
        static_cast<int>(fileSize / sizeof(Record));

    int recordsToCheck =
        (expectedRecords < recordsInFile)
        ? expectedRecords
        : recordsInFile;

    Record* records =
        static_cast<Record*>(view);

    bool correct = true;

    for (int i = 0; i < recordsToCheck; i++)
    {
        if (records[i].id != i + 1)
        {
            correct = false;

            std::cout
                << "Ошибка в записи №"
                << i + 1
                << std::endl;

            break;
        }
    }

    if (!UnmapViewOfFile(view))
    {
        printError("UnmapViewOfFile при проверке");
        correct = false;
    }

    if (!CloseHandle(hMapping))
    {
        printError("CloseHandle отображения при проверке");
        correct = false;
    }

    if (!CloseHandle(hFile))
    {
        printError("CloseHandle файла при проверке");
        correct = false;
    }

    return correct;
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout
        << "Расширение отображенного файла"
        << std::endl;

    std::cout << std::endl;

    std::cout
        << "Размер одной структуры Record: "
        << sizeof(Record)
        << " байт"
        << std::endl;

    std::cout << std::endl;

    LONGLONG initialSize;
    LONGLONG extensionSize;

    std::cout
        << "Введите начальный размер файла в байтах: ";

    if (!(std::cin >> initialSize))
    {
        std::cout
            << "Некорректный размер файла."
            << std::endl;

        return 1;
    }

    std::cout
        << "Введите размер одного расширения в байтах: ";

    if (!(std::cin >> extensionSize))
    {
        std::cout
            << "Некорректный размер расширения."
            << std::endl;

        return 1;
    }

    if (initialSize < static_cast<LONGLONG>(sizeof(Record)))
    {
        std::cout
            << "Начальный размер должен быть не меньше "
            << sizeof(Record)
            << " байт."
            << std::endl;

        return 1;
    }

    if (extensionSize < static_cast<LONGLONG>(sizeof(Record)))
    {
        std::cout
            << "Размер расширения должен быть не меньше "
            << sizeof(Record)
            << " байт."
            << std::endl;

        return 1;
    }

    DWORD attributes =
        GetFileAttributesW(FILE_NAME);

    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            std::cout
                << "records.dat является папкой."
                << std::endl;

            return 1;
        }

        std::cout
            << "Тестовый файл уже существует."
            << std::endl;
        std::cout
            << "Он будет создан заново."
            << std::endl;
    }

    HANDLE hFile = CreateFileW(
        FILE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        printError("CreateFileW");
        return 1;
    }

    if (!resizeFile(hFile, initialSize))
    {
        CloseHandle(hFile);
        return 1;
    }

    LONGLONG fileSize = 0;

    if (!getFileSize(hFile, fileSize))
    {
        CloseHandle(hFile);
        return 1;
    }

    HANDLE hMapping = nullptr;
    LPVOID view = nullptr;

    if (!createMapping(
        hFile,
        fileSize,
        hMapping,
        view))
    {
        CloseHandle(hFile);
        return 1;
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER totalStart{};
    LARGE_INTEGER totalEnd{};

    if (!QueryPerformanceFrequency(&frequency))
    {
        printError("QueryPerformanceFrequency");
        closeMapping(hMapping, view);
        CloseHandle(hFile);
        return 1;
    }

    if (!QueryPerformanceCounter(&totalStart))
    {
        printError("QueryPerformanceCounter");
        closeMapping(hMapping, view);
        CloseHandle(hFile);
        return 1;
    }

    LONGLONG usedBytes = 0;
    int recordCount = 0;
    int extensionCount = 0;

    std::cout << std::endl;

    std::cout
        << "Начальный размер файла: "
        << fileSize
        << " байт"
        << std::endl;

    while (extensionCount < REQUIRED_EXTENSIONS)
    {
        if (usedBytes + sizeof(Record) <= fileSize)
        {
            Record record =
                createRecord(recordCount + 1);

            std::memcpy(
                static_cast<char*>(view) + usedBytes,
                &record,
                sizeof(Record)
            );

            usedBytes += sizeof(Record);
            recordCount++;
        }
        else
        {
            LONGLONG oldSize = fileSize;

            LARGE_INTEGER extensionStart{};
            LARGE_INTEGER extensionEnd{};

            if (!QueryPerformanceCounter(
                &extensionStart))
            {
                printError("QueryPerformanceCounter");
                closeMapping(hMapping, view);
                CloseHandle(hFile);
                return 1;
            }

            if (!closeMapping(
                hMapping,
                view))
            {
                CloseHandle(hFile);
                return 1;
            }

            fileSize += extensionSize;

            if (!resizeFile(
                hFile,
                fileSize))
            {
                CloseHandle(hFile);
                return 1;
            }

            if (!createMapping(
                hFile,
                fileSize,
                hMapping,
                view))
            {
                CloseHandle(hFile);
                return 1;
            }

            extensionCount++;

            if (!QueryPerformanceCounter(
                &extensionEnd))
            {
                printError("QueryPerformanceCounter");
                closeMapping(hMapping, view);
                CloseHandle(hFile);
                return 1;
            }

            double extensionTime =
                static_cast<double>(
                    extensionEnd.QuadPart -
                    extensionStart.QuadPart
                ) * 1000.0 /
                static_cast<double>(
                    frequency.QuadPart);

            std::cout
                << "Расширение №"
                << extensionCount
                << ": "
                << oldSize
                << " -> "
                << fileSize
                << " байт"
                << std::endl;

            std::cout
                << "Время расширения: "
                << std::fixed
                << std::setprecision(3)
                << extensionTime
                << " мс"
                << std::endl;
        }
    }

    while (usedBytes + sizeof(Record) <= fileSize)
    {
        Record record =
            createRecord(recordCount + 1);

        std::memcpy(
            static_cast<char*>(view) + usedBytes,
            &record,
            sizeof(Record)
        );

        usedBytes += sizeof(Record);
        recordCount++;
    }

    if (!FlushViewOfFile(view, 0))
    {
        printError("FlushViewOfFile");
        closeMapping(hMapping, view);
        CloseHandle(hFile);
        return 1;
    }

    if (!FlushFileBuffers(hFile))
    {
        printError("FlushFileBuffers");
        closeMapping(hMapping, view);
        CloseHandle(hFile);
        return 1;
    }

    if (!QueryPerformanceCounter(&totalEnd))
    {
        printError("QueryPerformanceCounter");
        closeMapping(hMapping, view);
        CloseHandle(hFile);
        return 1;
    }

    double totalTime =
        static_cast<double>(
            totalEnd.QuadPart -
            totalStart.QuadPart
        ) * 1000.0 /
        static_cast<double>(
            frequency.QuadPart);

    if (!closeMapping(
        hMapping,
        view))
    {
        CloseHandle(hFile);
        return 1;
    }

    if (!CloseHandle(hFile))
    {
        printError("CloseHandle");
        return 1;
    }

    std::cout << std::endl;

    std::cout
        << "Результаты работы"
        << std::endl;

    std::cout
        << "Количество записей: "
        << recordCount
        << std::endl;

    std::cout
        << "Количество расширений: "
        << extensionCount
        << std::endl;

    std::cout
        << "Конечный размер файла: "
        << fileSize
        << " байт"
        << std::endl;

    std::cout
        << "Общее время: "
        << std::fixed
        << std::setprecision(3)
        << totalTime
        << " мс"
        << std::endl;

    std::cout << std::endl;

    std::cout
        << "Проверка записанных данных..."
        << std::endl;

    if (!verifyFile(recordCount))
    {
        std::cout
            << "Проверка не пройдена."
            << std::endl;

        return 1;
    }

    std::cout
        << "Проверка пройдена успешно."
        << std::endl;

    std::cout
        << "Все записи сохранены корректно."
        << std::endl;

    return 0;
}
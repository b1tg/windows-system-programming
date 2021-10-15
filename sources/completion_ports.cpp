// source: http://writeasync.net/?p=2201  Introducing overlapped I/O 
// 2021/10/15 😭 太绕了，搞不懂

#include <Windows.h>
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <memory>
#include <functional>
#include <ppltasks.h>
#include "ppltasks_extra.h"
 
using namespace concurrency;
using namespace concurrency::extras;
using namespace std;
 
// Utility function to trace a message with a thread ID
void TraceThread(wstring const & text)
{
    DWORD id = GetCurrentThreadId();
    wstringstream wss;
    wss << L"[T=" << id << L"] " << text << endl;
    wcout << wss.str();
}
 
// Base class to mark a class as non-copyable
struct DenyCopy
{
    DenyCopy() { }
    ~DenyCopy() { }
    DenyCopy & operator =(DenyCopy const &) = delete;
    DenyCopy(DenyCopy const &) = delete;
};
 
// Exception created from generic Win32 error code.
class Win32Error : public runtime_error
{
public:
    Win32Error(string const & message, DWORD error)
        : runtime_error(message),
        error_(error)
    {
    }
 
    ~Win32Error() { }
 
    DWORD get_Error() const { return error_; }
 
private:
    DWORD error_;
};
 
// IO-specific exception associated with a given file.
class IOError : public Win32Error
{
public:
    IOError(string const & message, DWORD error, wstring const & fileName)
        : Win32Error(message, error),
        fileName_(fileName)
    {
    }
 
    ~IOError() { }
 
    wstring const & get_FileName() const { return fileName_; }
 
private:
    wstring fileName_;
};

class FileReadHandle : private DenyCopy
{
public:
    FileReadHandle(wstring const & fileName)
        : fileName_(fileName),
        handle_(CreateFile(fileName.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr))
    {
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            DWORD error = GetLastError();
            throw IOError("Open failed.", error, fileName);
        }
    }
 
    ~FileReadHandle()
    {
        CloseHandle(handle_);
    }
 
    wstring const & get_FileName() const { return fileName_; }
 
    HANDLE get_Value() { return handle_; }
 
private:
    wstring fileName_;
    HANDLE handle_;
};

class PendingIO : private DenyCopy
{
public:
    PendingIO(PTP_IO io)
        : io_(io)
    {
    }
 
    ~PendingIO()
    {
        if (io_)
        {
            CancelThreadpoolIo(io_);
        }
    }
 
    void OnStarted()
    {
        io_ = nullptr;
    }
 
private:
    PTP_IO io_;
};
 
template <typename TCallback>
class ThreadPoolIO : private DenyCopy
{
public:
    ThreadPoolIO(HANDLE handle, TCallback callback)
        : io_(),
        callback_(callback)
    {
        io_ = CreateThreadpoolIo(handle, IOCompletedAdapter, this, nullptr);
        if (!io_)
        {
            DWORD error = GetLastError();
            throw Win32Error("CreateThreadPoolIo failed.", error);
        }
    }
 
    ~ThreadPoolIO()
    {
        WaitForThreadpoolIoCallbacks(io_, FALSE);
        CloseThreadpoolIo(io_);
    }
 
    PendingIO Start()
    {
        StartThreadpoolIo(io_);
        return PendingIO(io_);
    }
 
private:
    PTP_IO io_;
    TCallback callback_;
 
    static void WINAPI IOCompletedAdapter(
        PTP_CALLBACK_INSTANCE instance,
        PVOID context,
        PVOID overlapped,
        ULONG result,
        ULONG_PTR bytesTransferred,
        PTP_IO io)
    {
        static_cast<ThreadPoolIO *>(context)->OnIOCompleted(static_cast<OVERLAPPED *>(overlapped), result, bytesTransferred);
    }
 
    void OnIOCompleted(OVERLAPPED * overlapped, ULONG result, ULONG_PTR bytesTransferred)
    {
        callback_(overlapped, result, bytesTransferred);
    }
};


class FileReader : private DenyCopy
{
public:
    FileReader(FileReadHandle & handle)
        : handle_(handle),
        io_(handle.get_Value(), IOCompletedAdapter),
        offset_(0),
        endOfFile_(false)
    {
    }
 
    ~FileReader()
    {
    }
 
    bool EndOfFile() const { return endOfFile_; }
 
    task<int> ReadAsync(vector<BYTE> & buffer)
    {
        unique_ptr<ReadRequest> request = make_unique<ReadRequest>(*this);
        BOOL result = ReadFile(handle_.get_Value(), &buffer[0], static_cast<DWORD>(buffer.size()), nullptr, request.get());
        if (!result)
        {
            request->OnStartError();
        }
 
        return request.release()->OnStarted();
    }
 
private:
    class ReadRequest;
 
    FileReadHandle & handle_;
    ThreadPoolIO<function<void(OVERLAPPED *, ULONG, ULONG_PTR)>> io_;
    unsigned long long offset_;
    bool endOfFile_;
 
    static void WINAPI IOCompletedAdapter(OVERLAPPED * overlapped, ULONG result, ULONG_PTR bytesTransferred)
    {
        unique_ptr<ReadRequest> request(static_cast<ReadRequest *>(overlapped));
        request->OnCompleted(static_cast<DWORD>(result), static_cast<int>(bytesTransferred));
    }
 
    class ReadRequest : public OVERLAPPED
    {
    public:
        ReadRequest(FileReader & parent)
            : OVERLAPPED({ 0 }),
            parent_(parent),
            pendingIO_(parent.io_.Start()),
            taskEvent_()
        {
            Offset = parent_.offset_ & 0xFFFFFFFF;
            OffsetHigh = parent_.offset_ >> 32;
        }
 
        ~ReadRequest()
        {
        }
 
        void OnStartError()
        {
            DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING)
            {
                throw IOError("Read failed.", error, parent_.handle_.get_FileName());
            }
        }
 
        task<int> OnStarted()
        {
            pendingIO_.OnStarted();
            return task<int>(taskEvent_);
        }
 
        void OnCompleted(DWORD error, int bytesRead)
        {
            if (error == ERROR_HANDLE_EOF)
            {
                parent_.endOfFile_ = true;
                error = ERROR_SUCCESS;
            }
 
            if (error == ERROR_SUCCESS)
            {
                parent_.offset_ += bytesRead;
                taskEvent_.set(bytesRead);
            }
            else
            {
                taskEvent_.set_exception(make_exception_ptr(IOError("Read failed.", error, parent_.handle_.get_FileName())));
            }
        }
 
    private:
        FileReader & parent_;
        PendingIO pendingIO_;
        task_completion_event<int> taskEvent_;
    };
};

template<typename TCallback>
task<void> ReadLoopAsync(FileReader & reader, int bufferSize, TCallback callback)
{
    TraceThread(L"starting read loop");
    return create_iterative_task([&reader, bufferSize, callback]
    {
        shared_ptr<vector<BYTE>> buffer = make_shared<vector<BYTE>>(bufferSize);
        TraceThread(L"starting read");
        task<int> readTask = reader.ReadAsync(*buffer);
        return readTask.then([buffer, &reader, callback](task<int> t)
        {
            int bytesRead = t.get();
            TraceThread(L"completing read request");
            callback(*buffer, bytesRead);
            bool shouldContinue = (bytesRead > 0) && !reader.EndOfFile();
            return task_from_result(shouldContinue);
        });
    });
}


void TraceBufferRead(vector<BYTE> & buffer, int bytesRead)
{
    for (int i = 0; i < bytesRead; ++i)
    {
        cout << static_cast<char>(buffer[i]);
    }
 
    cout << endl;
}
 
void ReadSample(wstring const & fileName)
{
    FileReadHandle handle(fileName);
    FileReader reader(handle);
    task<void> task = ReadLoopAsync(reader, 64, TraceBufferRead);
    task.wait();
}
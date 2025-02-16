// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "synchronization.h"
#include <tuple>
#include <zen/process_priority.h>
#include <zen/perf.h>
#include <zen/guid.h>
#include <zen/crc.h>
#include "algorithm.h"
#include "lib/db_file.h"
#include "lib/dir_exist_async.h"
#include "lib/status_handler_impl.h"
#include "lib/versioning.h"
#include "lib/binary.h"
#include "fs/concrete.h"
#include "fs/native.h"

    #include <unistd.h> //fsync
    #include <fcntl.h>  //open

using namespace zen;
using namespace fff;


namespace
{
inline
int getCUD(const SyncStatistics& stat)
{
    return stat.createCount() +
           stat.updateCount() +
           stat.deleteCount();
}
}


SyncStatistics::SyncStatistics(const FolderComparison& folderCmp)
{
    std::for_each(begin(folderCmp), end(folderCmp), [&](const BaseFolderPair& baseFolder) { recurse(baseFolder); });
}


SyncStatistics::SyncStatistics(const ContainerObject& hierObj)
{
    recurse(hierObj);
}


SyncStatistics::SyncStatistics(const FilePair& file)
{
    processFile(file);
    ++rowsTotal_;
}


inline
void SyncStatistics::recurse(const ContainerObject& hierObj)
{
    for (const FilePair& file : hierObj.refSubFiles())
        processFile(file);
    for (const SymlinkPair& link : hierObj.refSubLinks())
        processLink(link);
    for (const FolderPair& folder : hierObj.refSubFolders())
        processFolder(folder);

    rowsTotal_ += hierObj.refSubFolders().size();
    rowsTotal_ += hierObj.refSubFiles  ().size();
    rowsTotal_ += hierObj.refSubLinks  ().size();
}


inline
void SyncStatistics::processFile(const FilePair& file)
{
    switch (file.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_CREATE_NEW_LEFT:
            ++createLeft_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
            break;

        case SO_CREATE_NEW_RIGHT:
            ++createRight_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
            break;

        case SO_DELETE_LEFT:
            ++deleteLeft_;
            physicalDeleteLeft_ = true;
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            physicalDeleteRight_ = true;
            break;

        case SO_MOVE_LEFT_TO:
            ++updateLeft_;
            //physicalDeleteLeft_ ? -> usually, no; except when falling back to "copy + delete"
            break;

        case SO_MOVE_RIGHT_TO:
            ++updateRight_;
            break;

        case SO_MOVE_LEFT_FROM:  //ignore; already counted
        case SO_MOVE_RIGHT_FROM: //
            break;

        case SO_OVERWRITE_LEFT:
            ++updateLeft_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
            physicalDeleteLeft_ = true;
            break;

        case SO_OVERWRITE_RIGHT:
            ++updateRight_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
            physicalDeleteRight_ = true;
            break;

        case SO_UNRESOLVED_CONFLICT:
            conflictMsgs_.push_back({ file.getPairRelativePath(), file.getSyncOpConflict() });
            break;

        case SO_COPY_METADATA_TO_LEFT:
            ++updateLeft_;
            break;

        case SO_COPY_METADATA_TO_RIGHT:
            ++updateRight_;
            break;

        case SO_DO_NOTHING:
        case SO_EQUAL:
            break;
    }
}


inline
void SyncStatistics::processLink(const SymlinkPair& link)
{
    switch (link.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_CREATE_NEW_LEFT:
            ++createLeft_;
            break;

        case SO_CREATE_NEW_RIGHT:
            ++createRight_;
            break;

        case SO_DELETE_LEFT:
            ++deleteLeft_;
            physicalDeleteLeft_ = true;
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            physicalDeleteRight_ = true;
            break;

        case SO_OVERWRITE_LEFT:
        case SO_COPY_METADATA_TO_LEFT:
            ++updateLeft_;
            physicalDeleteLeft_ = true;
            break;

        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_RIGHT:
            ++updateRight_;
            physicalDeleteRight_ = true;
            break;

        case SO_UNRESOLVED_CONFLICT:
            conflictMsgs_.push_back({ link.getPairRelativePath(), link.getSyncOpConflict() });
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
        case SO_DO_NOTHING:
        case SO_EQUAL:
            break;
    }
}


inline
void SyncStatistics::processFolder(const FolderPair& folder)
{
    switch (folder.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_CREATE_NEW_LEFT:
            ++createLeft_;
            break;

        case SO_CREATE_NEW_RIGHT:
            ++createRight_;
            break;

        case SO_DELETE_LEFT: //if deletion variant == versioning with user-defined directory existing on other volume, this results in a full copy + delete operation!
            ++deleteLeft_;    //however we cannot (reliably) anticipate this situation, fortunately statistics can be adapted during sync!
            physicalDeleteLeft_ = true;
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            physicalDeleteRight_ = true;
            break;

        case SO_UNRESOLVED_CONFLICT:
            conflictMsgs_.push_back({ folder.getPairRelativePath(), folder.getSyncOpConflict() });
            break;

        case SO_OVERWRITE_LEFT:
        case SO_COPY_METADATA_TO_LEFT:
            ++updateLeft_;
            break;

        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_RIGHT:
            ++updateRight_;
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
        case SO_DO_NOTHING:
        case SO_EQUAL:
            break;
    }

    recurse(folder); //since we model logical stats, we recurse, even if deletion variant is "recycler" or "versioning + same volume", which is a single physical operation!
}

//-----------------------------------------------------------------------------------------------------------

std::vector<FolderPairSyncCfg> fff::extractSyncCfg(const MainConfiguration& mainCfg)
{
    //merge first and additional pairs
    std::vector<LocalPairConfig> localCfgs = { mainCfg.firstPair };
    append(localCfgs, mainCfg.additionalPairs);

    std::vector<FolderPairSyncCfg> output;

    for (const LocalPairConfig& lpc : localCfgs)
    {
        //const CompConfig cmpCfg  = lpc.localCmpCfg  ? *lpc.localCmpCfg  : mainCfg.cmpCfg;
        const SyncConfig syncCfg = lpc.localSyncCfg ? *lpc.localSyncCfg : mainCfg.syncCfg;

        output.push_back(
        {
            syncCfg.directionCfg.var == DirectionConfig::TWO_WAY || detectMovedFilesEnabled(syncCfg.directionCfg),
            syncCfg.handleDeletion,
            syncCfg.versioningStyle,
            syncCfg.versioningFolderPhrase,
            syncCfg.directionCfg.var
        });
    }
    return output;
}

//------------------------------------------------------------------------------------------------------------

namespace
{
inline
Opt<SelectedSide> getTargetDirection(SyncOperation syncOp)
{
    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_DELETE_LEFT:
        case SO_OVERWRITE_LEFT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_LEFT_TO:
            return LEFT_SIDE;

        case SO_CREATE_NEW_RIGHT:
        case SO_DELETE_RIGHT:
        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_RIGHT:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_RIGHT_TO:
            return RIGHT_SIDE;

        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            break; //nothing to do
    }
    return NoValue();
}


//test if user accidentally selected the wrong folders to sync
bool significantDifferenceDetected(const SyncStatistics& folderPairStat)
{
    //initial file copying shall not be detected as major difference
    if ((folderPairStat.createCount< LEFT_SIDE>() == 0 ||
         folderPairStat.createCount<RIGHT_SIDE>() == 0) &&
        folderPairStat.updateCount  () == 0 &&
        folderPairStat.deleteCount  () == 0 &&
        folderPairStat.conflictCount() == 0)
        return false;

    const int nonMatchingRows = folderPairStat.createCount() +
                                folderPairStat.deleteCount();
    //folderPairStat.updateCount() +  -> not relevant when testing for "wrong folder selected"
    //folderPairStat.conflictCount();

    return nonMatchingRows >= 10 && nonMatchingRows > 0.5 * folderPairStat.rowCount();
}

//#################################################################################################################

//--------------------- data verification -------------------------
void flushFileBuffers(const Zstring& nativeFilePath) //throw FileError
{
    const int fileHandle = ::open(nativeFilePath.c_str(), O_WRONLY | O_APPEND);
    if (fileHandle == -1)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot open file %x."), L"%x", fmtPath(nativeFilePath)), L"open");
    ZEN_ON_SCOPE_EXIT(::close(fileHandle));

    if (::fsync(fileHandle) != 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file %x."), L"%x", fmtPath(nativeFilePath)), L"fsync");
}


void verifyFiles(const AbstractPath& sourcePath, const AbstractPath& targetPath, const IOCallback& notifyUnbufferedIO) //throw FileError
{
    try
    {
        //do like "copy /v": 1. flush target file buffers, 2. read again as usual (using OS buffers)
        // => it seems OS buffers are not invalidated by this: snake oil???
        if (Opt<Zstring> nativeTargetPath = AFS::getNativeItemPath(targetPath))
            flushFileBuffers(*nativeTargetPath); //throw FileError

        if (!filesHaveSameContent(sourcePath, targetPath, notifyUnbufferedIO)) //throw FileError
            throw FileError(replaceCpy(replaceCpy(_("%x and %y have different content."),
                                                  L"%x", L"\n" + fmtPath(AFS::getDisplayPath(sourcePath))),
                                       L"%y", L"\n" + fmtPath(AFS::getDisplayPath(targetPath))));
    }
    catch (const FileError& e) //add some context to error message
    {
        throw FileError(_("Data verification error:"), e.toString());
    }
}

//#################################################################################################################
//#################################################################################################################

/* ________________________________________________________________
   |                                                              |
   | Multithreaded File Copy: Parallel API for expensive file I/O |
   |______________________________________________________________| */

namespace parallel
{
template <class Function> inline
auto parallelScope(Function fun, std::mutex& singleThread) //throw X
{
    singleThread.unlock();
    ZEN_ON_SCOPE_EXIT(singleThread.lock());

    return fun(); //throw X
}


inline
AFS::ItemType getItemType(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::getItemType(ap); /*throw FileError*/ }, singleThread); }

inline
Opt<AFS::ItemType> getItemTypeIfExists(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::getItemTypeIfExists(ap); /*throw FileError*/ }, singleThread); }

inline
bool removeFileIfExists(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::removeFileIfExists(ap); /*throw FileError*/ }, singleThread); }

inline
bool removeSymlinkIfExists(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::removeSymlinkIfExists(ap); /*throw FileError*/ }, singleThread); }

inline
void renameItem(const AbstractPath& apSource, const AbstractPath& apTarget, std::mutex& singleThread) //throw FileError, ErrorDifferentVolume
{ parallelScope([apSource, apTarget] { AFS::renameItem(apSource, apTarget); /*throw FileError, ErrorDifferentVolume*/ }, singleThread); }

inline
AbstractPath getSymlinkResolvedPath(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::getSymlinkResolvedPath(ap); /*throw FileError*/ }, singleThread); }

inline
void copySymlink(const AbstractPath& apSource, const AbstractPath& apTarget, bool copyFilePermissions, std::mutex& singleThread) //throw FileError
{ parallelScope([apSource, apTarget, copyFilePermissions] { AFS::copySymlink(apSource, apTarget, copyFilePermissions); /*throw FileError*/ }, singleThread); }

inline
void copyNewFolder(const AbstractPath& apSource, const AbstractPath& apTarget, bool copyFilePermissions, std::mutex& singleThread) //throw FileError
{ parallelScope([apSource, apTarget, copyFilePermissions] { AFS::copyNewFolder(apSource, apTarget, copyFilePermissions); /*throw FileError*/ }, singleThread); }

inline
void removeFilePlain(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ parallelScope([ap] { AFS::removeFilePlain(ap); /*throw FileError*/ }, singleThread); }

//--------------------------------------------------------------
//ATTENTION CALLBACKS: they also run asynchronously *outside* the singleThread lock!
//--------------------------------------------------------------
inline
void removeFolderIfExistsRecursion(const AbstractPath& ap, //throw FileError
                                   const std::function<void (const std::wstring& displayPath)>& onBeforeFileDeletion, //optional
                                   const std::function<void (const std::wstring& displayPath)>& onBeforeFolderDeletion, //one call for each object!
                                   std::mutex& singleThread)
{ parallelScope([ap, onBeforeFileDeletion, onBeforeFolderDeletion] { AFS::removeFolderIfExistsRecursion(ap, onBeforeFileDeletion, onBeforeFolderDeletion); /*throw FileError*/ }, singleThread); }


inline
AFS::FileCopyResult copyFileTransactional(const AbstractPath& apSource, const AFS::StreamAttributes& attrSource, //throw FileError, ErrorFileLocked
                                          const AbstractPath& apTarget,
                                          bool copyFilePermissions,
                                          bool transactionalCopy,
                                          const std::function<void()>& onDeleteTargetFile,
                                          const IOCallback& notifyUnbufferedIO,
                                          std::mutex& singleThread)
{
    return parallelScope([=]
    {
        return AFS::copyFileTransactional(apSource, attrSource, apTarget, copyFilePermissions, transactionalCopy, onDeleteTargetFile, notifyUnbufferedIO); //throw FileError, ErrorFileLocked
    }, singleThread);
}

inline //RecycleSession::recycleItem() is internally synchronized!
bool recycleItem(AFS::RecycleSession& recyclerSession, const AbstractPath& ap, const Zstring& logicalRelPath, std::mutex& singleThread) //throw FileError
{ return parallelScope([=, &recyclerSession] { return recyclerSession.recycleItem(ap, logicalRelPath); /*throw FileError*/ }, singleThread); }

inline //FileVersioner::revisionFile() is internally synchronized!
bool revisionFile(FileVersioner& versioner, const FileDescriptor& fileDescr, const Zstring& relativePath, const IOCallback& notifyUnbufferedIO, std::mutex& singleThread) //throw FileError
{ return parallelScope([=, &versioner] { return versioner.revisionFile(fileDescr, relativePath, notifyUnbufferedIO); /*throw FileError*/ }, singleThread); }

inline //FileVersioner::revisionSymlink() is internally synchronized!
bool revisionSymlink(FileVersioner& versioner, const AbstractPath& linkPath, const Zstring& relativePath, std::mutex& singleThread) //throw FileError
{ return parallelScope([=, &versioner] { return versioner.revisionSymlink(linkPath, relativePath); /*throw FileError*/ }, singleThread); }

inline //FileVersioner::revisionFolder() is internally synchronized!
void revisionFolder(FileVersioner& versioner,
                    const AbstractPath& folderPath, const Zstring& relativePath, //throw FileError
                    const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFileMove,
                    const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFolderMove,
                    const IOCallback& notifyUnbufferedIO,
                    std::mutex& singleThread)
{ parallelScope([=, &versioner] { versioner.revisionFolder(folderPath, relativePath, onBeforeFileMove, onBeforeFolderMove, notifyUnbufferedIO); /*throw FileError*/ }, singleThread); }

inline
void verifyFiles(const AbstractPath& apSource, const AbstractPath& apTarget, const IOCallback& notifyUnbufferedIO, std::mutex& singleThread) //throw FileError
{ parallelScope([=] { ::verifyFiles(apSource, apTarget, notifyUnbufferedIO); /*throw FileError*/ }, singleThread); }

}


namespace
{
class AsyncCallback //actor pattern
{
public:
    AsyncCallback(size_t threadCount) : threadStatus_(threadCount), totalThreadCount_(threadCount) {}

    //non-blocking: context of worker thread
    void updateDataProcessed(int itemsDelta, int64_t bytesDelta) //noexcept!!
    {
        itemsDeltaProcessed_ += itemsDelta;
        bytesDeltaProcessed_ += bytesDelta;
    }
    void updateDataTotal(int itemsDelta, int64_t bytesDelta) //noexcept!!
    {
        itemsDeltaTotal_ += itemsDelta;
        bytesDeltaTotal_ += bytesDelta;
    }

    //context of main thread
    void reportStats(ProcessCallback& cb)
    {
        assert(std::this_thread::get_id() == mainThreadId);

        const std::pair<int, int64_t> deltaProcessed(itemsDeltaProcessed_, bytesDeltaProcessed_);
        if (deltaProcessed.first != 0 || deltaProcessed.second != 0)
        {
            updateDataProcessed   (-deltaProcessed.first, -deltaProcessed.second); //careful with these atomics: don't just set to 0
            cb.updateDataProcessed( deltaProcessed.first,  deltaProcessed.second); //noexcept!!
        }
        const std::pair<int, int64_t> deltaTotal(itemsDeltaTotal_, bytesDeltaTotal_);
        if (deltaTotal.first != 0 || deltaTotal.second != 0)
        {
            updateDataTotal   (-deltaTotal.first, -deltaTotal.second);
            cb.updateDataTotal( deltaTotal.first,  deltaTotal.second); //noexcept!!
        }
    }

    //context of worker thread
    void reportStatus(const std::wstring& msg, size_t threadIdx) //throw ThreadInterruption
    {
        assert(std::this_thread::get_id() != mainThreadId);
        std::lock_guard<std::mutex> dummy(lockCurrentStatus_);

        assert(threadStatus_[threadIdx].active);
        threadStatus_[threadIdx].statusMsg = msg;

        interruptionPoint(); //throw ThreadInterruption
    }

    //context of main thread, call repreatedly
    std::wstring getCurrentStatus()
    {
        assert(std::this_thread::get_id() == mainThreadId);

        int activeThreadCount = 0;
        std::wstring statusMsg;
        {
            std::lock_guard<std::mutex> dummy(lockCurrentStatus_);

            for (const ThreadStatus& ts : threadStatus_)
                if (ts.active)
                {
                    ++activeThreadCount;
                    if (statusMsg.empty())
                        statusMsg = ts.statusMsg;
                }
        }

        std::wstring output;
        if (activeThreadCount >= 2)
            output = L"[" + _P("1 thread", "%x threads", activeThreadCount) + L"] ";
        output += statusMsg;
        return output;
    }

    //blocking call: context of worker thread
    //=> indirect support for "pause": reportInfo() is called under singleThread lock,
    //   so all other worker threads will wait when coming out of parallel I/O (trying to lock singleThread)
    void reportInfo(const std::wstring& msg, size_t threadIdx) //throw ThreadInterruption
    {
        reportStatus(msg, threadIdx); //throw ThreadInterruption
        logInfo     (msg, threadIdx); //
    }

    //blocking call: context of worker thread
    void logInfo(const std::wstring& msg, size_t threadIdx) //throw ThreadInterruption
    {
        assert(std::this_thread::get_id() != mainThreadId);
        std::unique_lock<std::mutex> dummy(lockRequest_);
        interruptibleWait(conditionReadyForNewRequest_, dummy, [this] { return !logInfoRequest_; }); //throw ThreadInterruption

        logInfoRequest_ = (totalThreadCount_ > 1 ? L"[" + numberTo<std::wstring>(threadIdx + 1) + L"] " : L"") + msg;

        dummy.unlock(); //optimization for condition_variable::notify_all()
        conditionNewRequest.notify_all();
    }

    //blocking call: context of worker thread
    ProcessCallback::Response reportError(const std::wstring& msg, size_t retryNumber, size_t threadIdx) //throw ThreadInterruption
    {
        assert(std::this_thread::get_id() != mainThreadId);
        std::unique_lock<std::mutex> dummy(lockRequest_);
        interruptibleWait(conditionReadyForNewRequest_, dummy, [this] { return !errorRequest_ && !errorResponse_; }); //throw ThreadInterruption

        errorRequest_ = ErrorInfo({ (totalThreadCount_ > 1 ? L"[" + numberTo<std::wstring>(threadIdx + 1) + L"] " : L"") + msg, retryNumber });
        conditionNewRequest.notify_all();

        interruptibleWait(conditionHaveResponse_, dummy, [this] { return static_cast<bool>(errorResponse_); }); //throw ThreadInterruption

        ProcessCallback::Response rv = *errorResponse_;

        errorRequest_  = NoValue();
        errorResponse_ = NoValue();

        dummy.unlock(); //optimization for condition_variable::notify_all()
        conditionReadyForNewRequest_.notify_all(); //=> spurious wake-up for AsyncCallback::logInfo()
        return rv;
    }

    //context of main thread
    void waitUntilDone(std::chrono::milliseconds duration, ProcessCallback& cb) //throw X
    {
        assert(std::this_thread::get_id() == mainThreadId);
        for (;;)
        {
            const std::chrono::steady_clock::time_point callbackTime = std::chrono::steady_clock::now() + duration;

            for (std::unique_lock<std::mutex> dummy(lockRequest_) ;;) //process all errors without delay
            {
                const bool rv = conditionNewRequest.wait_until(dummy, callbackTime, [this] { return (errorRequest_ && !errorResponse_) || logInfoRequest_ || finishNowRequest_; });
                if (!rv) //time-out + condition not met
                    break;

                if (errorRequest_ && !errorResponse_)
                {
                    assert(!finishNowRequest_);
                    errorResponse_ = cb.reportError(errorRequest_->msg, errorRequest_->retryNumber); //throw X
                    conditionHaveResponse_.notify_all(); //instead of notify_one(); workaround bug: https://svn.boost.org/trac/boost/ticket/7796
                }
                if (logInfoRequest_)
                {
                    cb.logInfo(*logInfoRequest_);
                    logInfoRequest_ = NoValue();
                    conditionReadyForNewRequest_.notify_all(); //=> spurious wake-up for AsyncCallback::reportError()
                }
                if (finishNowRequest_)
                {
                    dummy.unlock(); //call member functions outside of mutex scope:
                    reportStats(cb); //one last call for accurate stat-reporting!
                    return;
                }
            }

            //call member functions outside of mutex scope:
            cb.reportStatus(getCurrentStatus()); //throw X
            reportStats(cb);
        }
    }

    void notifyWorkBegin(size_t threadIdx) //noexcept
    {
        std::lock_guard<std::mutex> dummy(lockCurrentStatus_);
        assert(!threadStatus_[threadIdx].active);
        threadStatus_[threadIdx].active = true;
    }

    void notifyWorkEnd(size_t threadIdx) //noexcept
    {
        std::lock_guard<std::mutex> dummy(lockCurrentStatus_);
        assert(threadStatus_[threadIdx].active);
        threadStatus_[threadIdx].active = false;
        threadStatus_[threadIdx].statusMsg.clear();
    }

    void notifyAllDone() //noexcept
    {
        std::lock_guard<std::mutex> dummy(lockRequest_);
        assert(!finishNowRequest_);
        finishNowRequest_ = true;
        conditionNewRequest.notify_all(); //perf: should unlock mutex before notify!? (insignificant)
    }

private:
    AsyncCallback           (const AsyncCallback&) = delete;
    AsyncCallback& operator=(const AsyncCallback&) = delete;

    struct ThreadStatus
    {
        bool active = false;
        std::wstring statusMsg;
    };
    struct ErrorInfo
    {
        std::wstring msg;
        size_t retryNumber = 0;
    };

    //---- main <-> worker communication channel ----
    std::mutex lockRequest_;
    std::condition_variable conditionReadyForNewRequest_;
    std::condition_variable conditionNewRequest;
    std::condition_variable conditionHaveResponse_;
    Opt<ErrorInfo>                 errorRequest_;
    Opt<ProcessCallback::Response> errorResponse_;
    Opt<std::wstring>              logInfoRequest_;
    bool finishNowRequest_ = false;

    //---- status updates ----
    std::mutex lockCurrentStatus_; //use a different lock for current file: continue traversing while some thread may process an error
    std::vector<ThreadStatus> threadStatus_;

    const size_t totalThreadCount_;

    //---- status updates II (lock-free) ----
    std::atomic<int>     itemsDeltaProcessed_{ 0 }; //
    std::atomic<int64_t> bytesDeltaProcessed_{ 0 }; //std:atomic is uninitialized by default!
    std::atomic<int>     itemsDeltaTotal_    { 0 }; //
    std::atomic<int64_t> bytesDeltaTotal_    { 0 }; //
};


template <typename Function> inline //return ignored error message if available
Opt<std::wstring> tryReportingError(Function cmd, size_t threadIdx, AsyncCallback& acb) //throw ThreadInterruption
{
    for (size_t retryNumber = 0;; ++retryNumber)
        try
        {
            cmd(); //throw FileError
            return NoValue();
        }
        catch (FileError& error)
        {
            switch (acb.reportError(error.toString(), retryNumber, threadIdx)) //throw ThreadInterruption
            {
                case ProcessCallback::IGNORE_ERROR:
                    return error.toString();
                case ProcessCallback::RETRY:
                    break; //continue with loop
            }
        }
}


//manage statistics reporting for a single item of work
class AsyncItemStatReporter
{
public:
    AsyncItemStatReporter(int itemsExpected, int64_t bytesExpected, size_t threadIdx, AsyncCallback& acb) :
        itemsExpected_(itemsExpected),
        bytesExpected_(bytesExpected),
        threadIdx_(threadIdx),
        acb_(acb) {}

    ~AsyncItemStatReporter()
    {
        const bool scopeFail = getUncaughtExceptionCount() > exeptionCount_;
        if (scopeFail)
            acb_.updateDataTotal(itemsReported_, bytesReported_); //=> unexpected increase of total workload
        else
            //update statistics to consider the real amount of data, e.g. more than the "file size" for ADS streams,
            //less for sparse and compressed files,  or file changed in the meantime!
            acb_.updateDataTotal(itemsReported_ - itemsExpected_, bytesReported_ - bytesExpected_); //noexcept!
    }

    void reportStatus(const std::wstring& text) { acb_.reportStatus(text, threadIdx_); } //throw ThreadInterruption

    void reportDelta(int itemsDelta, int64_t bytesDelta) //throw ThreadInterruption
    {
        acb_.updateDataProcessed(itemsDelta, bytesDelta); //nothrow!
        itemsReported_ += itemsDelta;
        bytesReported_ += bytesDelta;

        //special rule: avoid temporary statistics mess up, even though they are corrected anyway below:
        if (itemsReported_ > itemsExpected_)
        {
            acb_.updateDataTotal(itemsReported_ - itemsExpected_, 0);
            itemsReported_ = itemsExpected_;
        }
        if (bytesReported_ > bytesExpected_)
        {
            acb_.updateDataTotal(0, bytesReported_ - bytesExpected_); //=> everything above "bytesExpected" adds to both "processed" and "total" data
            bytesReported_ = bytesExpected_;
        }

        interruptionPoint(); //throw ThreadInterruption
    }

private:
    int itemsReported_ = 0;
    int64_t bytesReported_ = 0;
    const int itemsExpected_;
    const int64_t bytesExpected_;
    const size_t threadIdx_;
    AsyncCallback& acb_;
    const int exeptionCount_ = getUncaughtExceptionCount();
};
}

//#################################################################################################################
//#################################################################################################################

class DeletionHandling //abstract deletion variants: permanently, recycle bin, user-defined directory
{
public:
    DeletionHandling(const AbstractPath& baseFolderPath,
                     DeletionPolicy handleDel, //nothrow!
                     const Zstring& versioningFolderPhrase,
                     VersioningStyle versioningStyle,
                     const TimeComp& timeStamp);

    //clean-up temporary directory (recycle bin optimization)
    void tryCleanup(ProcessCallback& cb /*throw X*/, bool allowCallbackException); //throw FileError -> call this in non-exceptional code path, i.e. somewhere after sync!

    void removeDirWithCallback (const AbstractPath&   dirPath,   const Zstring& relativePath, AsyncItemStatReporter& statReporter, std::mutex& singleThread); //
    void removeFileWithCallback(const FileDescriptor& fileDescr, const Zstring& relativePath, AsyncItemStatReporter& statReporter, std::mutex& singleThread); //throw FileError, ThreadInterruption
    void removeLinkWithCallback(const AbstractPath&   linkPath,  const Zstring& relativePath, AsyncItemStatReporter& statReporter, std::mutex& singleThread); //

    const std::wstring& getTxtRemovingFile   () const { return txtRemovingFile_;    } //
    const std::wstring& getTxtRemovingFolder () const { return txtRemovingFolder_;  } //buffered status texts
    const std::wstring& getTxtRemovingSymLink() const { return txtRemovingSymlink_; } //

private:
    DeletionHandling           (const DeletionHandling&) = delete;
    DeletionHandling& operator=(const DeletionHandling&) = delete;

    AFS::RecycleSession& getOrCreateRecyclerSession() //throw FileError => dont create in constructor!!!
    {
        assert(deletionPolicy_ == DeletionPolicy::RECYCLER);
        if (!recyclerSession_)
            recyclerSession_ =  AFS::createRecyclerSession(baseFolderPath_); //throw FileError
        return *recyclerSession_;
    }

    FileVersioner& getOrCreateVersioner() //throw FileError => dont create in constructor!!!
    {
        assert(deletionPolicy_ == DeletionPolicy::VERSIONING);
        if (!versioner_)
            versioner_ = std::make_unique<FileVersioner>(versioningFolderPath_, versioningStyle_, timeStamp_); //throw FileError
        return *versioner_;
    }

    const DeletionPolicy deletionPolicy_; //keep it invariant! e.g. consider getOrCreateVersioner() one-time construction!

    const AbstractPath baseFolderPath_;
    std::unique_ptr<AFS::RecycleSession> recyclerSession_;

    //used only for DeletionPolicy::VERSIONING:
    const AbstractPath versioningFolderPath_;
    const VersioningStyle versioningStyle_;
    const TimeComp timeStamp_;
    std::unique_ptr<FileVersioner> versioner_; //throw FileError in constructor => create on demand!

    //buffer status texts:
    const std::wstring txtRemovingFile_;
    const std::wstring txtRemovingSymlink_;
    const std::wstring txtRemovingFolder_;
    const std::wstring txtMovingFileXtoY_   = _("Moving file %x to %y");
    const std::wstring txtMovingFolderXtoY_ = _("Moving folder %x to %y");
};


DeletionHandling::DeletionHandling(const AbstractPath& baseFolderPath, //nothrow!
                                   DeletionPolicy handleDel,
                                   const Zstring& versioningFolderPhrase,
                                   VersioningStyle versioningStyle,
                                   const TimeComp& timeStamp) :
    deletionPolicy_(handleDel),
    baseFolderPath_(baseFolderPath),
    versioningFolderPath_(createAbstractPath(versioningFolderPhrase)),
    versioningStyle_(versioningStyle),
    timeStamp_(timeStamp),
    txtRemovingFile_([&]
{
    switch (handleDel)
    {
        case DeletionPolicy::PERMANENT:
            return _("Deleting file %x");
        case DeletionPolicy::RECYCLER:
            return _("Moving file %x to the recycle bin");
        case DeletionPolicy::VERSIONING:
            return replaceCpy(_("Moving file %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));
    }
    return std::wstring();
}()),
txtRemovingSymlink_([&]
{
    switch (handleDel)
    {
        case DeletionPolicy::PERMANENT:
            return _("Deleting symbolic link %x");
        case DeletionPolicy::RECYCLER:
            return _("Moving symbolic link %x to the recycle bin");
        case DeletionPolicy::VERSIONING:
            return replaceCpy(_("Moving symbolic link %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));
    }
    return std::wstring();
}()),
txtRemovingFolder_([&]
{
    switch (handleDel)
    {
        case DeletionPolicy::PERMANENT:
            return _("Deleting folder %x");
        case DeletionPolicy::RECYCLER:
            return _("Moving folder %x to the recycle bin");
        case DeletionPolicy::VERSIONING:
            return replaceCpy(_("Moving folder %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));
    }
    return std::wstring();
}()) {}


void DeletionHandling::tryCleanup(ProcessCallback& cb /*throw X*/, bool allowCallbackException) //throw FileError
{
    assert(std::this_thread::get_id() == mainThreadId);
    switch (deletionPolicy_)
    {
        case DeletionPolicy::PERMANENT:
            break;

        case DeletionPolicy::RECYCLER:
            if (recyclerSession_)
            {
                auto notifyDeletionStatus = [&](const std::wstring& displayPath)
                {
                    try
                    {
                        if (!displayPath.empty())
                            cb.reportStatus(replaceCpy(txtRemovingFile_, L"%x", fmtPath(displayPath))); //throw X
                        else
                            cb.requestUiRefresh(); //throw X
                    }
                    catch (...)
                    {
                        if (allowCallbackException)
                            throw;
                    }
                };

                //move content of temporary directory to recycle bin in a single call
                getOrCreateRecyclerSession().tryCleanup(notifyDeletionStatus); //throw FileError
            }
            break;

        case DeletionPolicy::VERSIONING:
            //if (versioner_)
            //{
            //    if (allowUserCallback)
            //    {
            //        cb_.reportStatus(_("Removing old versions...")); //throw X
            //        versioner->limitVersions([&] { cb_.requestUiRefresh(); /*throw X */ }); //throw FileError
            //    }
            //    else
            //        versioner->limitVersions([] {}); //throw FileError
            //}
            break;
    }
}


void DeletionHandling::removeDirWithCallback(const AbstractPath& folderPath,//throw FileError, ThreadInterruption
                                             const Zstring& relativePath,
                                             AsyncItemStatReporter& statReporter, std::mutex& singleThread)
{
    switch (deletionPolicy_)
    {
        case DeletionPolicy::PERMANENT:
        {
            //callbacks run *outside* singleThread_ lock! => fine
            auto notifyDeletion = [&statReporter](const std::wstring& statusText, const std::wstring& displayPath)
            {
                statReporter.reportStatus(replaceCpy(statusText, L"%x", fmtPath(displayPath))); //throw ThreadInterruption
                statReporter.reportDelta(1, 0); //throw ThreadInterruption; it would be more correct to report *after* work was done!
            };
            static_assert(std::is_const<decltype(txtRemovingFile_)>::value, "callbacks better be thread-safe!");
            auto onBeforeFileDeletion = [&](const std::wstring& displayPath) { notifyDeletion(txtRemovingFile_,   displayPath); };
            auto onBeforeDirDeletion  = [&](const std::wstring& displayPath) { notifyDeletion(txtRemovingFolder_, displayPath); };

            parallel::removeFolderIfExistsRecursion(folderPath, onBeforeFileDeletion, onBeforeDirDeletion, singleThread); //throw FileError
        }
        break;

        case DeletionPolicy::RECYCLER:
            parallel::recycleItem(getOrCreateRecyclerSession(), folderPath, relativePath, singleThread); //throw FileError
            statReporter.reportDelta(1, 0); //throw ThreadInterruption; moving to recycler is ONE logical operation, irrespective of the number of child elements!
            break;

        case DeletionPolicy::VERSIONING:
        {
            //callbacks run *outside* singleThread_ lock! => fine
            auto notifyMove = [&statReporter](const std::wstring& statusText, const std::wstring& displayPathFrom, const std::wstring& displayPathTo)
            {
                statReporter.reportStatus(replaceCpy(replaceCpy(statusText, L"%x", L"\n" + fmtPath(displayPathFrom)), L"%y", L"\n" + fmtPath(displayPathTo))); //throw ThreadInterruption
                statReporter.reportDelta(1, 0); //throw ThreadInterruption; it would be more correct to report *after* work was done!
            };
            static_assert(std::is_const<decltype(txtMovingFileXtoY_)>::value, "callbacks better be thread-safe!");
            auto onBeforeFileMove   = [&](const std::wstring& displayPathFrom, const std::wstring& displayPathTo) { notifyMove(txtMovingFileXtoY_,   displayPathFrom, displayPathTo); };
            auto onBeforeFolderMove = [&](const std::wstring& displayPathFrom, const std::wstring& displayPathTo) { notifyMove(txtMovingFolderXtoY_, displayPathFrom, displayPathTo); };
            auto notifyUnbufferedIO = [&](int64_t bytesDelta) { statReporter.reportDelta(0, bytesDelta); }; //throw ThreadInterruption

            parallel::revisionFolder(getOrCreateVersioner(), folderPath, relativePath, onBeforeFileMove, onBeforeFolderMove, notifyUnbufferedIO, singleThread); //throw FileError, ThreadInterruption
        }
        break;
    }
}


void DeletionHandling::removeFileWithCallback(const FileDescriptor& fileDescr, //throw FileError, ThreadInterruption
                                              const Zstring& relativePath,
                                              AsyncItemStatReporter& statReporter, std::mutex& singleThread)
{

    if (endsWith(relativePath, AFS::TEMP_FILE_ENDING)) //special rule for .ffs_tmp files: always delete permanently!
        parallel::removeFileIfExists(fileDescr.path, singleThread); //throw FileError
    else
        switch (deletionPolicy_)
        {
            case DeletionPolicy::PERMANENT:
                parallel::removeFileIfExists(fileDescr.path, singleThread); //throw FileError
                break;
            case DeletionPolicy::RECYCLER:
                parallel::recycleItem(getOrCreateRecyclerSession(), fileDescr.path, relativePath, singleThread); //throw FileError
                break;
            case DeletionPolicy::VERSIONING:
            {
                //callback runs *outside* singleThread_ lock! => fine
                auto notifyUnbufferedIO = [&](int64_t bytesDelta) { statReporter.reportDelta(0, bytesDelta); }; //throw ThreadInterruption

                parallel::revisionFile(getOrCreateVersioner(), fileDescr, relativePath, notifyUnbufferedIO, singleThread); //throw FileError
            }
            break;
        }

    //even if the source item does not exist anymore, significant I/O work was done => report
    //-> also consider unconditional statReporter.reportDelta(-1, 0) when overwriting a file
    statReporter.reportDelta(1, 0); //throw ThreadInterruption
}


void DeletionHandling::removeLinkWithCallback(const AbstractPath& linkPath, //throw FileError, throw ThreadInterruption
                                              const Zstring& relativePath,
                                              AsyncItemStatReporter& statReporter, std::mutex& singleThread)
{
    switch (deletionPolicy_)
    {
        case DeletionPolicy::PERMANENT:
            parallel::removeSymlinkIfExists(linkPath, singleThread); //throw FileError
            break;
        case DeletionPolicy::RECYCLER:
            parallel::recycleItem(getOrCreateRecyclerSession(), linkPath, relativePath, singleThread); //throw FileError
            break;
        case DeletionPolicy::VERSIONING:
            parallel::revisionSymlink(getOrCreateVersioner(), linkPath, relativePath, singleThread); //throw FileError
            break;
    }

    //report unconditionally, see removeFileWithCallback()
    statReporter.reportDelta(1, 0); //throw ThreadInterruption
}

//------------------------------------------------------------------------------------------------------------

/*
  DeletionPolicy::PERMANENT:  deletion frees space
  DeletionPolicy::RECYCLER:   won't free space until recycler is full, but then frees space
  DeletionPolicy::VERSIONING: depends on whether versioning folder is on a different volume
-> if deleted item is a followed symlink, no space is freed
-> created/updated/deleted item may be on a different volume than base directory: consider symlinks, junctions!

=> generally assume deletion frees space; may avoid false positive disk space warnings for recycler and versioning
*/
class MinimumDiskSpaceNeeded
{
public:
    static std::pair<int64_t, int64_t> calculate(const BaseFolderPair& baseFolder)
    {
        MinimumDiskSpaceNeeded inst;
        inst.recurse(baseFolder);
        return { inst.spaceNeededLeft_, inst.spaceNeededRight_ };
    }

private:
    void recurse(const ContainerObject& hierObj)
    {
        //don't process directories

        //process files
        for (const FilePair& file : hierObj.refSubFiles())
            switch (file.getSyncOperation()) //evaluate comparison result and sync direction
            {
                case SO_CREATE_NEW_LEFT:
                    spaceNeededLeft_ += static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
                    break;

                case SO_CREATE_NEW_RIGHT:
                    spaceNeededRight_ += static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
                    break;

                case SO_DELETE_LEFT:
                    //if (freeSpaceDelLeft_)
                    spaceNeededLeft_ -= static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
                    break;

                case SO_DELETE_RIGHT:
                    //if (freeSpaceDelRight_)
                    spaceNeededRight_ -= static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
                    break;

                case SO_OVERWRITE_LEFT:
                    //if (freeSpaceDelLeft_)
                    spaceNeededLeft_ -= static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
                    spaceNeededLeft_ += static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
                    break;

                case SO_OVERWRITE_RIGHT:
                    //if (freeSpaceDelRight_)
                    spaceNeededRight_ -= static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
                    spaceNeededRight_ += static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
                    break;

                case SO_DO_NOTHING:
                case SO_EQUAL:
                case SO_UNRESOLVED_CONFLICT:
                case SO_COPY_METADATA_TO_LEFT:
                case SO_COPY_METADATA_TO_RIGHT:
                case SO_MOVE_LEFT_FROM:
                case SO_MOVE_RIGHT_FROM:
                case SO_MOVE_LEFT_TO:
                case SO_MOVE_RIGHT_TO:
                    break;
            }

        //symbolic links
        //[...]

        //recurse into sub-dirs
        for (const FolderPair& folder : hierObj.refSubFolders())
            recurse(folder);
    }

    int64_t spaceNeededLeft_  = 0;
    int64_t spaceNeededRight_ = 0;
};

//----------------------------------------------------------------------------------------
class Workload;

class FolderPairSyncer
{
public:
    struct SyncCtx
    {
        bool verifyCopiedFiles;
        bool copyFilePermissions;
        bool failSafeFileCopy;
        std::vector<FileError>& errorsModTime;
        DeletionHandling& delHandlingLeft;
        DeletionHandling& delHandlingRight;
        size_t threadCount;
    };

    static void runSync(SyncCtx& syncCtx, BaseFolderPair& baseFolder, ProcessCallback& cb)
    {
        runPass(PASS_ZERO, syncCtx, baseFolder, cb); //prepare file moves
        runPass(PASS_ONE,  syncCtx, baseFolder, cb); //delete files (or overwrite big ones with smaller ones)
        runPass(PASS_TWO,  syncCtx, baseFolder, cb); //copy rest
    }

private:
    friend class Workload;
    enum PassNo
    {
        PASS_ZERO, //prepare file moves
        PASS_ONE,  //delete files
        PASS_TWO,  //create, modify
        PASS_NEVER //skip item
    };

    FolderPairSyncer(SyncCtx& syncCtx, Workload& workload, std::mutex& singleThread, size_t threadIdx, AsyncCallback& acb) :
        errorsModTime_      (syncCtx.errorsModTime),
        delHandlingLeft_    (syncCtx.delHandlingLeft),
        delHandlingRight_   (syncCtx.delHandlingRight),
        verifyCopiedFiles_  (syncCtx.verifyCopiedFiles),
        copyFilePermissions_(syncCtx.copyFilePermissions),
        failSafeFileCopy_   (syncCtx.failSafeFileCopy),
        workload_(workload),
        singleThread_(singleThread),
        threadIdx_(threadIdx),
        acb_(acb) {}

    static PassNo getPass(const FilePair&    file);
    static PassNo getPass(const SymlinkPair& link);
    static PassNo getPass(const FolderPair&  folder);

    static void runPass(PassNo pass, SyncCtx& syncCtx, BaseFolderPair& baseFolder, ProcessCallback& cb); //throw X

    static void appendFolderLevelWorkItems(PassNo pass, ContainerObject& hierObj, //in
                                           std::vector<std::function<void(FolderPairSyncer& fps)>>& workItems, //out
                                           std::vector<ContainerObject*>& foldersToProcess);                   //

    template <SelectedSide side>
    void setup2StepMove(FilePair& sourceObj, FilePair& targetObj); //throw FileError, ThreadInterruption
    bool createParentFolder(FileSystemObject& fsObj); //throw FileError, ThreadInterruption
    template <SelectedSide side>
    void resolveMoveConflicts(FilePair& sourceObj, FilePair& targetObj); //throw FileError, ThreadInterruption
    void prepareFileMove(FilePair& file); //throw ThreadInterruption

    void synchronizeFile(FilePair& file);                                                       //
    template <SelectedSide side> void synchronizeFileInt(FilePair& file, SyncOperation syncOp); //throw FileError, ThreadInterruption

    void synchronizeLink(SymlinkPair& link);                                                          //
    template <SelectedSide sideTrg> void synchronizeLinkInt(SymlinkPair& link, SyncOperation syncOp); //throw FileError, ThreadInterruption

    void synchronizeFolder(FolderPair& folder);                                                          //
    template <SelectedSide sideTrg> void synchronizeFolderInt(FolderPair& folder, SyncOperation syncOp); //throw FileError, ThreadInterruption

    void reportInfo(const std::wstring& rawText, const std::wstring& displayPath) { acb_.reportInfo(replaceCpy(rawText, L"%x", fmtPath(displayPath)), threadIdx_); }
    void reportInfo(const std::wstring& rawText, const std::wstring& displayPath1, const std::wstring& displayPath2)
    {
        acb_.reportInfo(replaceCpy(replaceCpy(rawText, L"%x", L"\n" + fmtPath(displayPath1)), L"%y", L"\n" + fmtPath(displayPath2)), threadIdx_);
    }

    //target existing after onDeleteTargetFile(): undefined behavior! (fail/overwrite/auto-rename)
    AFS::FileCopyResult copyFileWithCallback(const FileDescriptor& sourceDescr, //throw FileError
                                             const AbstractPath& targetPath,
                                             const std::function<void()>& onDeleteTargetFile, //optional!
                                             AsyncItemStatReporter& statReporter);
    std::vector<FileError>& errorsModTime_;

    DeletionHandling& delHandlingLeft_;
    DeletionHandling& delHandlingRight_;

    const bool verifyCopiedFiles_;
    const bool copyFilePermissions_;
    const bool failSafeFileCopy_;

    Workload& workload_;
    std::mutex& singleThread_;
    const size_t threadIdx_;
    AsyncCallback& acb_;

    //preload status texts (premature?)
    const std::wstring txtCreatingFile_      {_("Creating file %x"         )};
    const std::wstring txtCreatingLink_      {_("Creating symbolic link %x")};
    const std::wstring txtCreatingFolder_    {_("Creating folder %x"       )};
    const std::wstring txtUpdatingFile_      {_("Updating file %x"         )};
    const std::wstring txtUpdatingLink_      {_("Updating symbolic link %x")};
    const std::wstring txtVerifyingFile_     {_("Verifying file %x"        )};
    const std::wstring txtUpdatingAttributes_{_("Updating attributes of %x")};
    const std::wstring txtMovingFileXtoY_    {_("Moving file %x to %y"     )};
    const std::wstring txtSourceItemNotFound_{_("Source item %x not found" )};
};

//---------------------------------------------------------------------------------------------------------------
/* ___________________________
   |                         |
   | Multithreaded File Copy |
   |_________________________|

           ----------------     =================
           |Async Callback| <-- |Worker Thread 1|
           ----------------     ====================
                 /|\               |Worker Thread 2|
                  |                =================
             =============           |   ...    |
  GUI    <-- |Main Thread|          \|/        \|/
Callback     =============     -------------------------------
                               |Workload | folders to process|
                               -------------------------------

Notes: - All threads share a single mutex, unlocked only during file I/O => do NOT require file_hierarchy.cpp classes to be thread-safe (i.e. internally synchronized)!
       - Workload holds (folder-level-) items in buckets associated with each worker thread (FTP scenario: avoid CWDs)
       - If a worker is idle, its Workload bucket is empty and no more folders to anaylze: steal from other buckets (=> take half of largest bucket)
       - Maximize opportunity for parallelization ASAP: Workload buckets serve folder-items *before* files/symlinks => reduce risk of work-stealing
       - Memory consumption: "Folders to process" may grow indefinitely; however: test case "C:\", 100.000 folders => worst case only ~ 800kB on x64
*/

class Workload
{
public:
    Workload(FolderPairSyncer::PassNo pass, BaseFolderPair& baseFolder, size_t threadCount, AsyncCallback& acb) :
        pass_(pass), acb_(acb), workload_(threadCount), foldersToProcess_{ &baseFolder } { assert(threadCount > 0); }

    //blocking call: context of worker thread
    std::function<void(FolderPairSyncer& fps)> getNext(size_t threadIdx) //throw ThreadInterruption
    {
        std::unique_lock<std::mutex> dummy(lockWork_);
        for (;;)
        {
            for (;;)
            {
                if (!workload_[threadIdx].empty())
                {
                    auto workItem = workload_[threadIdx].    back(); //yes, no strong exception guarantee (std::bad_alloc)
                    /**/            workload_[threadIdx].pop_back(); //
                    return workItem;
                }
                if (!foldersToProcess_.empty())
                {
                    ContainerObject& hierObj = *foldersToProcess_.    back();
                    /**/                        foldersToProcess_.pop_back();

                    //thread-safe thanks to std::mutex singleThread:
                    FolderPairSyncer::appendFolderLevelWorkItems(pass_, hierObj, //in
                                                                 workload_[threadIdx], //out, appending
                                                                 foldersToProcess_);   //
                }
                else
                    break;
            }

            //steal half of largest workload from other thread
            WorkItems& items = *std::max_element(workload_.begin(), workload_.end(), [](const WorkItems& lhs, const WorkItems& rhs) { return lhs.size() < rhs.size(); });
            if (!items.empty()) //=> != workload_[threadIdx]
            {
                size_t pos = 0;
                erase_if(items, [&](const WorkItem& wi)
                {
                    if (pos++ % 2 == 0)
                    {
                        workload_[threadIdx].push_back(wi);
                        return true;
                    }
                    return false;
                });
                auto workItem = workload_[threadIdx].    back(); //yes, no strong exception guarantee (std::bad_alloc)
                /**/            workload_[threadIdx].pop_back(); //
                return workItem;
            }

            if (++idleThreads_ == workload_.size())
                acb_.notifyAllDone(); //noexcept
            ZEN_ON_SCOPE_EXIT(--idleThreads_);

            acb_.notifyWorkEnd(threadIdx);
            ZEN_ON_SCOPE_EXIT(acb_.notifyWorkBegin(threadIdx));

            auto haveNewWork = [&] { return !foldersToProcess_.empty() || std::any_of(workload_.begin(), workload_.end(), [](const WorkItems& wi) { return !wi.empty(); }); };

            interruptibleWait(conditionNewWork_, dummy, [&] { return haveNewWork(); }); //throw ThreadInterruption
            //it's sufficient to notify condition in addFolderToProcess() only (as long as we use std::condition_variable::notify_all())
        }
    }

    void addFolderToProcess(ContainerObject& folder)
    {
        {
            std::lock_guard<std::mutex> dummy(lockWork_);
            foldersToProcess_.push_back(&folder);
        }
        conditionNewWork_.notify_all();
    }

private:
    Workload           (const Workload&) = delete;
    Workload& operator=(const Workload&) = delete;

    using WorkItem  = std::function<void(FolderPairSyncer& fps) /*throw ThreadInterruption*/>;
    using WorkItems = std::vector<WorkItem>;

    const FolderPairSyncer::PassNo pass_;
    AsyncCallback& acb_;

    std::mutex lockWork_;
    std::condition_variable conditionNewWork_;

    size_t idleThreads_ = 0;

    std::vector<WorkItems> workload_; //thread-specific buckets
    std::vector<ContainerObject*> foldersToProcess_;
};


void FolderPairSyncer::runPass(PassNo pass, SyncCtx& syncCtx, BaseFolderPair& baseFolder, ProcessCallback& cb) //throw X
{
    const size_t threadCount = std::max<size_t>(syncCtx.threadCount, 1);

    std::mutex singleThread; //only a single worker thread may run at a time, except for parallel file I/O

    AsyncCallback acb(threadCount);                        //
    Workload workload(pass, baseFolder, threadCount, acb); //manage life time: enclose InterruptibleThread's!!!

    FixedList<InterruptibleThread> worker;

    ZEN_ON_SCOPE_EXIT( for (InterruptibleThread& wt : worker) wt.join(); );
    ZEN_ON_SCOPE_EXIT( for (InterruptibleThread& wt : worker) wt.interrupt(); ); //interrupt all first, then join

    for (size_t threadIdx = 0; threadIdx < threadCount; ++threadIdx)
        worker.emplace_back([fps = FolderPairSyncer(syncCtx, workload, singleThread, threadIdx, acb), threadIdx, &singleThread, &acb, &workload]() mutable
    {
        setCurrentThreadName(("Sync Worker[" + numberTo<std::string>(threadIdx) + "]").c_str());

        acb.notifyWorkBegin(threadIdx);
        ZEN_ON_SCOPE_EXIT(acb.notifyWorkEnd(threadIdx));

        while (/*blocking call:*/ std::function<void(FolderPairSyncer& fps)> workItem = workload.getNext(threadIdx)) //throw ThreadInterruption
        {
            std::lock_guard<std::mutex> dummy(singleThread); //protect ALL accesses to "fps" and workItem execution!
            workItem(fps); //throw ThreadInterruption
        }
    });

    acb.waitUntilDone(UI_UPDATE_INTERVAL / 2 /*every ~50 ms*/, cb); //throw X
}


void FolderPairSyncer::appendFolderLevelWorkItems(PassNo pass, ContainerObject& hierObj, //in
                                                  std::vector<std::function<void(FolderPairSyncer& fps)>>& workItems,        //out
                                                  std::vector<ContainerObject*                          >& foldersToProcess) //
{
    const size_t itemCountOld   = workItems.size();
    const size_t folderCountOld = foldersToProcess.size();

    //synchronize folders:
    for (FolderPair& folder : hierObj.refSubFolders())
        if (pass == getPass(folder))
            workItems.push_back([&folder](FolderPairSyncer& fps)
        {
            tryReportingError([&] { fps.synchronizeFolder(folder); }, fps.threadIdx_, fps.acb_); //throw ThreadInterruption
            fps.workload_.addFolderToProcess(folder);
            warn_static("unnatural processing order!?")
        });
    else
        foldersToProcess.push_back(&folder);

    //synchronize files:
    for (FilePair& file : hierObj.refSubFiles())
        if (pass == PASS_ZERO)
            workItems.push_back([&file](FolderPairSyncer& fps) { fps.prepareFileMove(file); /*throw ThreadInterruption*/ });
    else if (pass == getPass(file))
        workItems.push_back([&file](FolderPairSyncer& fps)
    {
        tryReportingError([&] { fps.synchronizeFile(file); }, fps.threadIdx_, fps.acb_); //throw ThreadInterruption
    });

    //synchronize symbolic links:
    for (SymlinkPair& symlink : hierObj.refSubLinks())
        if (pass == getPass(symlink))
            workItems.push_back([&symlink](FolderPairSyncer& fps)
        {
            tryReportingError([&] { fps.synchronizeLink(symlink); }, fps.threadIdx_, fps.acb_); //throw ThreadInterruption
        });

    //ensure natural processing order despite LIFO:
    std::reverse(workItems       .begin() + itemCountOld,   workItems       .end());
    std::reverse(foldersToProcess.begin() + folderCountOld, foldersToProcess.end());
}


/*
__________________________
|Move algorithm, 0th pass|
--------------------------
1. loop over hierarchy and find "move source"

2. check whether parent directory of "move source" is going to be deleted or location of "move source" may lead to name clash with other dir/symlink
   -> no:  delay move until 2nd pass

3. create move target's parent directory recursively + execute move
   do we have name clash?
   -> prepare a 2-step move operation: 1. move source to base and update "move target" accordingly 2. delay move until 2nd pass

4. If any of the operations above did not succeed (even after retry), update statistics and revert to "copy + delete"
   Note: first pass may delete "move source"!!!

__________________
|killer-scenarios|
------------------
propagate the following move sequences:
I) a -> a/a      caveat sync'ing parent directory first leads to circular dependency!

II) a/a -> a     caveat: fixing name clash will remove source!

III) c -> d      caveat: move-sequence needs to be processed in correct order!
     b -> c/b
     a -> b/a
*/

template <class List> inline
bool haveNameClash(const Zstring& shortname, List& m)
{
    return std::any_of(m.begin(), m.end(),
    [&](const typename List::value_type& obj) { return equalFilePath(obj.getPairItemName(), shortname); });
}


template <SelectedSide side>
void FolderPairSyncer::setup2StepMove(FilePair& sourceObj, //throw FileError, ThreadInterruption
                                      FilePair& targetObj)
{
    //generate (hopefully) unique file name to avoid clashing with some remnant ffs_tmp file
    const Zstring shortGuid = printNumber<Zstring>(Zstr("%04x"), static_cast<unsigned int>(getCrc16(generateGUID())));
    const Zstring fileName = sourceObj.getItemName<side>();
    auto it = find_last(fileName.begin(), fileName.end(), Zchar('.')); //gracefully handle case of missing "."

    const Zstring sourceRelPathTmp = Zstring(fileName.begin(), it) + Zchar('.') + shortGuid + AFS::TEMP_FILE_ENDING;
    //-------------------------------------------------------------------------------------------
    //this could still lead to a name-clash in obscure cases, if some file exists on the other side with
    //the very same (.ffs_tmp) name and is copied before the second step of the move is executed
    //good news: even in this pathologic case, this may only prevent the copy of the other file, but not this move

    const AbstractPath sourcePathTmp = AFS::appendRelPath(sourceObj.base().getAbstractPath<side>(), sourceRelPathTmp);

    reportInfo(txtMovingFileXtoY_, //ThreadInterruption
               AFS::getDisplayPath(sourceObj.getAbstractPath<side>()),
               AFS::getDisplayPath(sourcePathTmp));

    parallel::renameItem(sourceObj.getAbstractPath<side>(), sourcePathTmp, singleThread_); //throw FileError, (ErrorDifferentVolume)

    //TODO: prepare2StepMove: consider ErrorDifferentVolume! e.g. symlink aliasing!

    //update file hierarchy
    FilePair& tempFile = sourceObj.base().addSubFile<side>(afterLast(sourceRelPathTmp, FILE_NAME_SEPARATOR, IF_MISSING_RETURN_ALL), sourceObj.getAttributes<side>());
    static_assert(IsSameType<FixedList<FilePair>, ContainerObject::FileList>::value,
                  "ATTENTION: we're adding to the file list WHILE looping over it! This is only working because FixedList iterators are not invalidated by insertion!");
    sourceObj.removeObject<side>(); //remove only *after* evaluating "sourceObj, side"!
    //note: this new item is *not* considered at the end of 0th pass because "!sourceWillBeDeleted && !haveNameClash"

    //prepare move in second pass
    tempFile.setSyncDir(side == LEFT_SIDE ? SyncDirection::LEFT : SyncDirection::RIGHT);

    targetObj.setMoveRef(tempFile .getId());
    tempFile .setMoveRef(targetObj.getId());

    //NO statistics update!
    interruptionPoint(); //throw ThreadInterruption
}


//return "false" on name clash
bool FolderPairSyncer::createParentFolder(FileSystemObject& fsObj) //throw FileError, ThreadInterruption
{
    if (auto parentFolder = dynamic_cast<FolderPair*>(&fsObj.parent()))
    {
        if (!createParentFolder(*parentFolder))
            return false;

        //detect (and try to resolve) file type conflicts: 1. symlinks 2. files
        const Zstring& shortname = parentFolder->getPairItemName();
        if (haveNameClash(shortname, parentFolder->parent().refSubLinks()) ||
            haveNameClash(shortname, parentFolder->parent().refSubFiles()))
            return false;

        //in this context "parentFolder" cannot be scheduled for deletion since it contains a "move target"!
        //note: if parentFolder were deleted, we'd end up destroying "fsObj"!
        assert(parentFolder->getSyncOperation() != SO_DELETE_LEFT &&
               parentFolder->getSyncOperation() != SO_DELETE_RIGHT);

        synchronizeFolder(*parentFolder); //throw FileError, ThreadInterruption
    }
    return true;
}


template <SelectedSide side>
void FolderPairSyncer::resolveMoveConflicts(FilePair& sourceFile, //throw FileError, ThreadInterruption
                                            FilePair& targetFile)
{
    assert((sourceFile.getSyncOperation() == SO_MOVE_LEFT_FROM  && targetFile.getSyncOperation() == SO_MOVE_LEFT_TO  && side == LEFT_SIDE) ||
           (sourceFile.getSyncOperation() == SO_MOVE_RIGHT_FROM && targetFile.getSyncOperation() == SO_MOVE_RIGHT_TO && side == RIGHT_SIDE));

    const bool sourceWillBeDeleted = [&]
    {
        if (auto parentFolder = dynamic_cast<const FolderPair*>(&sourceFile.parent()))
        {
            switch (parentFolder->getSyncOperation()) //evaluate comparison result and sync direction
            {
                case SO_DELETE_LEFT:
                case SO_DELETE_RIGHT:
                    return true; //we need to do something about it
                case SO_MOVE_LEFT_FROM:
                case SO_MOVE_RIGHT_FROM:
                case SO_MOVE_LEFT_TO:
                case SO_MOVE_RIGHT_TO:
                case SO_OVERWRITE_LEFT:
                case SO_OVERWRITE_RIGHT:
                case SO_CREATE_NEW_LEFT:
                case SO_CREATE_NEW_RIGHT:
                case SO_DO_NOTHING:
                case SO_EQUAL:
                case SO_UNRESOLVED_CONFLICT:
                case SO_COPY_METADATA_TO_LEFT:
                case SO_COPY_METADATA_TO_RIGHT:
                    break;
            }
        }
        return false;
    }();

    auto haveNameClash = [](const FilePair& file)
    {
        return ::haveNameClash(file.getPairItemName(), file.parent().refSubLinks()) ||
               ::haveNameClash(file.getPairItemName(), file.parent().refSubFolders());
    };

    if (sourceWillBeDeleted || haveNameClash(sourceFile))
    {
        //prepare for move now: - revert to 2-step move on name clashes
        if (haveNameClash(targetFile) ||
            !createParentFolder(targetFile)) //throw FileError, ThreadInterruption
            return setup2StepMove<side>(sourceFile, targetFile); //throw FileError, ThreadInterruption

        //finally start move! this should work now:
        synchronizeFile(targetFile); //throw FileError, ThreadInterruption
        //FolderPairSyncer::synchronizeFileInt() is *not* expecting SO_MOVE_LEFT_FROM/SO_MOVE_RIGHT_FROM => start move from targetFile, not sourceFile!
    }
    //else: sourceFile will not be deleted, and is not standing in the way => delay to second pass
    //note: this case may include new "move sources" from two-step sub-routine!!!
}


void FolderPairSyncer::prepareFileMove(FilePair& file) //throw ThreadInterruption
{
    const SyncOperation syncOp = file.getSyncOperation();
    switch (syncOp) //evaluate comparison result and sync direction
    {
        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
            if (FilePair* targetObj = dynamic_cast<FilePair*>(FileSystemObject::retrieve(file.getMoveRef())))
            {
                FilePair* sourceObj = &file;
                assert(dynamic_cast<FilePair*>(FileSystemObject::retrieve(targetObj->getMoveRef())) == sourceObj);

                Opt<std::wstring> errMsg = tryReportingError([&] //throw ThreadInterruption
                {
                    if (syncOp == SO_MOVE_LEFT_FROM)
                        resolveMoveConflicts<LEFT_SIDE>(*sourceObj, *targetObj); //throw FileError, ThreadInterruption
                    else
                        resolveMoveConflicts<RIGHT_SIDE>(*sourceObj, *targetObj); //
                }, threadIdx_, acb_); //throw ThreadInterruption

                if (errMsg)
                {
                    //move operation has failed! We cannot allow to continue and have move source's parent directory deleted, messing up statistics!
                    // => revert to ordinary "copy + delete"

                    auto getStats = [&]() -> std::pair<int, int64_t>
                    {
                        SyncStatistics statSrc(*sourceObj);
                        SyncStatistics statTrg(*targetObj);
                        return { getCUD(statSrc) + getCUD(statTrg), statSrc.getBytesToProcess() + statTrg.getBytesToProcess() };
                    };

                    const auto statBefore = getStats();
                    sourceObj->setMoveRef(nullptr);
                    targetObj->setMoveRef(nullptr);
                    const auto statAfter = getStats();
                    //fix statistics total to match "copy + delete"
                    acb_.updateDataTotal(statAfter.first - statBefore.first, statAfter.second - statBefore.second); //noexcept
                }
            }
            else assert(false);
            break;

        case SO_MOVE_LEFT_TO:  //it's enough to try each move-pair *once*
        case SO_MOVE_RIGHT_TO: //
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            break;
    }
}

//---------------------------------------------------------------------------------------------------------------

//1st, 2nd pass requirements:
// - avoid disk space shortage: 1. delete files, 2. overwrite big with small files first
// - support change in type: overwrite file by directory, symlink by file, ect.

inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const FilePair& file)
{
    switch (file.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PASS_ONE;

        case SO_OVERWRITE_LEFT:
            return file.getFileSize<LEFT_SIDE>() > file.getFileSize<RIGHT_SIDE>() ? PASS_ONE : PASS_TWO;

        case SO_OVERWRITE_RIGHT:
            return file.getFileSize<LEFT_SIDE>() < file.getFileSize<RIGHT_SIDE>() ? PASS_ONE : PASS_TWO;

        case SO_MOVE_LEFT_FROM:  //
        case SO_MOVE_RIGHT_FROM: // [!]
            return PASS_NEVER;
        case SO_MOVE_LEFT_TO:  //
        case SO_MOVE_RIGHT_TO: //make sure 2-step move is processed in second pass, after move *target* parent directory was created!
            return PASS_TWO;

        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PASS_TWO;

        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PASS_NEVER;
    }
    assert(false);
    return PASS_NEVER; //dummy
}


inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const SymlinkPair& link)
{
    switch (link.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PASS_ONE; //make sure to delete symlinks in first pass, and equally named file or dir in second pass: usecase "overwrite symlink with regular file"!

        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PASS_TWO;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PASS_NEVER;
    }
    assert(false);
    return PASS_NEVER; //dummy
}


inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const FolderPair& folder)
{
    switch (folder.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PASS_ONE;

        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PASS_TWO;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PASS_NEVER;
    }
    assert(false);
    return PASS_NEVER; //dummy
}

//---------------------------------------------------------------------------------------------------------------

inline
void FolderPairSyncer::synchronizeFile(FilePair& file) //throw FileError, ThreadInterruption
{
    const SyncOperation syncOp = file.getSyncOperation();

    if (Opt<SelectedSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == LEFT_SIDE)
            synchronizeFileInt<LEFT_SIDE>(file, syncOp);
        else
            synchronizeFileInt<RIGHT_SIDE>(file, syncOp);
    }
}


template <SelectedSide sideTrg>
void FolderPairSyncer::synchronizeFileInt(FilePair& file, SyncOperation syncOp) //throw FileError, ThreadInterruption
{
    static const SelectedSide sideSrc = OtherSide<sideTrg>::result;
    DeletionHandling& delHandlingTrg = SelectParam<sideTrg>::ref(delHandlingLeft_, delHandlingRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&file.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            //can't use "getAbstractPath<sideTrg>()" as file name is not available!
            const AbstractPath targetPath = file.getAbstractPath<sideTrg>();
            reportInfo(txtCreatingFile_, AFS::getDisplayPath(targetPath));

            AsyncItemStatReporter statReporter(1, file.getFileSize<sideSrc>(), threadIdx_, acb_);
            try
            {
                const AFS::FileCopyResult result = copyFileWithCallback({ file.getAbstractPath<sideSrc>(), file.getAttributes<sideSrc>() },
                                                                        targetPath,
                                                                        nullptr, //onDeleteTargetFile: nothing to delete; if existing: undefined behavior! (fail/overwrite/auto-rename)
                                                                        statReporter); //throw FileError
                if (result.errorModTime)
                    errorsModTime_.push_back(*result.errorModTime); //show all warnings later as a single message

                statReporter.reportDelta(1, 0);

                //update FilePair
                file.setSyncedTo<sideTrg>(file.getItemName<sideSrc>(), result.fileSize,
                                          result.modTime, //target time set from source
                                          result.modTime,
                                          result.targetFileId,
                                          result.sourceFileId,
                                          false, file.isFollowedSymlink<sideSrc>());
            }
            catch (FileError&)
            {
                bool sourceWasDeleted = false;
                try { sourceWasDeleted = !parallel::getItemTypeIfExists(file.getAbstractPath<sideSrc>(), singleThread_); /*throw FileError*/ }
                catch (FileError&) {} //previous exception is more relevant
                //do not check on type (symlink, file, folder) -> if there is a type change, FFS should not be quiet about it!

                if (sourceWasDeleted)
                {
                    //even if the source item does not exist anymore, significant I/O work was done => report
                    statReporter.reportDelta(1, 0);
                    reportInfo(txtSourceItemNotFound_, AFS::getDisplayPath(file.getAbstractPath<sideSrc>()));

                    file.removeObject<sideSrc>(); //source deleted meanwhile...nothing was done (logical point of view!)
                }
                else
                    throw;
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            reportInfo(delHandlingTrg.getTxtRemovingFile(), AFS::getDisplayPath(file.getAbstractPath<sideTrg>()));
            {
                AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);

                delHandlingTrg.removeFileWithCallback({ file.getAbstractPath<sideTrg>(), file.getAttributes<sideTrg>() },
                                                      file.getPairRelativePath(), statReporter, singleThread_); //throw FileError, X
                file.removeObject<sideTrg>(); //update FilePair
            }
            break;

        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            if (FilePair* moveFrom = dynamic_cast<FilePair*>(FileSystemObject::retrieve(file.getMoveRef())))
            {
                FilePair* moveTo = &file;

                assert((moveFrom->getSyncOperation() == SO_MOVE_LEFT_FROM  && moveTo->getSyncOperation() == SO_MOVE_LEFT_TO  && sideTrg == LEFT_SIDE) ||
                       (moveFrom->getSyncOperation() == SO_MOVE_RIGHT_FROM && moveTo->getSyncOperation() == SO_MOVE_RIGHT_TO && sideTrg == RIGHT_SIDE));

                const AbstractPath pathFrom = moveFrom->getAbstractPath<sideTrg>();
                const AbstractPath pathTo   = moveTo  ->getAbstractPath<sideTrg>();

                reportInfo(txtMovingFileXtoY_, AFS::getDisplayPath(pathFrom), AFS::getDisplayPath(pathTo));

                AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);

                //TODO: synchronizeFileInt: consider ErrorDifferentVolume! e.g. symlink aliasing!

                parallel::renameItem(pathFrom, pathTo, singleThread_); //throw FileError, (ErrorDifferentVolume)

                statReporter.reportDelta(1, 0);

                //update FilePair
                assert(moveFrom->getFileSize<sideTrg>() == moveTo->getFileSize<sideSrc>());
                moveTo->setSyncedTo<sideTrg>(moveTo->getItemName<sideSrc>(), moveTo->getFileSize<sideSrc>(),
                                             moveFrom->getLastWriteTime<sideTrg>(), //awkward naming! moveFrom is renamed on "sideTrg" side!
                                             moveTo  ->getLastWriteTime<sideSrc>(),
                                             moveFrom->getFileId<sideTrg>(),
                                             moveTo  ->getFileId<sideSrc>(),
                                             moveFrom->isFollowedSymlink<sideTrg>(),
                                             moveTo  ->isFollowedSymlink<sideSrc>());
                moveFrom->removeObject<sideTrg>(); //remove only *after* evaluating "moveFrom, sideTrg"!
            }
            else (assert(false));
            break;

        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        {
            //respect differences in case of source object:
            const AbstractPath targetPathLogical = AFS::appendRelPath(file.parent().getAbstractPath<sideTrg>(), file.getItemName<sideSrc>());

            AbstractPath targetPathResolvedOld = file.getAbstractPath<sideTrg>(); //support change in case when syncing to case-sensitive SFTP on Windows!
            AbstractPath targetPathResolvedNew = targetPathLogical;
            if (file.isFollowedSymlink<sideTrg>()) //follow link when updating file rather than delete it and replace with regular file!!!
                targetPathResolvedOld = targetPathResolvedNew = parallel::getSymlinkResolvedPath(file.getAbstractPath<sideTrg>(), singleThread_); //throw FileError

            reportInfo(txtUpdatingFile_, AFS::getDisplayPath(targetPathResolvedOld));

            AsyncItemStatReporter statReporter(1, file.getFileSize<sideSrc>(), threadIdx_, acb_);

            if (file.isFollowedSymlink<sideTrg>()) //since we follow the link, we need to sync case sensitivity of the link manually!
                if (file.getItemName<sideTrg>() != file.getItemName<sideSrc>()) //have difference in case?
                    parallel::renameItem(file.getAbstractPath<sideTrg>(), targetPathLogical, singleThread_); //throw FileError, (ErrorDifferentVolume)

            auto onDeleteTargetFile = [&] //delete target at appropriate time
            {
                //reportStatus(this->delHandlingTrg.getTxtRemovingFile(), AFS::getDisplayPath(targetPathResolvedOld)); -> superfluous/confuses user

                FileAttributes followedTargetAttr = file.getAttributes<sideTrg>();
                followedTargetAttr.isFollowedSymlink = false;

                delHandlingTrg.removeFileWithCallback({ targetPathResolvedOld, followedTargetAttr }, file.getPairRelativePath(), statReporter, singleThread_); //throw FileError, X
                //no (logical) item count update desired - but total byte count may change, e.g. move(copy) old file to versioning dir
                statReporter.reportDelta(-1, 0); //undo item stats reporting within DeletionHandling::removeFileWithCallback()

                //file.removeObject<sideTrg>(); -> doesn't make sense for isFollowedSymlink(); "file, sideTrg" evaluated below!

                //if fail-safe file copy is active, then the next operation will be a simple "rename"
                //=> don't risk reportStatus() throwing ThreadInterruption() leaving the target deleted rather than updated!
                //=> if failSafeFileCopy_ : don't run callbacks that could throw
            };

            const AFS::FileCopyResult result = copyFileWithCallback({ file.getAbstractPath<sideSrc>(), file.getAttributes<sideSrc>() },
                                                                    targetPathResolvedNew,
                                                                    onDeleteTargetFile,
                                                                    statReporter); //throw FileError
            if (result.errorModTime)
                errorsModTime_.push_back(*result.errorModTime); //show all warnings later as a single message

            statReporter.reportDelta(1, 0); //we model "delete + copy" as ONE logical operation

            //update FilePair
            file.setSyncedTo<sideTrg>(file.getItemName<sideSrc>(), result.fileSize,
                                      result.modTime, //target time set from source
                                      result.modTime,
                                      result.targetFileId,
                                      result.sourceFileId,
                                      file.isFollowedSymlink<sideTrg>(),
                                      file.isFollowedSymlink<sideSrc>());
        }
        break;

        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            //harmonize with file_hierarchy.cpp::getSyncOpDescription!!
            reportInfo(txtUpdatingAttributes_, AFS::getDisplayPath(file.getAbstractPath<sideTrg>()));
            {
                AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);

                assert(file.getItemName<sideTrg>() != file.getItemName<sideSrc>());
                if (file.getItemName<sideTrg>() != file.getItemName<sideSrc>()) //have difference in case?
                    parallel::renameItem(file.getAbstractPath<sideTrg>(), //throw FileError, (ErrorDifferentVolume)
                                         AFS::appendRelPath(file.parent().getAbstractPath<sideTrg>(), file.getItemName<sideSrc>()), singleThread_);

#if 0 //changing file time without copying content is not justified after CompareVariant::SIZE finds "equal" files! similar issue with CompareVariant::TIME_SIZE and FileTimeTolerance == -1
                //Bonus: some devices don't support setting (precise) file times anyway, e.g. FAT or MTP!
                if (file.getLastWriteTime<sideTrg>() != file.getLastWriteTime<sideSrc>())
                    //- no need to call sameFileTime() or respect 2 second FAT/FAT32 precision in this comparison
                    //- do NOT read *current* source file time, but use buffered value which corresponds to time of comparison!
                    parallel::setModTime(file.getAbstractPath<sideTrg>(), file.getLastWriteTime<sideSrc>()); //throw FileError
#endif
                statReporter.reportDelta(1, 0);

                //-> both sides *should* be completely equal now...
                assert(file.getFileSize<sideTrg>() == file.getFileSize<sideSrc>());
                file.setSyncedTo<sideTrg>(file.getItemName<sideSrc>(), file.getFileSize<sideSrc>(),
                                          file.getLastWriteTime<sideTrg>(),
                                          file.getLastWriteTime<sideSrc>(),
                                          file.getFileId       <sideTrg>(),
                                          file.getFileId       <sideSrc>(),
                                          file.isFollowedSymlink<sideTrg>(),
                                          file.isFollowedSymlink<sideSrc>());
            }
            break;

        case SO_MOVE_LEFT_FROM:  //use SO_MOVE_LEFT_TO/SO_MOVE_RIGHT_TO to execute move:
        case SO_MOVE_RIGHT_FROM: //=> makes sure parent directory has been created
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            assert(false); //should have been filtered out by FolderPairSyncer::getPass()
            return; //no update on processed data!
    }

    interruptionPoint(); //throw ThreadInterruption
}


inline
void FolderPairSyncer::synchronizeLink(SymlinkPair& link) //throw FileError, ThreadInterruption
{
    const SyncOperation syncOp = link.getSyncOperation();

    if (Opt<SelectedSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == LEFT_SIDE)
            synchronizeLinkInt<LEFT_SIDE>(link, syncOp);
        else
            synchronizeLinkInt<RIGHT_SIDE>(link, syncOp);
    }
}


template <SelectedSide sideTrg>
void FolderPairSyncer::synchronizeLinkInt(SymlinkPair& symlink, SyncOperation syncOp) //throw FileError, ThreadInterruption
{
    warn_static("test constexpr compiler conformance")
    static const SelectedSide sideSrc = OtherSide<sideTrg>::result;
    DeletionHandling& delHandlingTrg = SelectParam<sideTrg>::ref(delHandlingLeft_, delHandlingRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&symlink.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            const AbstractPath targetPath = symlink.getAbstractPath<sideTrg>();
            reportInfo(txtCreatingLink_, AFS::getDisplayPath(targetPath));

            AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);
            try
            {
                parallel::copySymlink(symlink.getAbstractPath<sideSrc>(), targetPath, copyFilePermissions_, singleThread_); //throw FileError

                statReporter.reportDelta(1, 0);

                //update SymlinkPair
                symlink.setSyncedTo<sideTrg>(symlink.getItemName<sideSrc>(),
                                             symlink.getLastWriteTime<sideSrc>(), //target time set from source
                                             symlink.getLastWriteTime<sideSrc>());

            }
            catch (FileError&)
            {
                bool sourceWasDeleted = false;
                try { sourceWasDeleted = !parallel::getItemTypeIfExists(symlink.getAbstractPath<sideSrc>(), singleThread_); /*throw FileError*/ }
                catch (FileError&) {} //previous exception is more relevant
                //do not check on type (symlink, file, folder) -> if there is a type change, FFS should not be quiet about it!

                if (sourceWasDeleted)
                {
                    //even if the source item does not exist anymore, significant I/O work was done => report
                    statReporter.reportDelta(1, 0);
                    reportInfo(txtSourceItemNotFound_, AFS::getDisplayPath(symlink.getAbstractPath<sideSrc>()));

                    symlink.removeObject<sideSrc>(); //source deleted meanwhile...nothing was done (logical point of view!)
                }
                else
                    throw;
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            reportInfo(delHandlingTrg.getTxtRemovingSymLink(), AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>()));
            {
                AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);

                delHandlingTrg.removeLinkWithCallback(symlink.getAbstractPath<sideTrg>(), symlink.getPairRelativePath(), statReporter, singleThread_); //throw FileError, X

                symlink.removeObject<sideTrg>(); //update SymlinkPair
            }
            break;

        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
            reportInfo(txtUpdatingLink_, AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>()));
            {
                AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);

                //reportStatus(delHandlingTrg.getTxtRemovingSymLink(), AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>()));
                delHandlingTrg.removeLinkWithCallback(symlink.getAbstractPath<sideTrg>(), symlink.getPairRelativePath(), statReporter, singleThread_); //throw FileError, X
                statReporter.reportDelta(-1, 0); //undo item stats reporting within DeletionHandling::removeLinkWithCallback()

                //symlink.removeObject<sideTrg>(); -> "symlink, sideTrg" evaluated below!

                //=> don't risk reportStatus() throwing ThreadInterruption() leaving the target deleted rather than updated:
                //reportStatus(txtUpdatingLink_, AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>())); //restore status text

                parallel::copySymlink(symlink.getAbstractPath<sideSrc>(),
                                      AFS::appendRelPath(symlink.parent().getAbstractPath<sideTrg>(), symlink.getItemName<sideSrc>()), //respect differences in case of source object
                                      copyFilePermissions_, singleThread_); //throw FileError

                statReporter.reportDelta(1, 0); //we model "delete + copy" as ONE logical operation

                //update SymlinkPair
                symlink.setSyncedTo<sideTrg>(symlink.getItemName<sideSrc>(),
                                             symlink.getLastWriteTime<sideSrc>(), //target time set from source
                                             symlink.getLastWriteTime<sideSrc>());
            }
            break;

        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            reportInfo(txtUpdatingAttributes_, AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>()));
            {
                AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);

                if (symlink.getItemName<sideTrg>() != symlink.getItemName<sideSrc>()) //have difference in case?
                    parallel::renameItem(symlink.getAbstractPath<sideTrg>(), //throw FileError, (ErrorDifferentVolume)
                                         AFS::appendRelPath(symlink.parent().getAbstractPath<sideTrg>(), symlink.getItemName<sideSrc>()), singleThread_);

                //if (symlink.getLastWriteTime<sideTrg>() != symlink.getLastWriteTime<sideSrc>())
                //    //- no need to call sameFileTime() or respect 2 second FAT/FAT32 precision in this comparison
                //    //- do NOT read *current* source file time, but use buffered value which corresponds to time of comparison!
                //    parallel::setModTimeSymlink(symlink.getAbstractPath<sideTrg>(), symlink.getLastWriteTime<sideSrc>()); //throw FileError

                statReporter.reportDelta(1, 0);

                //-> both sides *should* be completely equal now...
                symlink.setSyncedTo<sideTrg>(symlink.getItemName<sideSrc>(),
                                             symlink.getLastWriteTime<sideTrg>(), //target time set from source
                                             symlink.getLastWriteTime<sideSrc>());
            }
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            assert(false); //should have been filtered out by FolderPairSyncer::getPass()
            return; //no update on processed data!
    }

    interruptionPoint(); //throw ThreadInterruption
}


inline
void FolderPairSyncer::synchronizeFolder(FolderPair& folder) //throw FileError, ThreadInterruption
{
    const SyncOperation syncOp = folder.getSyncOperation();

    if (Opt<SelectedSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == LEFT_SIDE)
            synchronizeFolderInt<LEFT_SIDE>(folder, syncOp);
        else
            synchronizeFolderInt<RIGHT_SIDE>(folder, syncOp);
    }
}


template <SelectedSide sideTrg>
void FolderPairSyncer::synchronizeFolderInt(FolderPair& folder, SyncOperation syncOp) //throw FileError, ThreadInterruption
{
    static const SelectedSide sideSrc = OtherSide<sideTrg>::result;
    DeletionHandling& delHandlingTrg = SelectParam<sideTrg>::ref(delHandlingLeft_, delHandlingRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&folder.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            const AbstractPath targetPath = folder.getAbstractPath<sideTrg>();
            reportInfo(txtCreatingFolder_, AFS::getDisplayPath(targetPath));

            //shallow-"copying" a folder might not fail if source is missing, so we need to check this first:
            if (parallel::getItemTypeIfExists(folder.getAbstractPath<sideSrc>(), singleThread_)) //throw FileError
            {
                AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);
                try
                {
                    //target existing: undefined behavior! (fail/overwrite)
                    parallel::copyNewFolder(folder.getAbstractPath<sideSrc>(), targetPath, copyFilePermissions_, singleThread_); //throw FileError
                }
                catch (FileError&)
                {
                    bool folderAlreadyExists = false;
                    try { folderAlreadyExists = parallel::getItemType(targetPath, singleThread_) == AFS::ItemType::FOLDER; } /*throw FileError*/ catch (FileError&) {}
                    //previous exception is more relevant

                    if (!folderAlreadyExists)
                        throw;
                }

                statReporter.reportDelta(1, 0);

                //update FolderPair
                folder.setSyncedTo<sideTrg>(folder.getItemName<sideSrc>(),
                                            false, //isSymlinkTrg
                                            folder.isFollowedSymlink<sideSrc>());
            }
            else //source deleted meanwhile...
            {
                const SyncStatistics subStats(folder);
                AsyncItemStatReporter statReporter(1 + getCUD(subStats), subStats.getBytesToProcess(), threadIdx_, acb_);

                //even if the source item does not exist anymore, significant I/O work was done => report
                statReporter.reportDelta(1, 0);
                reportInfo(txtSourceItemNotFound_, AFS::getDisplayPath(folder.getAbstractPath<sideSrc>()));

                //remove only *after* evaluating folder!!
                folder.refSubFiles  ().clear(); //
                folder.refSubLinks  ().clear(); //update FolderPair
                folder.refSubFolders().clear(); //
                folder.removeObject<sideSrc>(); //
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            reportInfo(delHandlingTrg.getTxtRemovingFolder(), AFS::getDisplayPath(folder.getAbstractPath<sideTrg>()));
            {
                const SyncStatistics subStats(folder); //counts sub-objects only!
                AsyncItemStatReporter statReporter(1 + getCUD(subStats), subStats.getBytesToProcess(), threadIdx_, acb_);

                delHandlingTrg.removeDirWithCallback(folder.getAbstractPath<sideTrg>(), folder.getPairRelativePath(), statReporter, singleThread_); //throw FileError, X

                warn_static("perf => not parallel!")

                folder.refSubFiles  ().clear(); //
                folder.refSubLinks  ().clear(); //update FolderPair
                folder.refSubFolders().clear(); //
                folder.removeObject<sideTrg>(); //
            }
            break;

        case SO_OVERWRITE_LEFT:  //possible: e.g. manually-resolved dir-traversal conflict
        case SO_OVERWRITE_RIGHT: //
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            reportInfo(txtUpdatingAttributes_, AFS::getDisplayPath(folder.getAbstractPath<sideTrg>()));
            {
                AsyncItemStatReporter statReporter(1, 0, threadIdx_, acb_);

                assert(folder.getItemName<sideTrg>() != folder.getItemName<sideSrc>());
                if (folder.getItemName<sideTrg>() != folder.getItemName<sideSrc>()) //have difference in case?
                    parallel::renameItem(folder.getAbstractPath<sideTrg>(), //throw FileError, (ErrorDifferentVolume)
                                         AFS::appendRelPath(folder.parent().getAbstractPath<sideTrg>(), folder.getItemName<sideSrc>()), singleThread_);
                //copyFileTimes -> useless: modification time changes with each child-object creation/deletion

                statReporter.reportDelta(1, 0);

                //-> both sides *should* be completely equal now...
                folder.setSyncedTo<sideTrg>(folder.getItemName<sideSrc>(),
                                            folder.isFollowedSymlink<sideTrg>(),
                                            folder.isFollowedSymlink<sideSrc>());
            }
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            assert(false); //should have been filtered out by FolderPairSyncer::getPass()
            return; //no update on processed data!
    }

    interruptionPoint(); //throw ThreadInterruption
}

//###########################################################################################

//returns current attributes of source file
AFS::FileCopyResult FolderPairSyncer::copyFileWithCallback(const FileDescriptor& sourceDescr, //throw FileError
                                                           const AbstractPath& targetPath,
                                                           const std::function<void()>& onDeleteTargetFile, //optional!
                                                           AsyncItemStatReporter& statReporter)
{
    const AbstractPath& sourcePath = sourceDescr.path;
    const AFS::StreamAttributes sourceAttr{ sourceDescr.attr.modTime, sourceDescr.attr.fileSize, sourceDescr.attr.fileId };

    auto copyOperation = [this, &sourceAttr, &targetPath, &onDeleteTargetFile, &statReporter](const AbstractPath& sourcePathTmp)
    {
        //target existing after onDeleteTargetFile(): undefined behavior! (fail/overwrite/auto-rename)
        const AFS::FileCopyResult result = parallel::copyFileTransactional(sourcePathTmp, sourceAttr, //throw FileError, ErrorFileLocked
                                                                           targetPath,
                                                                           copyFilePermissions_,
                                                                           failSafeFileCopy_,
                                                                           [&]
        {
            if (onDeleteTargetFile) //running *outside* singleThread_ lock! => onDeleteTargetFile-callback expects lock being held:
            {
                std::lock_guard<std::mutex> dummy(singleThread_);
                onDeleteTargetFile();
            }
        },
        [&](int64_t bytesDelta) { statReporter.reportDelta(0, bytesDelta); }, //callback runs *outside* singleThread_ lock! => fine
        singleThread_);

        //#################### Verification #############################
        if (verifyCopiedFiles_)
        {
            ZEN_ON_SCOPE_FAIL(try { parallel::removeFilePlain(targetPath, singleThread_); }
            catch (FileError&) {}); //delete target if verification fails

            reportInfo(txtVerifyingFile_, AFS::getDisplayPath(targetPath));

            //callback runs *outside* singleThread_ lock! => fine
            auto verifyCallback = [&](int64_t bytesDelta) { interruptionPoint(); /*throw ThreadInterruption*/ };

            parallel::verifyFiles(sourcePathTmp, targetPath, verifyCallback, singleThread_); //throw FileError
        }
        //#################### /Verification #############################

        return result;
    };

    return copyOperation(sourcePath);
}

//###########################################################################################

template <SelectedSide side>
bool baseFolderDrop(BaseFolderPair& baseFolder, int folderAccessTimeout, ProcessCallback& callback)
{
    const AbstractPath folderPath = baseFolder.getAbstractPath<side>();

    if (baseFolder.isAvailable<side>())
        if (Opt<std::wstring> errMsg = tryReportingError([&]
    {
        const FolderStatus status = getFolderStatusNonBlocking({ folderPath }, {} /*deviceParallelOps*/,
        folderAccessTimeout, false /*allowUserInteraction*/, callback);

            static_assert(IsSameType<decltype(status.failedChecks.begin()->second), FileError>::value, "");
            if (!status.failedChecks.empty())
                throw status.failedChecks.begin()->second;

            if (status.existing.find(folderPath) == status.existing.end())
                throw FileError(replaceCpy(_("Cannot find folder %x."), L"%x", fmtPath(AFS::getDisplayPath(folderPath))));
            //should really be logged as a "fatal error" if ignored by the user...
        }, callback)) //throw X
    return true;

    return false;
}


template <SelectedSide side> //create base directories first (if not yet existing) -> no symlink or attribute copying!
bool createBaseFolder(BaseFolderPair& baseFolder, int folderAccessTimeout, ProcessCallback& callback) //return false if fatal error occurred
{
    const AbstractPath baseFolderPath = baseFolder.getAbstractPath<side>();

    if (AFS::isNullPath(baseFolderPath))
        return true;

    if (!baseFolder.isAvailable<side>()) //create target directory: user presumably ignored error "dir existing" in order to have it created automatically
    {
        bool temporaryNetworkDrop = false;
        zen::Opt<std::wstring> errMsg = tryReportingError([&]
        {
            const FolderStatus status = getFolderStatusNonBlocking({ baseFolderPath },  {} /*deviceParallelOps*/,
            folderAccessTimeout, false /*allowUserInteraction*/, callback);

            static_assert(IsSameType<decltype(status.failedChecks.begin()->second), FileError>::value, "");
            if (!status.failedChecks.empty())
                throw status.failedChecks.begin()->second;

            if (status.notExisting.find(baseFolderPath) != status.notExisting.end())
            {
                AFS::createFolderIfMissingRecursion(baseFolderPath); //throw FileError
                baseFolder.setAvailable<side>(true); //update our model!
            }
            else
            {
                //TEMPORARY network drop! base directory not found during comparison, but reappears during synchronization
                //=> sync-directions are based on false assumptions! Abort.
                callback.reportFatalError(replaceCpy(_("Target folder %x already existing."), L"%x", fmtPath(AFS::getDisplayPath(baseFolderPath))));
                temporaryNetworkDrop = true;

                //Is it possible we're catching a "false positive" here, could FFS have created the directory indirectly after comparison?
                //  1. deletion handling: recycler       -> no, temp directory created only at first deletion
                //  2. deletion handling: versioning     -> "
                //  3. log file creates containing folder -> no, log only created in batch mode, and only *before* comparison
            }
        }, callback); //throw X
        return !errMsg && !temporaryNetworkDrop;
    }
    return true;
}


enum class FolderPairJobType
{
    PROCESS,
    ALREADY_IN_SYNC,
    SKIP,
};
}


void fff::synchronize(const std::chrono::system_clock::time_point& syncStartTime,
                      bool verifyCopiedFiles,
                      bool copyLockedFiles,
                      bool copyFilePermissions,
                      bool failSafeFileCopy,
                      bool runWithBackgroundPriority,
                      int folderAccessTimeout,
                      const std::vector<FolderPairSyncCfg>& syncConfig,
                      FolderComparison& folderCmp,
                      const std::map<AbstractPath, size_t>& deviceParallelOps,
                      WarningDialogs& warnings,
                      ProcessCallback& callback)
{
    //PERF_START;

    if (syncConfig.size() != folderCmp.size())
        throw std::logic_error("Contract violation! " + std::string(__FILE__) + ":" + numberTo<std::string>(__LINE__));

    //aggregate basic information
    std::vector<SyncStatistics> folderPairStats;
    {
        int     itemsTotal = 0;
        int64_t bytesTotal = 0;
        std::for_each(begin(folderCmp), end(folderCmp),
                      [&](const BaseFolderPair& baseFolder)
        {
            SyncStatistics fpStats(baseFolder);
            itemsTotal += getCUD(fpStats);
            bytesTotal += fpStats.getBytesToProcess();
            folderPairStats.push_back(fpStats);
        });

        //inform about the total amount of data that will be processed from now on
        //keep at beginning so that all gui elements are initialized properly
        callback.initNewPhase(itemsTotal, //throw X
                              bytesTotal,
                              ProcessCallback::PHASE_SYNCHRONIZING);
    }

    //-------------------------------------------------------------------------------

    //specify process and resource handling priorities
    std::unique_ptr<ScheduleForBackgroundProcessing> backgroundPrio;
    if (runWithBackgroundPriority)
        try
        {
            backgroundPrio = std::make_unique<ScheduleForBackgroundProcessing>(); //throw FileError
        }
        catch (const FileError& e) //not an error in this context
        {
            callback.reportInfo(e.toString()); //may throw!
        }

    //prevent operating system going into sleep state
    std::unique_ptr<PreventStandby> noStandby;
    try
    {
        noStandby = std::make_unique<PreventStandby>(); //throw FileError
    }
    catch (const FileError& e) //not an error in this context
    {
        callback.reportInfo(e.toString()); //may throw!
    }

    //-------------------execute basic checks all at once before starting sync--------------------------------------

    std::vector<FolderPairJobType> jobType(folderCmp.size(), FolderPairJobType::PROCESS); //folder pairs may be skipped after fatal errors were found

    std::vector<SyncStatistics::ConflictInfo> unresolvedConflicts;

    std::vector<std::tuple<AbstractPath, const HardFilter*, bool /*write access*/>> readWriteCheckBaseFolders;

    std::vector<std::pair<AbstractPath, AbstractPath>> significantDiffPairs;

    std::vector<std::pair<AbstractPath, std::pair<int64_t, int64_t>>> diskSpaceMissing; //base folder / space required / space available

    //status of base directories which are set to DeletionPolicy::RECYCLER (and contain actual items to be deleted)
    std::map<AbstractPath, bool> recyclerSupported; //expensive to determine on Win XP => buffer + check recycle bin existence only once per base folder!

    std::set<AbstractPath>           verCheckVersioningPaths;
    std::vector<std::pair<AbstractPath, const HardFilter*>> verCheckBaseFolderPaths; //hard filter creates new logical hierarchies for otherwise equal AbstractPath...

    //start checking folder pairs
    for (auto itBase = begin(folderCmp); itBase != end(folderCmp); ++itBase)
    {
        BaseFolderPair& baseFolder = *itBase;
        const size_t folderIndex = itBase - begin(folderCmp);
        const FolderPairSyncCfg& folderPairCfg  = syncConfig     [folderIndex];
        const SyncStatistics&    folderPairStat = folderPairStats[folderIndex];

        //aggregate all conflicts:
        append(unresolvedConflicts, folderPairStat.getConflicts());

        //exclude a few pathological cases (including empty left, right folders)
        if (AFS::equalAbstractPath(baseFolder.getAbstractPath< LEFT_SIDE>(),
                                   baseFolder.getAbstractPath<RIGHT_SIDE>()))
        {
            jobType[folderIndex] = FolderPairJobType::SKIP;
            continue;
        }

        //skip folder pair if there is nothing to do (except for two-way mode and move-detection, where DB files need to be updated)
        //-> skip creating (not yet existing) base directories in particular if there's no need
        if (getCUD(folderPairStat) == 0)
        {
            jobType[folderIndex] = FolderPairJobType::ALREADY_IN_SYNC;
            continue;
        }

        const bool writeLeft = folderPairStat.createCount<LEFT_SIDE>() +
                               folderPairStat.updateCount<LEFT_SIDE>() +
                               folderPairStat.deleteCount<LEFT_SIDE>() > 0;

        const bool writeRight = folderPairStat.createCount<RIGHT_SIDE>() +
                                folderPairStat.updateCount<RIGHT_SIDE>() +
                                folderPairStat.deleteCount<RIGHT_SIDE>() > 0;

        //check for empty target folder paths: this only makes sense if empty field is source (and no DB files need to be created)
        if ((AFS::isNullPath(baseFolder.getAbstractPath< LEFT_SIDE>()) && (writeLeft  || folderPairCfg.saveSyncDB)) ||
            (AFS::isNullPath(baseFolder.getAbstractPath<RIGHT_SIDE>()) && (writeRight || folderPairCfg.saveSyncDB)))
        {
            callback.reportFatalError(_("Target folder input field must not be empty."));
            jobType[folderIndex] = FolderPairJobType::SKIP;
            continue;
        }

        //check for network drops after comparison
        // - convenience: exit sync right here instead of showing tons of errors during file copy
        // - early failure! there's no point in evaluating subsequent warnings
        if (baseFolderDrop< LEFT_SIDE>(baseFolder, folderAccessTimeout, callback) ||
            baseFolderDrop<RIGHT_SIDE>(baseFolder, folderAccessTimeout, callback))
        {
            jobType[folderIndex] = FolderPairJobType::SKIP;
            continue;
        }

        //allow propagation of deletions only from *null-* or *existing* source folder:
        auto sourceFolderMissing = [&](const AbstractPath& baseFolderPath, bool wasAvailable) //we need to evaluate existence status from time of comparison!
        {
            if (!AFS::isNullPath(baseFolderPath))
                //PERMANENT network drop: avoid data loss when source directory is not found AND user chose to ignore errors (else we wouldn't arrive here)
                if (folderPairStat.deleteCount() > 0) //check deletions only... (respect filtered items!)
                    //folderPairStat.conflictCount() == 0 && -> there COULD be conflicts for <Two way> variant if directory existence check fails, but loading sync.ffs_db succeeds
                    //https://sourceforge.net/tracker/?func=detail&atid=1093080&aid=3531351&group_id=234430 -> fixed, but still better not consider conflicts!
                    if (!wasAvailable) //avoid race-condition: we need to evaluate existence status from time of comparison!
                    {
                        callback.reportFatalError(replaceCpy(_("Source folder %x not found."), L"%x", fmtPath(AFS::getDisplayPath(baseFolderPath))));
                        return true;
                    }
            return false;
        };
        if (sourceFolderMissing(baseFolder.getAbstractPath< LEFT_SIDE>(), baseFolder.isAvailable< LEFT_SIDE>()) ||
            sourceFolderMissing(baseFolder.getAbstractPath<RIGHT_SIDE>(), baseFolder.isAvailable<RIGHT_SIDE>()))
        {
            jobType[folderIndex] = FolderPairJobType::SKIP;
            continue;
        }

        if (folderPairCfg.handleDeletion == DeletionPolicy::VERSIONING)
        {
            const AbstractPath versioningFolderPath = createAbstractPath(folderPairCfg.versioningFolderPhrase);

            //check if user-defined directory for deletion was specified
            if (AFS::isNullPath(versioningFolderPath))
            {
                //should never arrive here: already checked in SyncCfgDialog
                callback.reportFatalError(_("Please enter a target folder for versioning."));
                jobType[folderIndex] = FolderPairJobType::SKIP;
                continue;
            }
            //===============================================================================================
            //================ end of checks that may skip folder pairs => begin of warnings ================
            //===============================================================================================

            //prepare: check if versioning path itself will be synchronized (and was not excluded via filter)
            verCheckVersioningPaths.insert(versioningFolderPath);;
            verCheckBaseFolderPaths.emplace_back(baseFolder.getAbstractPath<LEFT_SIDE >(), &baseFolder.getFilter());
            verCheckBaseFolderPaths.emplace_back(baseFolder.getAbstractPath<RIGHT_SIDE>(), &baseFolder.getFilter());
        }

        //prepare: check if folders are used by multiple pairs in read/write access
        readWriteCheckBaseFolders.emplace_back(baseFolder.getAbstractPath<LEFT_SIDE >(), &baseFolder.getFilter(), writeLeft);
        readWriteCheckBaseFolders.emplace_back(baseFolder.getAbstractPath<RIGHT_SIDE>(), &baseFolder.getFilter(), writeRight);

        //check if more than 50% of total number of files/dirs are to be created/overwritten/deleted
        if (!AFS::isNullPath(baseFolder.getAbstractPath< LEFT_SIDE>()) &&
            !AFS::isNullPath(baseFolder.getAbstractPath<RIGHT_SIDE>()))
            if (significantDifferenceDetected(folderPairStat))
                significantDiffPairs.emplace_back(baseFolder.getAbstractPath< LEFT_SIDE>(),
                                                  baseFolder.getAbstractPath<RIGHT_SIDE>());

        //check for sufficient free diskspace
        auto checkSpace = [&](const AbstractPath& baseFolderPath, int64_t minSpaceNeeded)
        {
            if (!AFS::isNullPath(baseFolderPath))
                try
                {
                    const int64_t freeSpace = AFS::getFreeDiskSpace(baseFolderPath); //throw FileError, returns 0 if not available

                    if (0 < freeSpace && //zero means "request not supported" (e.g. see WebDav)
                        freeSpace < minSpaceNeeded)
                        diskSpaceMissing.push_back({ baseFolderPath, { minSpaceNeeded, freeSpace } });
                }
                catch (FileError&) {} //for warning only => no need for tryReportingError()
        };
        const std::pair<int64_t, int64_t> spaceNeeded = MinimumDiskSpaceNeeded::calculate(baseFolder);
        checkSpace(baseFolder.getAbstractPath< LEFT_SIDE>(), spaceNeeded.first);
        checkSpace(baseFolder.getAbstractPath<RIGHT_SIDE>(), spaceNeeded.second);

        //windows: check if recycle bin really exists; if not, Windows will silently delete, which is wrong
        auto checkRecycler = [&](const AbstractPath& baseFolderPath)
        {
            assert(!AFS::isNullPath(baseFolderPath));
            if (!AFS::isNullPath(baseFolderPath))
                if (recyclerSupported.find(baseFolderPath) == recyclerSupported.end()) //perf: avoid duplicate checks!
                {
                    callback.reportStatus(replaceCpy(_("Checking recycle bin availability for folder %x..."), L"%x",
                                                     fmtPath(AFS::getDisplayPath(baseFolderPath))));
                    bool recSupported = false;
                    tryReportingError([&]
                    {
                        recSupported = AFS::supportsRecycleBin(baseFolderPath, [&]{ callback.requestUiRefresh(); /*may throw*/ }); //throw FileError
                    }, callback); //throw X

                    recyclerSupported.emplace(baseFolderPath, recSupported);
                }
        };
        if (folderPairCfg.handleDeletion == DeletionPolicy::RECYCLER)
        {
            if (folderPairStat.expectPhysicalDeletion<LEFT_SIDE>())
                checkRecycler(baseFolder.getAbstractPath<LEFT_SIDE>());

            if (folderPairStat.expectPhysicalDeletion<RIGHT_SIDE>())
                checkRecycler(baseFolder.getAbstractPath<RIGHT_SIDE>());
        }
    }

    //check if unresolved conflicts exist
    if (!unresolvedConflicts.empty())
    {
        std::wstring msg = _("The following items have unresolved conflicts and will not be synchronized:");

        for (const SyncStatistics::ConflictInfo& item : unresolvedConflicts) //show *all* conflicts in warning message
            msg += L"\n\n" + fmtPath(item.relPath) + L": " + item.msg;

        callback.reportWarning(msg, warnings.warnUnresolvedConflicts);
    }

    //check if user accidentally selected wrong directories for sync
    if (!significantDiffPairs.empty())
    {
        std::wstring msg = _("The following folders are significantly different. Please check that the correct folders are selected for synchronization.");

        for (const auto& item : significantDiffPairs)
            msg += L"\n\n" +
                   AFS::getDisplayPath(item.first) + L" <-> " + L"\n" +
                   AFS::getDisplayPath(item.second);

        callback.reportWarning(msg, warnings.warnSignificantDifference);
    }

    //check for sufficient free diskspace
    if (!diskSpaceMissing.empty())
    {
        std::wstring msg = _("Not enough free disk space available in:");

        for (const auto& item : diskSpaceMissing)
            msg += L"\n\n" + AFS::getDisplayPath(item.first) + L"\n" +
                   _("Required:")  + L" " + formatFilesizeShort(item.second.first)  + L"\n" +
                   _("Available:") + L" " + formatFilesizeShort(item.second.second);

        callback.reportWarning(msg, warnings.warnNotEnoughDiskSpace);
    }

    //windows: check if recycle bin really exists; if not, Windows will silently delete, which is wrong
    {
        std::wstring msg;
        for (const auto& item : recyclerSupported)
            if (!item.second)
                msg += L"\n" + AFS::getDisplayPath(item.first);

        if (!msg.empty())
            callback.reportWarning(_("The recycle bin is not supported by the following folders. Deleted or overwritten files will not be able to be restored:") + L"\n" + msg,
                                   warnings.warnRecyclerMissing);
    }

    //check if folders are used by multiple pairs in read/write access
    {
        std::set<AbstractPath> dependentFolders;

        //race condition := multiple accesses of which at least one is a write
        for (auto it = readWriteCheckBaseFolders.begin(); it != readWriteCheckBaseFolders.end(); ++it)
            if (std::get<bool>(*it)) //write access
                for (auto it2 = readWriteCheckBaseFolders.begin(); it2 != readWriteCheckBaseFolders.end(); ++it2)
                    if (!std::get<bool>(*it2) || it < it2) //avoid duplicate comparisons
                        if (Opt<PathDependency> pd = getPathDependency(std::get<AbstractPath>(*it),  *std::get<const HardFilter*>(*it),
                                                                       std::get<AbstractPath>(*it2), *std::get<const HardFilter*>(*it2)))
                        {
                            dependentFolders.insert(pd->basePathParent);
                            dependentFolders.insert(pd->basePathChild);
                        }

        if (!dependentFolders.empty())
        {
            std::wstring msg = _("Some files will be synchronized as part of multiple base folders.") + L"\n" +
                               _("To avoid conflicts, set up exclude filters so that each updated file is considered by only one base folder.") + L"\n";

            for (const AbstractPath& baseFolderPath : dependentFolders)
                msg += L"\n" + AFS::getDisplayPath(baseFolderPath);

            callback.reportWarning(msg, warnings.warnDependentBaseFolders);
        }
    }

    //check if versioning path itself will be synchronized (and was not excluded via filter)
    {
        std::wstring msg;
        for (const AbstractPath& versioningFolderPath : verCheckVersioningPaths)
        {
            std::map<AbstractPath, std::wstring> uniqueMsgs; //=> at most one msg per base folder (*and* per versioningFolderPath)

            for (const auto& item : verCheckBaseFolderPaths) //may contain duplicate paths, but with *different* hard filter!
                if (Opt<PathDependency> pd = getPathDependency(versioningFolderPath, NullFilter(), item.first, *item.second))
                {
                    std::wstring line = L"\n\n" + _("Versioning folder:") + L" \t" + AFS::getDisplayPath(versioningFolderPath) +
                                        L"\n"   + _("Base folder:")       + L" \t" + AFS::getDisplayPath(item.first);
                    if (AFS::equalAbstractPath(pd->basePathParent, item.first) && !pd->relPath.empty())
                        line += L"\n" + _("Exclude:") + L" \t" + utfTo<std::wstring>(FILE_NAME_SEPARATOR + pd->relPath + FILE_NAME_SEPARATOR);

                    uniqueMsgs[item.first] = line;
                }
            for (const auto& item : uniqueMsgs)
                msg += item.second;
        }
        if (!msg.empty())
            callback.reportWarning(_("The versioning folder is contained in a base folder.") + L"\n" +
                                   _("The folder should be excluded from synchronization via filter.") + msg, warnings.warnVersioningFolderPartOfSync);
    }

    //-------------------end of basic checks------------------------------------------

    std::vector<FileError> errorsModTime; //show all warnings as a single message

    try
    {
        const TimeComp timeStamp = getLocalTime(std::chrono::system_clock::to_time_t(syncStartTime));
        if (timeStamp == TimeComp())
            throw std::runtime_error("Failed to determine current time: " + numberTo<std::string>(syncStartTime.time_since_epoch().count()));

        //loop through all directory pairs
        for (auto itBase = begin(folderCmp); itBase != end(folderCmp); ++itBase)
        {
            BaseFolderPair& baseFolder = *itBase;
            const size_t folderIndex = itBase - begin(folderCmp);
            const FolderPairSyncCfg& folderPairCfg  = syncConfig     [folderIndex];
            const SyncStatistics&    folderPairStat = folderPairStats[folderIndex];

            if (jobType[folderIndex] == FolderPairJobType::SKIP) //folder pairs may be skipped after fatal errors were found
                continue;

            //------------------------------------------------------------------------------------------
            callback.reportInfo(_("Synchronizing folder pair:") + L" " + getVariantNameForLog(folderPairCfg.syncVariant) + L"\n" +
                                L"    " + AFS::getDisplayPath(baseFolder.getAbstractPath< LEFT_SIDE>()) + L"\n" +
                                L"    " + AFS::getDisplayPath(baseFolder.getAbstractPath<RIGHT_SIDE>()));
            //------------------------------------------------------------------------------------------

            //checking a second time: (a long time may have passed since folder comparison!)
            if (baseFolderDrop< LEFT_SIDE>(baseFolder, folderAccessTimeout, callback) ||
                baseFolderDrop<RIGHT_SIDE>(baseFolder, folderAccessTimeout, callback))
                continue;

            //create base folders if not yet existing
            if (folderPairStat.createCount() > 0 || folderPairCfg.saveSyncDB) //else: temporary network drop leading to deletions already caught by "sourceFolderMissing" check!
                if (!createBaseFolder< LEFT_SIDE>(baseFolder, folderAccessTimeout, callback) || //+ detect temporary network drop!!
                    !createBaseFolder<RIGHT_SIDE>(baseFolder, folderAccessTimeout, callback))   //
                    continue;

            //------------------------------------------------------------------------------------------
            //execute synchronization recursively

            //update synchronization database in case of errors:
            auto guardDbSave = makeGuard<ScopeGuardRunMode::ON_FAIL>([&]
            {
                try
                {
                    if (folderPairCfg.saveSyncDB)
                        saveLastSynchronousState(baseFolder, //throw FileError
                        [&](const std::wstring& statusMsg) { try { callback.reportStatus(statusMsg); /*throw X*/} catch (...) {}});
                }
                catch (FileError&) {}
            });

            if (jobType[folderIndex] == FolderPairJobType::PROCESS)
            {
                //guarantee removal of invalid entries (where element is empty on both sides)
                ZEN_ON_SCOPE_EXIT(BaseFolderPair::removeEmpty(baseFolder));

                bool copyPermissionsFp = false;
                tryReportingError([&]
                {
                    copyPermissionsFp = copyFilePermissions && //copy permissions only if asked for and supported by *both* sides!
                    !AFS::isNullPath(baseFolder.getAbstractPath< LEFT_SIDE>()) && //scenario: directory selected on one side only
                    !AFS::isNullPath(baseFolder.getAbstractPath<RIGHT_SIDE>()) && //
                    AFS::supportPermissionCopy(baseFolder.getAbstractPath<LEFT_SIDE>(),
                                               baseFolder.getAbstractPath<RIGHT_SIDE>()); //throw FileError
                }, callback); //throw X


                auto getEffectiveDeletionPolicy = [&](const AbstractPath& baseFolderPath) -> DeletionPolicy
                {
                    if (folderPairCfg.handleDeletion == DeletionPolicy::RECYCLER)
                    {
                        auto it = recyclerSupported.find(baseFolderPath);
                        if (it != recyclerSupported.end()) //buffer filled during intro checks (but only if deletions are expected)
                            if (!it->second)
                                return DeletionPolicy::PERMANENT; //Windows' ::SHFileOperation() will do this anyway, but we have a better and faster deletion routine (e.g. on networks)
                    }
                    return folderPairCfg.handleDeletion;
                };

                DeletionHandling delHandlerL(baseFolder.getAbstractPath<LEFT_SIDE>(),
                                             getEffectiveDeletionPolicy(baseFolder.getAbstractPath<LEFT_SIDE>()),
                                             folderPairCfg.versioningFolderPhrase,
                                             folderPairCfg.versioningStyle,
                                             timeStamp);

                DeletionHandling delHandlerR(baseFolder.getAbstractPath<RIGHT_SIDE>(),
                                             getEffectiveDeletionPolicy(baseFolder.getAbstractPath<RIGHT_SIDE>()),
                                             folderPairCfg.versioningFolderPhrase,
                                             folderPairCfg.versioningStyle,
                                             timeStamp);

                //always (try to) clean up, even if synchronization is aborted!
                ZEN_ON_SCOPE_EXIT(
                    //may block heavily, but still do not allow user callback:
                    //-> avoid throwing user cancel exception again, leading to incomplete clean-up!
                    try
                {
                    delHandlerL.tryCleanup(callback, false /*allowCallbackException*/); //throw FileError, (throw X)
                }
                catch (FileError&) {}
                catch (...) { assert(false); } //what is this?
                try
                {
                    delHandlerR.tryCleanup(callback, false /*allowCallbackException*/); //throw FileError, (throw X)
                }
                catch (FileError&) {}
                catch (...) { assert(false); } //what is this?
                );

                auto getParallelOps = [&](const AbstractPath& ap)
                {
                    auto itParOps = deviceParallelOps.find(AFS::getPathComponents(ap).rootPath);
                    return std::max<size_t>(itParOps != deviceParallelOps.end() ? itParOps->second : 1, 1); //sanitize early for correct status display
                };
                const size_t parallelOps = std::max(getParallelOps(baseFolder.getAbstractPath<LEFT_SIDE >()),
                                                    getParallelOps(baseFolder.getAbstractPath<RIGHT_SIDE>()));
                //harmonize with sync_cfg.cpp: parallelOps used for versioning shown == number used for folder pair!

                warn_static("TODO: warn if parallelOps is > than what versioningFolderPhrase can handle (S)FTP!")
                //const AbstractPath versioningFolderPath = createAbstractPath(folderPairCfg.versioningFolderPhrase)
                //getParallelOps(versioningFolderPath)

                FolderPairSyncer::SyncCtx syncCtx =
                {
                    verifyCopiedFiles, copyPermissionsFp, failSafeFileCopy,
                    errorsModTime,
                    delHandlerL, delHandlerR,
                    parallelOps
                };
                FolderPairSyncer::runSync(syncCtx, baseFolder, callback);

                //(try to gracefully) cleanup temporary Recycle bin folders and versioning -> will be done in ~DeletionHandling anyway...
                tryReportingError([&] { delHandlerL.tryCleanup(callback, true /*allowCallbackException*/); /*throw FileError*/}, callback); //throw X
                tryReportingError([&] { delHandlerR.tryCleanup(callback, true                           ); /*throw FileError*/}, callback); //throw X
            }

            //(try to gracefully) write database file
            if (folderPairCfg.saveSyncDB)
            {
                callback.reportStatus(_("Generating database..."));
                callback.forceUiRefresh(); //throw X

                tryReportingError([&]
                {
                    saveLastSynchronousState(baseFolder, //throw FileError
                    [&](const std::wstring& statusMsg) { callback.reportStatus(statusMsg); /*throw X*/});
                }, callback); //throw X

                guardDbSave.dismiss(); //[!] after "graceful" try: user might have cancelled during DB write: ensure DB is still written
            }
        }

        //------------------- show warnings after end of synchronization --------------------------------------

        //TODO: mod time warnings are not shown if user cancelled sync before batch-reporting the warnings: problem?

        //show errors when setting modification time: warning, not an error
        if (!errorsModTime.empty())
        {
            std::wstring msg;
            for (const FileError& e : errorsModTime)
            {
                std::wstring singleMsg = replaceCpy(e.toString(), L"\n\n", L"\n");
                msg += singleMsg + L"\n\n";
            }
            msg.resize(msg.size() - 2);

            callback.reportWarning(msg, warnings.warnModificationTimeError); //throw X
        }
    }
    catch (const std::exception& e)
    {
        callback.reportFatalError(utfTo<std::wstring>(e.what()));
        callback.abortProcessNow(); //throw X
        throw std::logic_error("Contract violation! " + std::string(__FILE__) + ":" + numberTo<std::string>(__LINE__));
    }
}

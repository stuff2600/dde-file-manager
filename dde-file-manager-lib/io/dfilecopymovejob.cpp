/*
 * Copyright (C) 2017 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     zccrs <zccrs@live.com>
 *
 * Maintainer: zccrs <zhangjide@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "dfilecopymovejob.h"
#include "private/dfilecopymovejob_p.h"

#include "dfileservices.h"
#include "dabstractfileinfo.h"
#include "dfiledevice.h"
#include "dfilehandler.h"
#include "ddiriterator.h"
#include "dfilestatisticsjob.h"

#include <QMutex>
#include <QTimer>
#include <QLoggingCategory>

#include <unistd.h>

DFM_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(fileJob, "file.job")

class ElapsedTimer
{
public:
    inline void start() {
        invalidElapsed = 0;
        elapsedOfPause = -1;
        timer.start();
    }

    void togglePause() {
        if (!timer.isValid()) {
            return;
        }

        if (elapsedOfPause < 0) {
            elapsedOfPause = timer.elapsed();
        } else {
            invalidElapsed += (timer.elapsed() - elapsedOfPause);
            elapsedOfPause = -1;
        }
    }

    inline bool isPaused() const {
        return elapsedOfPause > 0;
    }

    inline bool isRunning() const {
        return timer.isValid();
    }

    inline qint64 elapsed() const {
        return timer.elapsed() - invalidElapsed;
    }

    inline qint64 totalElapsed() const {
        return timer.elapsed();
    }

private:
    qint64 elapsedOfPause = -1;
    qint64 invalidElapsed = 0;

    QElapsedTimer timer;
};

DFileCopyMoveJobPrivate::DFileCopyMoveJobPrivate(DFileCopyMoveJob *qq)
    : q_ptr(qq)
    , updateSpeedElapsedTimer(new ElapsedTimer())
{
    // 非debug模式下默认不开启debug输出
#ifdef QT_NO_DEBUG
    QLoggingCategory::setFilterRules("file.job.debug=false");
#endif
}

DFileCopyMoveJobPrivate::~DFileCopyMoveJobPrivate()
{
    delete updateSpeedElapsedTimer;
}

QString DFileCopyMoveJobPrivate::errorToString(DFileCopyMoveJob::Error error)
{
    switch (error) {
    default:
        break;
    }

    return QString();
}

void DFileCopyMoveJobPrivate::setState(DFileCopyMoveJob::State s)
{
    if (state == s)
        return;

    state = s;

    Q_Q(DFileCopyMoveJob);

    Q_EMIT q->stateChanged(s);

    if (updateSpeedTimer->thread()->loopLevel() <= 0) {
        qWarning() << "The thread of update speed timer no event loop" << updateSpeedTimer->thread();
    }

    if (s == DFileCopyMoveJob::RunningState) {
        if (updateSpeedElapsedTimer->isRunning())
            updateSpeedElapsedTimer->togglePause();
        else
            updateSpeedElapsedTimer->start();

        QMetaObject::invokeMethod(updateSpeedTimer, "start", Q_ARG(int, 500));
    } else {
        updateSpeedElapsedTimer->togglePause();

        QMetaObject::invokeMethod(updateSpeedTimer, "stop");
    }

    qCDebug(fileJob()) << "state changed, new state:" << s;
}

void DFileCopyMoveJobPrivate::setError(DFileCopyMoveJob::Error e, const QString &es)
{
    if (error == e)
        return;

    error = e;
    errorString = es.isEmpty() ? errorToString(e) : es;

    Q_Q(DFileCopyMoveJob);

    Q_EMIT q->errorChanged(e);

    qCDebug(fileJob()) << "new error, type=" << e << ", message=" << es;
}

void DFileCopyMoveJobPrivate::unsetError()
{
    error = DFileCopyMoveJob::NoError;
    errorString.clear();
}

DFileCopyMoveJob::Action DFileCopyMoveJobPrivate::handleError(const DAbstractFileInfo *sourceInfo,
                                                              const DAbstractFileInfo *targetInfo)
{
    if (actionOfError[error] != DFileCopyMoveJob::NoAction) {
        lastErrorHandleAction = actionOfError[error];
        unsetError();
        qCDebug(fileJob()) << "from actionOfError list," << "action:" << lastErrorHandleAction
                           << "source url:" << sourceInfo->fileUrl()
                           << "target url:" << (targetInfo ? DUrl() : targetInfo->fileUrl());

        return lastErrorHandleAction;
    }

    if (!handle) {
        switch (error) {
        case DFileCopyMoveJob::PermissionError:
        case DFileCopyMoveJob::UnknowUrlError:
            lastErrorHandleAction = DFileCopyMoveJob::SkipAction;
            unsetError();
            break;
        case DFileCopyMoveJob::FileExistsError:
        case DFileCopyMoveJob::DirectoryExistsError:
            lastErrorHandleAction = DFileCopyMoveJob::CoexistAction;
            unsetError();
            break;
        default:
            lastErrorHandleAction = DFileCopyMoveJob::CancelAction;
            setError(DFileCopyMoveJob::CancelError);
            break;
        }

        qCDebug(fileJob()) << "no handle," << "default action:" << lastErrorHandleAction
                           << "source url:" << sourceInfo->fileUrl()
                           << "target url:" << (targetInfo ? DUrl() : targetInfo->fileUrl());

        return lastErrorHandleAction;
    }

    setState(DFileCopyMoveJob::SleepState);

    do {
        if (threadAtStart && threadAtStart->loopLevel() > 0) {
            lastErrorHandleAction = DThreadUtil::runInThread(threadAtStart, handle, &DFileCopyMoveJob::Handle::handleError,
                                              q_ptr, error, sourceInfo, targetInfo);
        } else {
            lastErrorHandleAction = DThreadUtil::runInMainThread(handle, &DFileCopyMoveJob::Handle::handleError,
                                                  q_ptr, error, sourceInfo, targetInfo);
        }

        if (!stateCheck()) {
            lastErrorHandleAction = DFileCopyMoveJob::CancelAction;
            break;
        }
    } while (lastErrorHandleAction == DFileCopyMoveJob::NoAction);

    if (state == DFileCopyMoveJob::SleepState)
        setState(DFileCopyMoveJob::RunningState);

    if (lastErrorHandleAction == DFileCopyMoveJob::NoAction) {
        lastErrorHandleAction = DFileCopyMoveJob::CancelAction;
    }

    unsetError();

    if (lastErrorHandleAction == DFileCopyMoveJob::CancelAction) {
        setError(DFileCopyMoveJob::CancelError);
    }

    qCDebug(fileJob()) << "from user," << "action:" << lastErrorHandleAction
                       << "source url:" << sourceInfo->fileUrl()
                       << "target url:" << (targetInfo ? targetInfo->fileUrl() : DUrl());

    return lastErrorHandleAction;
}

bool DFileCopyMoveJobPrivate::jobWait()
{
    QMutex lock;

    lock.lock();
    waitCondition.wait(&lock);
    lock.unlock();

    return state == DFileCopyMoveJob::RunningState;
}

bool DFileCopyMoveJobPrivate::stateCheck()
{
    if (state == DFileCopyMoveJob::RunningState) {
        if (needUpdateProgress) {
            needUpdateProgress = false;

            updateProgress();
            updateSpeed();
        }

        return true;
    }

    if (state == DFileCopyMoveJob::PausedState) {
        qCDebug(fileJob()) << "Will be suspended";

        if (!jobWait()) {
            setError(DFileCopyMoveJob::CancelError);
            qCDebug(fileJob()) << "Will be abort";

            return false;
        }
    } else if (state == DFileCopyMoveJob::StoppedState) {
        setError(DFileCopyMoveJob::CancelError);
        qCDebug(fileJob()) << "Will be abort";

        return false;
    }

    return true;
}

QString DFileCopyMoveJobPrivate::getNewFileName(const DAbstractFileInfo *sourceFileInfo, const DAbstractFileInfo *targetDirectory)
{
    const QString &copy_text = QCoreApplication::translate(QT_STRINGIFY(DFileCopyMoveJob), "copy",
                                                           "Extra name added to new file name when used for file name.");

    DAbstractFileInfoPointer target_file_info;
    QString file_base_name = sourceFileInfo->baseName();
    QString suffix = sourceFileInfo->suffix();
    int number = 0;

    QString new_file_name;

    do {
        new_file_name = number > 0 ? QString("%1(%2 %3)").arg(file_base_name, copy_text).arg(number) : QString("%1(%2)").arg(file_base_name, copy_text);

        if (!suffix.isEmpty()) {
            new_file_name.append('.').append(suffix);
        }

        ++number;
        target_file_info = DFileService::instance()->createFileInfo(nullptr, targetDirectory->getUrlByChildFileName(new_file_name));
    } while(target_file_info->exists());

    return new_file_name;
}

bool DFileCopyMoveJobPrivate::process(const DUrl &from, const DAbstractFileInfo *target_info)
{
    Q_Q(DFileCopyMoveJob);

    // reset error and action
    unsetError();
    lastErrorHandleAction = DFileCopyMoveJob::NoAction;

    DAbstractFileInfoPointer source_info = DFileService::instance()->createFileInfo(nullptr, from);

    if (!source_info) {
        setError(DFileCopyMoveJob::UnknowUrlError, "Failed create file info");

        return handleError(source_info.constData(), nullptr) == DFileCopyMoveJob::SkipAction;
    }

    if (!source_info->exists()) {
        setError(DFileCopyMoveJob::NonexistenceError, QString("\"%1\" non-exists or not permission").arg(from.toString()));

        return handleError(source_info.constData(), nullptr) == DFileCopyMoveJob::SkipAction;
    }

    switch (source_info->fileType()) {
    case DAbstractFileInfo::CharDevice:
    case DAbstractFileInfo::BlockDevice:
    case DAbstractFileInfo::FIFOFile:
    case DAbstractFileInfo::SocketFile: {
        setError(DFileCopyMoveJob::SpecialFileError, "Can't copy/move/rm special file");

        return handleError(source_info.constData(), nullptr) == DFileCopyMoveJob::SkipAction;
    }
    default:
        break;
    }

    DFileHandler *handler = DFileService::instance()->createFileHandler(nullptr, from);

    if (!handler) {
        setError(DFileCopyMoveJob::UnknowUrlError, "Failed create file handler");

        return handleError(source_info.constData(), nullptr) == DFileCopyMoveJob::SkipAction;
    }

    // only remove
    if (!target_info) {
        bool ok = false;

        qint64 size = source_info->size();

        if (source_info->isFile() || source_info->isSymLink()) {
            ok = removeFile(handler, source_info.constData());

            if (ok) {
                joinToCompletedFileList(from, DUrl(), size);
            }
        } else {
            ok = mergeDirectory(handler, source_info.constData(), nullptr);

            if (ok) {
                joinToCompletedDirectoryList(from, DUrl(), size);
            }
        }

        return ok;
    }

    QString file_name = handle ? handle->getNewFileName(q_ptr, source_info.constData()) : source_info->fileName();
create_new_file_info:
    const DAbstractFileInfoPointer &new_file_info = DFileService::instance()->createFileInfo(nullptr, target_info->getUrlByChildFileName(file_name));

    if (new_file_info->exists()) {
        bool source_is_file = source_info->isFile() || source_info->isSymLink();
        bool target_is_file = new_file_info->isFile() || new_file_info->isSymLink();

        if (target_is_file)
            setError(DFileCopyMoveJob::FileExistsError);
        else
            setError(DFileCopyMoveJob::DirectoryExistsError);

        switch (handleError(source_info.constData(), new_file_info.constData())) {
        case DFileCopyMoveJob::ReplaceAction:
            if (source_is_file && source_is_file == target_is_file) {
                break;
            } else {
                return false;
            }
        case DFileCopyMoveJob::MergeAction:
            if (!source_is_file && source_is_file == target_is_file)
                break;
            else
                return false;
        case DFileCopyMoveJob::SkipAction:
            return true;
        case DFileCopyMoveJob::CoexistAction:
            file_name = handle ? handle->getNonExistsFileName(q_ptr, source_info.constData(), target_info)
                               : getNewFileName(source_info.constData(), target_info);
            goto create_new_file_info;
        default:
            return false;
        }
    }

    if (source_info->isSymLink()) {
        bool ok = false;
        qint64 size = source_info->size();

        if (mode == DFileCopyMoveJob::CopyMode) {
            if (fileHints.testFlag(DFileCopyMoveJob::FollowSymlink)) {
                do {
                    const DAbstractFileInfoPointer &symlink_target = DFileService::instance()->createFileInfo(nullptr, source_info->symLinkTarget());

                    if (!symlink_target->exists())
                        break;

                    source_info = symlink_target;
                } while (source_info->isSymLink());

                if (source_info->exists()) {
                    goto process_file;
                }
            }

            ok = linkFile(handler, new_file_info.constData(), source_info->symlinkTargetPath());
        } else {
            ok = renameFile(handler, source_info.constData(), new_file_info.constData());
        }

        if (ok) {
            joinToCompletedFileList(from, new_file_info->fileUrl(), size);
        }

        return ok;
    }

process_file:
    if (source_info->isFile()) {
        bool ok = false;
        qint64 size = source_info->size();

        if (mode == DFileCopyMoveJob::CopyMode) {
            if (new_file_info->isSymLink() || fileHints.testFlag(DFileCopyMoveJob::RemoveDestination)) {
                if (!removeFile(handler, new_file_info.constData())) {
                    return false;
                }
            }

            ok = copyFile(source_info.constData(), new_file_info.constData());
        } else {
            ok = renameFile(handler, source_info.constData(), new_file_info.constData());
        }

        if (ok) {
            joinToCompletedFileList(from, new_file_info->fileUrl(), size);
        }

        return ok;
    } else if (source_info->isDir()) {
        bool ok = true;
        qint64 size = source_info->size();

        if (mode == DFileCopyMoveJob::CopyMode) {
            ok = mergeDirectory(handler, source_info.constData(), new_file_info.constData());
        } else if (!doRenameFile(handler, source_info.constData(), new_file_info.constData())) { // 尝试直接rename操作
            qCDebug(fileJob(), "Failed on rename, Well be copy and delete the directory");
            ok = mergeDirectory(handler, source_info.constData(), new_file_info.constData());
        }

        if (ok) {
            joinToCompletedDirectoryList(from, new_file_info->fileUrl(), size);
        }

        return ok;
    }

    return false;
}

bool DFileCopyMoveJobPrivate::mergeDirectory(DFileHandler *handler, const DAbstractFileInfo *fromInfo, const DAbstractFileInfo *toInfo)
{
    if (toInfo && !toInfo->exists()) {
        bool ok = handler->mkdir(toInfo->fileUrl());

        if (!ok) {
            setError(DFileCopyMoveJob::MkdirError, handler->errorString());

            return handleError(fromInfo, toInfo) == DFileCopyMoveJob::SkipAction;
        }
    }

    const DDirIteratorPointer &iterator = DFileService::instance()->createDirIterator(nullptr, fromInfo->fileUrl(),
                                                                                      QStringList(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);

    if (!iterator) {
        setError(DFileCopyMoveJob::UnknowUrlError, "Failed on create dir iterator");

        return false;
    }

    while (iterator->hasNext()) {
        if (!stateCheck())
            return false;

        const DUrl &url = iterator->next();

        if (!process(url, toInfo))
            return false;
    }

    if (mode == DFileCopyMoveJob::CopyMode)
        return true;

    // 剪切模式时, 完成操作后删除原目录
    return removeFile(handler, fromInfo);
}

bool DFileCopyMoveJobPrivate::doCopyFile(const DAbstractFileInfo *fromInfo, const DAbstractFileInfo *toInfo, int blockSize)
{
    QScopedPointer<DFileDevice> fromDevice(DFileService::instance()->createFileDevice(nullptr, fromInfo->fileUrl()));

    if (!fromDevice) {
        setError(DFileCopyMoveJob::UnknowUrlError, "Failed on create file device");

        return false;
    }

    QScopedPointer<DFileDevice> toDevice(DFileService::instance()->createFileDevice(nullptr, toInfo->fileUrl()));

    if (!toDevice) {
        setError(DFileCopyMoveJob::UnknowUrlError, "Failed on create file device");

        return false;
    }

    {
        DFileCopyMoveJob::Action action = DFileCopyMoveJob::NoAction;

        do {
            if (fromDevice->open(QIODevice::ReadOnly)) {
                action = DFileCopyMoveJob::NoAction;
            } else {
                setError(DFileCopyMoveJob::OpenError, fromDevice->errorString());
                action = handleError(fromInfo, toInfo);
            }
        } while (action == DFileCopyMoveJob::RetryAction);

        if (action == DFileCopyMoveJob::SkipAction) {
            return true;
        } else if (action != DFileCopyMoveJob::NoAction) {
            return false;
        }

        do {
            if (toDevice->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                action = DFileCopyMoveJob::NoAction;
            } else {
                setError(DFileCopyMoveJob::OpenError, fromDevice->errorString());
                action = handleError(fromInfo, toInfo);
            }
        } while (action == DFileCopyMoveJob::RetryAction);

        if (action == DFileCopyMoveJob::SkipAction) {
            return true;
        } else if (action != DFileCopyMoveJob::NoAction) {
            return false;
        }

        if (fileHints.testFlag(DFileCopyMoveJob::ResizeDestinationFile)) {
            do {
                if (toDevice->resize(fromDevice->size())) {
                    action = DFileCopyMoveJob::NoAction;
                } else {
                    setError(DFileCopyMoveJob::ResizeError, toDevice->errorString());
                    action = handleError(toInfo, nullptr);
                }
            } while (action == DFileCopyMoveJob::RetryAction);

            if (action == DFileCopyMoveJob::SkipAction) {
                return true;
            } else if (action != DFileCopyMoveJob::NoAction) {
                return false;
            }
        }
    }

    currentJobDataSizeInfo.first = fromDevice->size();
    currentJobFileHandle = toDevice->handle();

    int writtenDataSize = 0;

    Q_FOREVER {
        qint64 current_pos = fromDevice->pos();
read_data:
        if (!stateCheck()) {
            return false;
        }

        char data[blockSize + 1];
        qint64 size_read = fromDevice->read(data, blockSize);

        if (size_read <= 0) {
            if (fromDevice->atEnd()) {
                break;
            }

            setError(DFileCopyMoveJob::ReadError, fromDevice->errorString());

            switch (handleError(fromInfo, toInfo)) {
            case DFileCopyMoveJob::RetryAction: {
                if (!fromDevice->seek(current_pos)) {
                    setError(DFileCopyMoveJob::UnknowError, fromDevice->errorString());

                    return false;
                }

                goto read_data;
            }
            case DFileCopyMoveJob::SkipAction:
                return true;
            default:
                return false;
            }
        }

        current_pos = toDevice->pos();
write_data:
        if (!stateCheck()) {
            return false;
        }

        qint64 size_write = toDevice->write(data, size_read);

        if (size_write != size_read) {
            setError(DFileCopyMoveJob::ReadError, toDevice->errorString());

            switch (handleError(fromInfo, toInfo)) {
            case DFileCopyMoveJob::RetryAction: {
                if (!toDevice->seek(current_pos)) {
                    setError(DFileCopyMoveJob::UnknowError, toDevice->errorString());

                    return false;
                }

                goto write_data;
            }
            case DFileCopyMoveJob::SkipAction:
                return true;
            default:
                return false;
            }
        }

        currentJobDataSizeInfo.second += size_write;
        completedDataSize += size_write;
        writtenDataSize += size_write;

        if (writtenDataSize > 20000000) {
            writtenDataSize = 0;
            toDevice->syncToDisk();
        }
    }

    fromDevice->close();
    toDevice->close();

    return true;
}

bool DFileCopyMoveJobPrivate::doRemoveFile(DFileHandler *handler, const DAbstractFileInfo *fileInfo)
{
    if (!fileInfo->exists())
        return true;

    DFileCopyMoveJob::Action action = DFileCopyMoveJob::NoAction;
    bool is_file = fileInfo->isFile() || fileInfo->isSymLink();

    do {
        if (is_file ? handler->remove(fileInfo->fileUrl()) : handler->rmdir(fileInfo->fileUrl()))
            return true;

        setError(DFileCopyMoveJob::RemoveError, handler->errorString());
        action = handleError(fileInfo, nullptr);
    } while (action == DFileCopyMoveJob::RetryAction);

    return action == DFileCopyMoveJob::SkipAction;
}

bool DFileCopyMoveJobPrivate::doRenameFile(DFileHandler *handler, const DAbstractFileInfo *oldInfo, const DAbstractFileInfo *newInfo)
{
    // 先尝试直接rename
    if (handler->rename(oldInfo->fileUrl(), newInfo->fileUrl(), true)) {
        return true;
    }

    qCDebug(fileJob(), "Failed on rename, Well be copy and delete the file");

    // 先复制再删除
    if (!doCopyFile(oldInfo, newInfo)) {
        return false;
    }

    if (!handler->exists(newInfo->fileUrl())) {
        // 说明复制文件过程被跳过
        return true;
    }

    if (!doRemoveFile(handler, oldInfo)) {
        return false;
    }

    return true;
}

bool DFileCopyMoveJobPrivate::doLinkFile(DFileHandler *handler, const DAbstractFileInfo *fileInfo, const QString &linkPath)
{
    if (fileInfo->exists()) {
        if (!removeFile(handler, fileInfo))
            return false;
    }

    DFileCopyMoveJob::Action action = DFileCopyMoveJob::NoAction;

    do {
        if (handler->link(linkPath, fileInfo->fileUrl())) {
            return true;
        }

        setError(DFileCopyMoveJob::SymlinkError, handler->errorString());
        action = handleError(fileInfo, nullptr);
    } while (action == DFileCopyMoveJob::RetryAction);

    return action == DFileCopyMoveJob::SkipAction;
}

bool DFileCopyMoveJobPrivate::copyFile(const DAbstractFileInfo *fromInfo, const DAbstractFileInfo *toInfo, int blockSize)
{
    qint64 elapsed = 0;

    if (fileJob().isDebugEnabled()) {
        elapsed = updateSpeedElapsedTimer->elapsed();
    }

    beginJob(JobInfo::Copy, fromInfo->fileUrl(), toInfo->fileUrl());
    bool ok = doCopyFile(fromInfo, toInfo, blockSize);
    endJob();

    if (fileJob().isDebugEnabled()) {
        qCDebug(fileJob(), "Time spent of copy the file: %lld", updateSpeedElapsedTimer->elapsed() - elapsed);
    }

    return ok;
}

bool DFileCopyMoveJobPrivate::removeFile(DFileHandler *handler, const DAbstractFileInfo *fileInfo)
{
    beginJob(JobInfo::Remove, fileInfo->fileUrl(), DUrl());
    bool ok = doRemoveFile(handler, fileInfo);
    endJob();

    return ok;
}

bool DFileCopyMoveJobPrivate::renameFile(DFileHandler *handler, const DAbstractFileInfo *oldInfo, const DAbstractFileInfo *newInfo)
{
    beginJob(JobInfo::Move, oldInfo->fileUrl(), newInfo->fileUrl());
    bool ok = doRenameFile(handler, oldInfo, newInfo);
    endJob();

    return ok;
}

bool DFileCopyMoveJobPrivate::linkFile(DFileHandler *handler, const DAbstractFileInfo *fileInfo, const QString &linkPath)
{
    beginJob(JobInfo::Link, DUrl(linkPath), fileInfo->fileUrl());
    bool ok = doLinkFile(handler, fileInfo, linkPath);
    endJob();

    return ok;
}

void DFileCopyMoveJobPrivate::beginJob(JobInfo::Type type, const DUrl &from, const DUrl &target)
{
    qCDebug(fileJob(), "job begin, Type: %d, from: %s, to: %s", type, qPrintable(from.toString()), qPrintable(target.toString()));
    jobStack.push({type, QPair<DUrl, DUrl>(from, target)});
    currentJobDataSizeInfo = qMakePair(-1, 0);
    currentJobFileHandle = -1;

    Q_EMIT q_ptr->currentJobChanged(from, target);
}

void DFileCopyMoveJobPrivate::endJob()
{
    jobStack.pop();
    currentJobFileHandle = -1;

    qCDebug(fileJob()) << "job end, error:" << error << "last error handle action:" << lastErrorHandleAction;
}

void DFileCopyMoveJobPrivate::joinToCompletedFileList(const DUrl &from, const DUrl &target, qint64 dataSize)
{
    qCDebug(fileJob(), "file. from: %s, target: %s, data size: %lld", qPrintable(from.toString()), qPrintable(from.toString()), dataSize);

    if (currentJobDataSizeInfo.first < 0)
        completedDataSize += dataSize;

    ++completedFilesCount;

    Q_EMIT q_ptr->completedFilesCountChanged(completedFilesCount);

    if (lastErrorHandleAction == DFileCopyMoveJob::SkipAction) {
        return;
    }

    completedFileList << qMakePair(from, target);
}

void DFileCopyMoveJobPrivate::joinToCompletedDirectoryList(const DUrl &from, const DUrl &target, qint64 dataSize)
{
    qCDebug(fileJob(), "directory. from: %s, target: %s, data size: %lld", qPrintable(from.toString()), qPrintable(from.toString()), dataSize);

    completedDataSize += dataSize;
    ++completedFilesCount;

    Q_EMIT q_ptr->completedFilesCountChanged(completedFilesCount);

    if (lastErrorHandleAction == DFileCopyMoveJob::SkipAction) {
        return;
    }

    completedDirectoryList << qMakePair(from, target);
}

void DFileCopyMoveJobPrivate::updateProgress()
{
    if (fileStatistics->isFinished()) {
        const qint64 data_size = completedDataSize;
        Q_EMIT q_ptr->progressChanged(qreal(data_size) / fileStatistics->totalSize(), data_size);

        qCDebug(fileJob(), "completed data size: %lld, total data size: %lld", data_size, fileStatistics->totalSize());
    }

    if (currentJobDataSizeInfo.first > 0) {
        Q_EMIT q_ptr->currentFileProgressChanged(qreal(currentJobDataSizeInfo.second) / currentJobDataSizeInfo.first, currentJobDataSizeInfo.second);
    }
}

void DFileCopyMoveJobPrivate::updateSpeed()
{
    const qint64 time = updateSpeedElapsedTimer->elapsed();
    const qint64 total_size = completedDataSize;

    qint64 speed = total_size / time * 1000;

    Q_EMIT q_ptr->speedUpdated(speed);
}

void DFileCopyMoveJobPrivate::_q_updateProgress()
{
    ++timeOutCount;
    needUpdateProgress = true;

    updateSpeed();
}

DFileCopyMoveJob::DFileCopyMoveJob(QObject *parent)
    : DFileCopyMoveJob(*new DFileCopyMoveJobPrivate(this), parent)
{

}

DFileCopyMoveJob::~DFileCopyMoveJob()
{
    stop();
    // ###(zccrs): wait() ?
}

DFileCopyMoveJob::Handle *DFileCopyMoveJob::errorHandle() const
{
    Q_D(const DFileCopyMoveJob);

    return d->handle;
}

void DFileCopyMoveJob::setErrorHandle(DFileCopyMoveJob::Handle *handle)
{
    Q_D(DFileCopyMoveJob);

    d->handle = handle;
}

void DFileCopyMoveJob::setActionOfErrorType(DFileCopyMoveJob::Error error, DFileCopyMoveJob::Action action)
{
    Q_D(DFileCopyMoveJob);
    Q_ASSERT(d->state == SleepState);

    d->actionOfError[error] = action;
}

DFileCopyMoveJob::Mode DFileCopyMoveJob::mode() const
{
    Q_D(const DFileCopyMoveJob);

    return d->mode;
}

DFileCopyMoveJob::State DFileCopyMoveJob::state() const
{
    Q_D(const DFileCopyMoveJob);

    return static_cast<State>(d->state.load());
}

DFileCopyMoveJob::Error DFileCopyMoveJob::error() const
{
    Q_D(const DFileCopyMoveJob);

    return d->error;
}

DFileCopyMoveJob::FileHints DFileCopyMoveJob::fileHints() const
{
    Q_D(const DFileCopyMoveJob);

    return d->fileHints;
}

QString DFileCopyMoveJob::errorString() const
{
    Q_D(const DFileCopyMoveJob);

    return d->errorString;
}

DUrlList DFileCopyMoveJob::sourceUrlList() const
{
    Q_D(const DFileCopyMoveJob);

    return d->sourceUrlList;
}

DUrlList DFileCopyMoveJob::targetUrlList() const
{
    Q_D(const DFileCopyMoveJob);

    return d->targetUrlList;
}

DUrl DFileCopyMoveJob::targetUrl() const
{
    Q_D(const DFileCopyMoveJob);

    return d->targetUrl;
}

qint64 DFileCopyMoveJob::totalDataSize() const
{
    Q_D(const DFileCopyMoveJob);

    if (d->fileStatistics->isRunning()) {
        return -1;
    }

    return d->fileStatistics->totalSize();
}

int DFileCopyMoveJob::totalFilesCount() const
{
    Q_D(const DFileCopyMoveJob);

    if (d->fileStatistics->isRunning()) {
        return -1;
    }

    return d->fileStatistics->filesCount() + d->fileStatistics->directorysCount();
}

QList<QPair<DUrl, DUrl>> DFileCopyMoveJob::completedFiles() const
{
    Q_D(const DFileCopyMoveJob);
    Q_ASSERT(isRunning());

    return d->completedFileList;
}

QList<QPair<DUrl, DUrl>> DFileCopyMoveJob::completedDirectorys() const
{
    Q_D(const DFileCopyMoveJob);
    Q_ASSERT(isRunning());

    return d->completedDirectoryList;
}

void DFileCopyMoveJob::start(const DUrlList &sourceUrls, const DUrl &targetUrl)
{
    Q_ASSERT(!isRunning());
    Q_D(DFileCopyMoveJob);

    d->sourceUrlList = sourceUrls;
    d->targetUrl = targetUrl;
    d->threadAtStart = QThread::currentThread();

    if (d->fileStatistics->isRunning()) {
        d->fileStatistics->stop();
        d->fileStatistics->wait();
    }

    d->fileStatistics->start(sourceUrls);

    QThread::start();
}

void DFileCopyMoveJob::stop()
{
    Q_D(DFileCopyMoveJob);

    if (d->state == StoppedState)
        return;

    d->fileStatistics->stop();

    d->setState(StoppedState);
    d->waitCondition.wakeAll();
}

void DFileCopyMoveJob::togglePause()
{
    Q_D(DFileCopyMoveJob);

    if (d->state == StoppedState)
        return;

    d->fileStatistics->togglePause();

    if (d->state == PausedState) {
        d->setState(RunningState);
        d->waitCondition.wakeAll();
    } else {
        d->setState(PausedState);
    }
}

void DFileCopyMoveJob::setMode(DFileCopyMoveJob::Mode mode)
{
    Q_ASSERT(!isRunning());
    Q_D(DFileCopyMoveJob);

    d->mode = mode;
}

void DFileCopyMoveJob::setFileHints(FileHints fileHints)
{
    Q_D(DFileCopyMoveJob);
    Q_ASSERT(d->state != RunningState);

    d->fileHints = fileHints;
    d->fileStatistics->setFileHints(fileHints.testFlag(FollowSymlink) ? DFileStatisticsJob::FollowSymlink : DFileStatisticsJob::FileHints());
}

DFileCopyMoveJob::DFileCopyMoveJob(DFileCopyMoveJobPrivate &dd, QObject *parent)
    : QThread(parent)
    , d_d_ptr(&dd)
{
    dd.fileStatistics = new DFileStatisticsJob(this);
    dd.updateSpeedTimer = new QTimer(this);

    connect(dd.fileStatistics, &DFileStatisticsJob::finished, this, &DFileCopyMoveJob::fileStatisticsFinished, Qt::DirectConnection);
    connect(dd.updateSpeedTimer, SIGNAL(timeout()), this, SLOT(_q_updateProgress()), Qt::DirectConnection);
}

void DFileCopyMoveJob::run()
{
    Q_D(DFileCopyMoveJob);

//    if (d->mode == CopyMode) {
//        if (d->fileHints.testFlag(CreateParents)) {

//        }
//    }

    qCDebug(fileJob()) << "start job, mode:" << d->mode << "file url list:" << d->sourceUrlList << ", target url:" << d->targetUrl;

    d->unsetError();
    d->setState(RunningState);
    d->completedDirectoryList.clear();
    d->completedFileList.clear();
    d->targetUrlList.clear();
    d->completedDataSize = 0;
    d->completedFilesCount = 0;

    DAbstractFileInfoPointer target_info;

    if (d->targetUrl.isValid()) {
        target_info = DFileService::instance()->createFileInfo(nullptr, d->targetUrl);

        if (!target_info) {
            d->setError(UnknowUrlError);
            goto end;
        }

        if (!target_info->exists()) {
            d->setError(NonexistenceError, "The target directory non-exists or not permission");
            goto end;
        }

        if (!target_info->isDir()) {
            d->setError(UnknowError, "The target url is not directory");
            goto end;
        }
    } else if (d->mode == CopyMode) {
        d->setError(UnknowError, "Invalid target url");
        goto end;
    } else {
        // remove mode
        qCDebug(fileJob(), "remove mode");
    }

    for (DUrl &source : d->sourceUrlList) {
        if (!d->stateCheck()) {
            goto end;
        }

        if (!d->process(source, target_info.constData())) {
            goto end;
        }

        DUrl target_url;

        if (!d->completedFileList.isEmpty()) {
            if (d->completedFileList.last().first == source) {
                target_url = d->completedFileList.last().second;
            }
        }

        if (!d->completedDirectoryList.isEmpty()) {
            if (d->completedDirectoryList.last().first == source) {
                target_url = d->completedDirectoryList.last().second;
            }
        }

        d->targetUrlList << target_url;

        Q_EMIT finished(source, target_url);
    }

    d->setError(NoError);

end:
    d->setState(StoppedState);

    if (d->error == NoError)
        Q_EMIT progressChanged(1, d->completedDataSize);

    qCDebug(fileJob()) << "job finished, error:" << error() << ", message:" << errorString();
}

QString DFileCopyMoveJob::Handle::getNewFileName(DFileCopyMoveJob *job, const DAbstractFileInfo *sourceInfo)
{
    Q_UNUSED(job)

    return sourceInfo->fileName();
}

QString DFileCopyMoveJob::Handle::getNonExistsFileName(DFileCopyMoveJob *job, const DAbstractFileInfo *sourceInfo, const DAbstractFileInfo *targetDirectory)
{
    Q_UNUSED(job)

    return DFileCopyMoveJobPrivate::getNewFileName(sourceInfo, targetDirectory);
}

DFM_END_NAMESPACE

#include "moc_dfilecopymovejob.cpp"
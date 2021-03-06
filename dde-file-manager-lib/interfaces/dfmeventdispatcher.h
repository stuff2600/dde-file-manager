/**
 * Copyright (C) 2017 Deepin Technology Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 **/
#ifndef DFMEVENTDISPATCHER_H
#define DFMEVENTDISPATCHER_H

#include "dfmglobal.h"
#include "dfmevent.h"

#include <QFuture>
#include <QEventLoop>

class DFMEvent;
DFM_BEGIN_NAMESPACE

class DFMEventFuture
{
public:
    explicit DFMEventFuture(const QFuture<QVariant> &future);
    DFMEventFuture(const DFMEventFuture &other);

    void cancel();
    bool isCanceled() const;

    bool isStarted() const;
    bool isFinished() const;
    bool isRunning() const;

    void waitForFinished();
    int waitForFinishedWithEventLoop(QEventLoop::ProcessEventsFlags flags = QEventLoop::EventLoopExec) const;

    QVariant result() const;
    template<typename T>
    inline T result() const
    { return qvariant_cast<T>(result());}

    void operator =(const DFMEventFuture &other);

private:
    QFuture<QVariant> m_future;
};

class DFMAbstractEventHandler;
class DFMEventDispatcherPrivate;
class DFMEventDispatcher : public QObject
{
    Q_OBJECT

    Q_PROPERTY(State state READ state NOTIFY stateChanged)

public:
    enum State {
        Normal,
        Busy
    };

    Q_ENUM(State)

    static DFMEventDispatcher *instance();
    ~DFMEventDispatcher();

    QVariant processEvent(const QSharedPointer<DFMEvent> &event, DFMAbstractEventHandler *target = nullptr);
    template<class T, typename... Args>
    QVariant processEvent(Args&&... args)
    {
        return processEvent(dMakeEventPointer<T>(std::forward<Args>(args)...));
    }
    DFMEventFuture processEventAsync(const QSharedPointer<DFMEvent> &event, DFMAbstractEventHandler *target = nullptr);
    template<class T, typename... Args>
    DFMEventFuture processEventAsync(Args&&... args)
    {
        return processEventAsync(dMakeEventPointer<T>(std::forward<Args>(args)...));
    }
    QVariant processEventWithEventLoop(const QSharedPointer<DFMEvent> &event, DFMAbstractEventHandler *target = nullptr);
    template<class T, typename... Args>
    QVariant processEventWithEventLoop(Args&&... args)
    {
        return processEventWithEventLoop(dMakeEventPointer<T>(std::forward<Args>(args)...));
    }

    void installEventFilter(DFMAbstractEventHandler *handler);
    void removeEventFilter(DFMAbstractEventHandler *handler);

    State state() const;

signals:
    void stateChanged(State state);

protected:
    DFMEventDispatcher();

private:
    QScopedPointer<DFMEventDispatcherPrivate> d_ptr;

    void installEventHandler(DFMAbstractEventHandler *handler);
    void removeEventHandler(DFMAbstractEventHandler *handler);

    friend class DFMAbstractEventHandler;

    Q_DECLARE_PRIVATE(DFMEventDispatcher)
};

DFM_END_NAMESPACE

#endif // DFMEVENTDISPATCHER_H

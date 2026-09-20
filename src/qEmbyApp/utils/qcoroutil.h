#ifndef QCOROUTIL_H
#define QCOROUTIL_H

#include <QObject>
#include <qcorotask.h>

// Fire-and-forget helper for QCoro coroutines.
//
// Keeps the coroutine alive (via QCoro::connect) until it finishes, including
// across suspension points. Dropping the temporary Task directly would cancel
// the coroutine at its first co_await. `context`'s destruction cancels the
// coroutine as well, so pass the object whose lifetime the task must not
// outlive (usually `this`, or a child object with a tighter lifetime).
inline void launchTask(QCoro::Task<void>&& task, QObject* context)
{
    QCoro::connect(std::move(task), context, []() {});
}

#endif // QCOROUTIL_H

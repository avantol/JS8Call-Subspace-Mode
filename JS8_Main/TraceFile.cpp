/**
 * @file TraceFile.cpp
 * @brief Implementation of TraceFile class for logging
 */

#include "TraceFile.h"
#include "JS8_Include/pimpl_impl.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QMessageLogContext>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QTextStream>

#include <stdexcept>

namespace {
QMutex lock;
}

class TraceFile::impl {
  public:
    impl(QString const &trace_file_path);
    ~impl();

    // no copying
    impl(impl const &) = delete;
    impl &operator=(impl const &) = delete;

  private:
    // write Qt messages to the diagnostic log file
    static void message_handler(QtMsgType type,
                                QMessageLogContext const &context,
                                QString const &msg);

    QFile file_;
    QTextStream stream_;
    QTextStream *original_stream_;
    QtMessageHandler original_handler_;
    static QTextStream *current_stream_;
};

QTextStream *TraceFile::impl::current_stream_;

// delegate to implementation class
TraceFile::TraceFile(QString const &trace_file_path) : m_{trace_file_path} {}

TraceFile::~TraceFile() {}

TraceFile::impl::impl(QString const &trace_file_path)
    : file_{trace_file_path}, original_stream_{current_stream_},
      original_handler_{nullptr} {
    // Open with QFile::Unbuffered so writes go straight to the OS without
    // sitting in QFile's internal buffer. Combined with explicit flush()
    // after each message (in the handler) this means a hard crash leaves a
    // log that's accurate up to the last instruction we executed, rather
    // than truncated at whatever was last flushed.
    if (file_.open(QFile::WriteOnly | QFile::Append | QFile::Text |
                   QFile::Unbuffered)) {
        stream_.setDevice(&file_);
        current_stream_ = &stream_;
        original_handler_ = qInstallMessageHandler(message_handler);
    }
}

TraceFile::impl::~impl() {
    // unhook our message handler before the stream and file are destroyed
    if (original_handler_) {
        qInstallMessageHandler(original_handler_);
    }
    current_stream_ = original_stream_; // revert to prior stream
}

// write Qt messages to the diagnostic log file
void TraceFile::impl::message_handler(QtMsgType type,
                                      QMessageLogContext const &context,
                                      QString const &msg) {
    char const *severity;
    switch (type) {
    case QtDebugMsg:
        severity = "Debug";
        break;

    case QtWarningMsg:
        severity = "Warning";
        break;

    case QtFatalMsg:
        severity = "Fatal";
        break;

    default:
        severity = "Critical";
        break;
    }

    {
        // guard against multiple threads with overlapping messages
        QMutexLocker guard(&lock);
        Q_ASSERT_X(current_stream_, "TraceFile:message_handler",
                   "no stream to write to");
        *current_stream_
            << QDateTime::currentDateTimeUtc().toString(
                   "yyyy-MM-ddTHH:mm:ss.zzzZ")
            << '(' << context.file << ':'
            << context.line /* << ", " << context.function */ << ')' << severity
            << ": " << msg.trimmed() << Qt::endl;
        // Belt-and-suspenders flush: Qt::endl flushes the QTextStream into
        // the QFile, but also force the QFile's own buffer down to disk so
        // a subsequent crash can't lose the line. flush() lives on
        // QFileDevice, not QIODevice, so cast.
        if (auto *fdev =
                qobject_cast<QFileDevice *>(current_stream_->device())) {
            fdev->flush();
        }
    }

    if (QtFatalMsg == type) {
        throw std::runtime_error{"Fatal Qt Error"};
    }
}

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "DhakshaRtspClient.h"

Q_DECLARE_LOGGING_CATEGORY(DhakshaNativeVideoLog)

struct ANativeWindow;
struct AMediaCodec;

/// Android hardware decoder (NDK MediaCodec, synchronous mode) drawing straight onto a GPU surface.
///
/// One worker thread makes every codec call: it feeds queued frames into free input buffers and releases each
/// decoded frame to the surface the moment it is ready. The network thread only queues frames and wakes it, so
/// no thread ever waits on the codec while holding a lock another needs. Frames never wait for the decoder: the
/// queue skips ahead to a key frame when the decoder falls behind. A decoder that fails, or gives no picture for
/// 2 s while frames arrive, is replaced; the camera connection is kept. Synchronous mode needs Android 5, so it
/// runs on every device the 4.2 app supports.
class DhakshaDecoderCore
{
public:
    DhakshaDecoderCore();
    ~DhakshaDecoderCore();

    DhakshaDecoderCore(const DhakshaDecoderCore&) = delete;
    DhakshaDecoderCore& operator=(const DhakshaDecoderCore&) = delete;

    /// The surface decoded frames are drawn on; nullptr detaches it and stops the decoder
    void setOutputWindow(ANativeWindow* window);

    /// A new camera session: decoding restarts at its next key frame, with a new decoder if the format changed
    void startStream(const DhakshaVideoFormat& format);

    /// Stops decoding until the next startStream()
    void stopStream();

    /// One complete H.264/H.265 access unit in byte-stream format
    void queueAccessUnit(QByteArray accessUnit, bool keyFrame);

    /// Called from the decoder thread after each frame is released to the surface (cleared with nullptr)
    void setFrameCallback(std::function<void()> callback);

    quint64 framesRendered() const { return _framesRendered.load(std::memory_order_acquire); }

    /// Frames not decoded: dropped when the decoder fell behind, or while waiting for a key frame
    quint64 framesDropped() const { return _framesDropped.load(std::memory_order_acquire); }

    /// Presentation time of the newest frame released to the surface; the surface reports it per frame
    qint64 lastRenderedTimestampNs() const { return _lastRenderedNs.load(std::memory_order_acquire); }

    QSize videoSize() const;
    QString decoderName() const;

private:
    struct Frame
    {
        QByteArray data;
        bool keyFrame = false;
    };

    void _run();
    void _openCodec();
    void _closeCodec();
    bool _feedOne(qint64 now);
    bool _drainOutputs(qint64 now);
    void _trimQueueLocked();
    void _skipToKeyFrameLocked();

    // _mutex guards everything below; the codec itself is only touched by the worker thread
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::thread _thread;
    bool _quit = false;
    QElapsedTimer _clock;

    ANativeWindow* _window = nullptr;
    bool _windowChanged = false;
    DhakshaVideoFormat _format;
    quint64 _formatGeneration = 0;
    std::deque<Frame> _queue;
    bool _waitKeyFrame = true;
    qint64 _lastInputMs = -1;
    QSize _videoSize;
    QString _decoderName;

    // Worker thread only; unused where there is no hardware decoder
    [[maybe_unused]] AMediaCodec* _codec = nullptr;
    [[maybe_unused]] ANativeWindow* _codecWindow = nullptr;
    [[maybe_unused]] qint64 _heldInput = -1;  ///< Input buffer taken from the decoder while no frame was queued
    [[maybe_unused]] quint64 _codecFormatGeneration = 0;
    [[maybe_unused]] bool _fedSinceOpen = false;
    [[maybe_unused]] qint64 _firstFedMs = 0;
    [[maybe_unused]] qint64 _lastOutputMs = 0;
    [[maybe_unused]] qint64 _retryAtMs = 0;
    [[maybe_unused]] qint64 _nextPtsUs = 0;

    std::atomic<quint64> _framesRendered{0};
    std::atomic<quint64> _framesDropped{0};
    std::atomic<qint64> _lastRenderedNs{0};

    std::mutex _callbackMutex;
    std::function<void()> _frameCallback;
};

/// Low-delay camera video for Android, played the way the camera makers' apps play it: DhakshaRtspClient
/// receives the RTSP stream on its own thread and hands each frame to DhakshaDecoderCore, which decodes it with
/// the hardware decoder straight onto a GPU surface that DhakshaVideoItem shows. No GStreamer, no jitter
/// buffer, no CPU copy of the picture.
class DhakshaNativePlayer : public QObject
{
    Q_OBJECT

public:
    explicit DhakshaNativePlayer(QObject* parent = nullptr);
    ~DhakshaNativePlayer() override;

    /// True on Android, where the hardware decoder can draw on a surface
    static bool supported();

    std::shared_ptr<DhakshaDecoderCore> decoder() const { return _decoder; }

    void start(const QString& url, bool tcp);
    void stop();

    bool running() const { return _running; }

    bool connected() const { return _client.connected(); }

    QString codecName() const;
    QString transportName() const;

private:
    std::shared_ptr<DhakshaDecoderCore> _decoder = std::make_shared<DhakshaDecoderCore>();
    DhakshaRtspClient _client;
    bool _running = false;
};

#include "DhakshaNativePlayer.h"

#include <chrono>
#include <cstring>

#if defined(__android__) || defined(Q_OS_ANDROID)
#define DHAKSHA_NATIVE_VIDEO 1
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

Q_LOGGING_CATEGORY(DhakshaNativeVideoLog, "Dhaksha.NativeVideo")

namespace {
// As in the camera makers' players: a short queue that skips ahead rather than delay the picture
constexpr size_t kMaxQueuedFrames = 20;
#ifdef DHAKSHA_NATIVE_VIDEO
// Frames are shown as soon as they are decoded, so the presentation time only counts frames
constexpr qint64 kPtsStepUs = 33333;
// A decoder that gives no picture this long while frames arrive is replaced; the camera connection is kept
constexpr qint64 kNoOutputResetMs = 2000;
constexpr qint64 kOpenRetryMs = 1000;
constexpr int32_t kMinInputBytes = 1024 * 1024;
constexpr int kFallbackWidth = 1920;
constexpr int kFallbackHeight = 1080;
// How often the worker looks for decoded frames while no new frame wakes it
constexpr std::chrono::milliseconds kPollInterval{2};
constexpr std::chrono::milliseconds kIdleInterval{100};
#endif
}  // namespace

// ----------------------------------------------------------------------------------------------
// DhakshaDecoderCore
// ----------------------------------------------------------------------------------------------

DhakshaDecoderCore::DhakshaDecoderCore()
{
    _clock.start();
#ifdef DHAKSHA_NATIVE_VIDEO
    _thread = std::thread(&DhakshaDecoderCore::_run, this);
#endif
}

DhakshaDecoderCore::~DhakshaDecoderCore()
{
    {
        std::lock_guard<std::mutex> locker(_mutex);
        _quit = true;
    }
    _wake.notify_all();
    if (_thread.joinable()) {
        _thread.join();
    }
#ifdef DHAKSHA_NATIVE_VIDEO
    if (_window) {
        ANativeWindow_release(_window);
        _window = nullptr;
    }
#endif
}

void DhakshaDecoderCore::setOutputWindow(ANativeWindow* window)
{
    std::unique_lock<std::mutex> locker(_mutex);
    if (window == _window) {
        return;
    }
#ifdef DHAKSHA_NATIVE_VIDEO
    if (window) {
        ANativeWindow_acquire(window);
    }
    if (_window) {
        ANativeWindow_release(_window);
    }
#endif
    _window = window;
    _windowChanged = true;
    _wake.notify_all();
    // The caller releases the surface next: wait until the worker has let go of the decoder drawing on it
    (void) _wake.wait_for(locker, std::chrono::seconds(1), [this] { return !_windowChanged || _quit; });
}

void DhakshaDecoderCore::startStream(const DhakshaVideoFormat& format)
{
    std::lock_guard<std::mutex> locker(_mutex);
    if (!(format == _format)) {
        _format = format;
        ++_formatGeneration;
    }
    // Frames of the new session refer to pictures the decoder has not seen until the next key frame
    _queue.clear();
    _waitKeyFrame = true;
    _wake.notify_all();
}

void DhakshaDecoderCore::stopStream()
{
    std::lock_guard<std::mutex> locker(_mutex);
    _format = DhakshaVideoFormat();
    ++_formatGeneration;
    _queue.clear();
    _waitKeyFrame = true;
    _wake.notify_all();
}

void DhakshaDecoderCore::setFrameCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> locker(_callbackMutex);
    _frameCallback = std::move(callback);
}

QSize DhakshaDecoderCore::videoSize() const
{
    std::lock_guard<std::mutex> locker(_mutex);
    return _videoSize;
}

QString DhakshaDecoderCore::decoderName() const
{
    std::lock_guard<std::mutex> locker(_mutex);
    return _decoderName;
}

void DhakshaDecoderCore::queueAccessUnit(QByteArray accessUnit, bool keyFrame)
{
    std::lock_guard<std::mutex> locker(_mutex);
    if (!_format.valid() || accessUnit.isEmpty()) {
        return;
    }
    _lastInputMs = _clock.elapsed();
    if (_waitKeyFrame) {
        if (!keyFrame) {
            _framesDropped.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        _waitKeyFrame = false;
    }
    _queue.push_back({std::move(accessUnit), keyFrame});
    _trimQueueLocked();
    _wake.notify_all();
}

void DhakshaDecoderCore::_trimQueueLocked()
{
    if (_queue.size() > kMaxQueuedFrames) {
        // The decoder fell behind: skip ahead to the newest frames, starting at a key frame so the
        // picture never shows a frame decoded without the ones it depends on
        while (_queue.size() > kMaxQueuedFrames) {
            _queue.pop_front();
            _framesDropped.fetch_add(1, std::memory_order_acq_rel);
        }
        _skipToKeyFrameLocked();
    }
}

void DhakshaDecoderCore::_skipToKeyFrameLocked()
{
    while (!_queue.empty() && !_queue.front().keyFrame) {
        _queue.pop_front();
        _framesDropped.fetch_add(1, std::memory_order_acq_rel);
    }
    if (_queue.empty()) {
        _waitKeyFrame = true;
    }
}

void DhakshaDecoderCore::_run()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    while (true) {
        bool closeCodec = false;
        bool openCodec = false;
        {
            std::unique_lock<std::mutex> locker(_mutex);
            const bool queued = !_queue.empty();
            if (!_quit && !_windowChanged && !(queued && _codec)) {
                (void) _wake.wait_for(locker, _codec ? kPollInterval : kIdleInterval);
            }
            if (_quit) {
                break;
            }
            const qint64 now = _clock.elapsed();
            if (_windowChanged || (_codec && (_codecFormatGeneration != _formatGeneration))) {
                closeCodec = true;
            } else if (_codec && _fedSinceOpen && ((now - qMax(_lastOutputMs, _firstFedMs)) > kNoOutputResetMs) &&
                       ((now - _lastInputMs) < kNoOutputResetMs)) {
                qCWarning(DhakshaNativeVideoLog) << "Restarting the decoder: no picture for 2 s";
                closeCodec = true;
            } else if (!_codec && _window && _format.valid() && !_queue.empty() && (now >= _retryAtMs)) {
                openCodec = true;
            }
        }
        if (closeCodec) {
            _closeCodec();
            continue;
        }
        if (openCodec) {
            _openCodec();
        }
        if (_codec) {
            const qint64 now = _clock.elapsed();
            bool rendered = _drainOutputs(now);
            while (_feedOne(now)) {
            }
            rendered = _drainOutputs(now) || rendered;
            if (rendered) {
                std::lock_guard<std::mutex> locker(_callbackMutex);
                if (_frameCallback) {
                    _frameCallback();
                }
            }
        }
    }
#endif
    _closeCodec();
}

void DhakshaDecoderCore::_openCodec()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    ANativeWindow* window = nullptr;
    DhakshaVideoFormat format;
    quint64 generation = 0;
    {
        std::lock_guard<std::mutex> locker(_mutex);
        window = _window;
        format = _format;
        generation = _formatGeneration;
        if (!window || !format.valid()) {
            return;
        }
        ANativeWindow_acquire(window);
    }

    const qint64 now = _clock.elapsed();
    AMediaCodec* codec = AMediaCodec_createDecoderByType(format.mime());
    if (!codec) {
        qCWarning(DhakshaNativeVideoLog) << "No hardware decoder for" << format.mime();
        _retryAtMs = now + kOpenRetryMs;
        ANativeWindow_release(window);
        return;
    }

    const int width = (format.width > 0) ? format.width : kFallbackWidth;
    const int height = (format.height > 0) ? format.height : kFallbackHeight;
    AMediaFormat* mediaFormat = AMediaFormat_new();
    AMediaFormat_setString(mediaFormat, AMEDIAFORMAT_KEY_MIME, format.mime());
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_HEIGHT, height);
    // The camera's own parameter sets, so the decoder is set up for the stream before its first frame
    AMediaFormat_setBuffer(mediaFormat, "csd-0", format.csd0.constData(), static_cast<size_t>(format.csd0.size()));
    if (!format.csd1.isEmpty()) {
        AMediaFormat_setBuffer(mediaFormat, "csd-1", format.csd1.constData(), static_cast<size_t>(format.csd1.size()));
    }
    // Room for the largest key frame: a frame that does not fit would be lost
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, qMax(kMinInputBytes, width * height));
    // Ask for the decoder's low-latency, real-time behaviour; decoders that do not know a key ignore it
    AMediaFormat_setInt32(mediaFormat, "low-latency", 1);
    AMediaFormat_setInt32(mediaFormat, "priority", 0);
    AMediaFormat_setInt32(mediaFormat, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(mediaFormat, "vendor.rtc-ext-dec-low-latency.enable", 1);
    const bool configured = AMediaCodec_configure(codec, mediaFormat, window, nullptr, 0) == AMEDIA_OK;
    AMediaFormat_delete(mediaFormat);
    if (!configured || (AMediaCodec_start(codec) != AMEDIA_OK)) {
        qCWarning(DhakshaNativeVideoLog) << "Hardware decoder failed to start for" << format.mime() << width << "x"
                                         << height;
        (void) AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        _retryAtMs = now + kOpenRetryMs;
        return;
    }

    _codec = codec;
    _codecWindow = window;
    _codecFormatGeneration = generation;
    _fedSinceOpen = false;
    _firstFedMs = now;
    _lastOutputMs = 0;
    {
        std::lock_guard<std::mutex> locker(_mutex);
        _decoderName = QStringLiteral("hardware %1").arg(QString::fromLatin1(format.mime()));
        if ((format.width > 0) && (format.height > 0)) {
            _videoSize = QSize(format.width, format.height);
        }
        // A fresh decoder must start at a key frame
        _skipToKeyFrameLocked();
    }
    qCDebug(DhakshaNativeVideoLog) << "Decoding" << format.mime() << width << "x" << height;
#endif
}

void DhakshaDecoderCore::_closeCodec()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    if (_codec) {
        (void) AMediaCodec_stop(_codec);
        (void) AMediaCodec_delete(_codec);
        _codec = nullptr;
    }
    if (_codecWindow) {
        ANativeWindow_release(_codecWindow);
        _codecWindow = nullptr;
    }
    _heldInput = -1;
#endif
    _fedSinceOpen = false;
    {
        std::lock_guard<std::mutex> locker(_mutex);
        _windowChanged = false;
        _skipToKeyFrameLocked();
    }
    _wake.notify_all();
}

bool DhakshaDecoderCore::_feedOne(qint64 now)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    if (_heldInput < 0) {
        const ssize_t index = AMediaCodec_dequeueInputBuffer(_codec, 0);
        if (index < 0) {
            return false;
        }
        _heldInput = index;
    }
    Frame frame;
    {
        std::lock_guard<std::mutex> locker(_mutex);
        if (_queue.empty()) {
            return false;
        }
        frame = std::move(_queue.front());
        _queue.pop_front();
    }
    const size_t index = static_cast<size_t>(_heldInput);
    _heldInput = -1;
    const size_t size = static_cast<size_t>(frame.data.size());
    size_t capacity = 0;
    uint8_t* input = AMediaCodec_getInputBuffer(_codec, index, &capacity);
    bool queued = false;
    if (input && (capacity >= size)) {
        memcpy(input, frame.data.constData(), size);
        queued = AMediaCodec_queueInputBuffer(_codec, index, 0, size, static_cast<uint64_t>(_nextPtsUs), 0) == AMEDIA_OK;
    } else {
        qCWarning(DhakshaNativeVideoLog) << "Frame of" << size << "bytes does not fit the decoder input of" << capacity;
        (void) AMediaCodec_queueInputBuffer(_codec, index, 0, 0, 0, 0);
    }
    if (queued) {
        _nextPtsUs += kPtsStepUs;
        if (!_fedSinceOpen) {
            _fedSinceOpen = true;
            _firstFedMs = now;
        }
    } else {
        // Later frames refer to the lost one: skip to the next key frame instead of smearing the picture
        _framesDropped.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> locker(_mutex);
        _skipToKeyFrameLocked();
    }
    return true;
#else
    Q_UNUSED(now)
    return false;
#endif
}

bool DhakshaDecoderCore::_drainOutputs(qint64 now)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    bool rendered = false;
    while (true) {
        AMediaCodecBufferInfo info;
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(_codec, &info, 0);
        if (index >= 0) {
            // Drawn now, not at a presentation time: the newest frame is always the one shown
            const bool render = info.size > 0;
            (void) AMediaCodec_releaseOutputBuffer(_codec, static_cast<size_t>(index), render);
            if (render) {
                rendered = true;
                _lastOutputMs = now;
                _lastRenderedNs.store(info.presentationTimeUs * 1000, std::memory_order_release);
                _framesRendered.fetch_add(1, std::memory_order_acq_rel);
            }
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* format = AMediaCodec_getOutputFormat(_codec);
            if (format) {
                int32_t width = 0, height = 0, left = 0, top = 0, right = 0, bottom = 0;
                QSize size;
                if (AMediaFormat_getInt32(format, "crop-left", &left) && AMediaFormat_getInt32(format, "crop-top", &top) &&
                    AMediaFormat_getInt32(format, "crop-right", &right) &&
                    AMediaFormat_getInt32(format, "crop-bottom", &bottom) && (right > left) && (bottom > top)) {
                    size = QSize(right - left + 1, bottom - top + 1);
                } else if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) &&
                           AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height)) {
                    size = QSize(width, height);
                }
                AMediaFormat_delete(format);
                if (size.isValid()) {
                    std::lock_guard<std::mutex> locker(_mutex);
                    _videoSize = size;
                }
            }
        } else if (index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            break;
        }
    }
    return rendered;
#else
    Q_UNUSED(now)
    return false;
#endif
}

// ----------------------------------------------------------------------------------------------
// DhakshaNativePlayer
// ----------------------------------------------------------------------------------------------

DhakshaNativePlayer::DhakshaNativePlayer(QObject* parent) : QObject(parent) {}

DhakshaNativePlayer::~DhakshaNativePlayer()
{
    stop();
}

bool DhakshaNativePlayer::supported()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    return true;
#else
    return false;
#endif
}

void DhakshaNativePlayer::start(const QString& url, bool tcp)
{
    stop();
    if (url.isEmpty() || !supported()) {
        return;
    }
    const std::shared_ptr<DhakshaDecoderCore> decoder = _decoder;
    DhakshaRtspClient::Callbacks callbacks;
    callbacks.format = [decoder](const DhakshaVideoFormat& format) { decoder->startStream(format); };
    callbacks.accessUnit = [decoder](QByteArray accessUnit, bool keyFrame) {
        decoder->queueAccessUnit(std::move(accessUnit), keyFrame);
    };
    _client.start(url, tcp ? DhakshaRtspClient::Transport::Tcp : DhakshaRtspClient::Transport::Udp,
                  std::move(callbacks));
    _running = true;
    qCDebug(DhakshaNativeVideoLog) << "Playing" << url << (tcp ? "over TCP" : "over UDP");
}

void DhakshaNativePlayer::stop()
{
    _client.stop();
    _decoder->stopStream();
    _running = false;
}

QString DhakshaNativePlayer::codecName() const
{
    return _client.h265() ? QStringLiteral("H.265") : QStringLiteral("H.264");
}

QString DhakshaNativePlayer::transportName() const
{
    return (_client.activeTransport() == DhakshaRtspClient::Transport::Tcp) ? QStringLiteral("TCP")
                                                                            : QStringLiteral("UDP");
}

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QLoggingCategory>
#include <QtCore/QString>
#include <atomic>
#include <functional>
#include <thread>

Q_DECLARE_LOGGING_CATEGORY(DhakshaRtspLog)

/// What the hardware decoder must be told about a stream before its first frame
struct DhakshaVideoFormat
{
    bool h265 = false;
    QByteArray csd0;  ///< H.264 SPS, or H.265 VPS + SPS + PPS; each NAL unit after a 00 00 00 01 start code
    QByteArray csd1;  ///< H.264 PPS; empty for H.265
    int width = 0;    ///< From the SPS; 0 when it could not be read
    int height = 0;

    bool valid() const { return !csd0.isEmpty(); }

    const char* mime() const { return h265 ? "video/hevc" : "video/avc"; }

    bool operator==(const DhakshaVideoFormat& other) const
    {
        return (h265 == other.h265) && (csd0 == other.csd0) && (csd1 == other.csd1) && (width == other.width) &&
               (height == other.height);
    }
};

/// The video track of an RTSP session description (SDP). Parameter sets are raw NAL units.
struct DhakshaSdpVideo
{
    bool found = false;
    bool supported = false;  ///< H.264 or H.265
    bool h265 = false;
    int payloadType = -1;
    QByteArray encoding;
    QByteArray sessionControl;
    QByteArray control;
    QByteArray vps;
    QByteArray sps;
    QByteArray pps;
};

namespace DhakshaRtsp {

DhakshaSdpVideo parseSdp(const QByteArray& sdp);

/// Track or session URL from an SDP a=control value and the session's base URL
QByteArray resolveControl(const QByteArray& baseUrl, const QByteArray& control);

/// Picture size coded in an SPS NAL unit (with its NAL header)
bool parseSpsSize(bool h265, const QByteArray& sps, int& width, int& height);

/// Decoder format from parameter sets; invalid when one is missing
DhakshaVideoFormat makeFormat(bool h265, const QByteArray& vps, const QByteArray& sps, const QByteArray& pps);

}  // namespace DhakshaRtsp

/// Rebuilds complete H.264 / H.265 access units in byte-stream format from RTP packets (RFC 6184 single
/// NAL unit, STAP-A and FU-A; RFC 7798 single NAL unit, AP and FU). A frame that lost packets is still
/// delivered: the decoder conceals the missing slices, which looks better than skipping to a key frame.
class DhakshaRtpDepacketizer
{
public:
    using AccessUnitHandler = std::function<void(QByteArray accessUnit, bool keyFrame)>;

    void setCodec(bool h265, int payloadType);

    void setHandler(AccessUnitHandler handler) { _handler = std::move(handler); }

    /// One RTP packet, header included
    void push(const uint8_t* data, size_t size);

    bool haveSource() const { return _haveSource; }

    quint32 sourceSsrc() const { return _ssrc; }

    /// Highest sequence number received, extended with the wrap-around count (RFC 3550)
    quint32 extendedHighestSequence() const { return _cycles + _lastSequence; }

    quint32 packetsExpected() const { return _haveSource ? (extendedHighestSequence() - _baseSequence + 1) : 0; }

    quint32 packetsReceived() const { return _received; }

    /// Parameter sets most recently seen in the stream itself
    const QByteArray& vps() const { return _vps; }

    const QByteArray& sps() const { return _sps; }

    const QByteArray& pps() const { return _pps; }

private:
    void _pushH264(const uint8_t* payload, size_t size);
    void _pushH265(const uint8_t* payload, size_t size);
    void _addNal(const uint8_t* nal, size_t size);
    void _flush();

    AccessUnitHandler _handler;
    bool _h265 = false;
    int _payloadType = -1;
    bool _haveSource = false;
    quint32 _ssrc = 0;
    quint16 _lastSequence = 0;
    quint32 _cycles = 0;
    quint32 _baseSequence = 0;
    quint32 _received = 0;
    bool _haveTimestamp = false;
    quint32 _timestamp = 0;
    QByteArray _accessUnit;
    bool _keyFrame = false;
    QByteArray _fragment;
    bool _fragmentActive = false;
    QByteArray _vps;
    QByteArray _sps;
    QByteArray _pps;
};

/// RTSP client for the gimbal cameras, written after the camera makers' own players: no GStreamer and no
/// jitter buffer. It runs on a thread of its own (POSIX sockets and poll), receives RTP over TCP
/// (interleaved) or UDP, keeps the session alive with OPTIONS every 30 s and an RTCP receiver report every
/// 2 s, and hands every complete frame on the moment its last packet arrives. When the connection drops it
/// reconnects after 1 s, until stop().
class DhakshaRtspClient
{
public:
    enum class Transport
    {
        Tcp,
        Udp,
    };

    struct Callbacks
    {
        /// A session started playing; called before its first frame
        std::function<void(const DhakshaVideoFormat& format)> format;
        std::function<void(QByteArray accessUnit, bool keyFrame)> accessUnit;
    };

    DhakshaRtspClient() = default;
    ~DhakshaRtspClient();

    DhakshaRtspClient(const DhakshaRtspClient&) = delete;
    DhakshaRtspClient& operator=(const DhakshaRtspClient&) = delete;

    /// Callbacks are called from the client's thread
    void start(const QString& url, Transport transport, Callbacks callbacks);

    /// Ends the session; no callback is running or called once this returns
    void stop();

    bool connected() const { return _connected.load(std::memory_order_acquire); }

    /// What the current session receives over; may differ from the requested one if the camera refused it
    Transport activeTransport() const { return _activeTransport.load(std::memory_order_acquire); }

    bool h265() const { return _h265.load(std::memory_order_acquire); }

    quint64 packetsLost() const { return _packetsLost.load(std::memory_order_acquire); }

private:
    void _run();

    QString _url;
    Transport _transport = Transport::Tcp;
    Callbacks _callbacks;
    std::thread _thread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _connected{false};
    std::atomic<Transport> _activeTransport{Transport::Tcp};
    std::atomic<bool> _h265{false};
    std::atomic<quint64> _packetsLost{0};

    friend class DhakshaRtspSession;
};

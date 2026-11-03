#include "DhakshaRtspClient.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>
#include <chrono>
#include <cstring>

#ifndef Q_OS_WIN
#define DHAKSHA_RTSP_SOCKETS 1
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

Q_LOGGING_CATEGORY(DhakshaRtspLog, "Dhaksha.Rtsp")

namespace {

constexpr int kReconnectDelayMs = 1000;
constexpr int kConnectTimeoutMs = 5000;
constexpr int kResponseTimeoutMs = 5000;
// Nothing received for this long means the camera or the link is gone; over UDP no socket closes to say so
constexpr int kNoDataTimeoutMs = 5000;
constexpr int kKeepaliveMs = 30000;
constexpr int kReceiverReportMs = 2000;
constexpr int kPollMs = 100;
constexpr int kSendTimeoutMs = 2000;
constexpr quint16 kFirstClientPort = 8210;
constexpr int kClientPortAttempts = 20;
constexpr int kSocketBufferBytes = 1024 * 1024;
constexpr int kMaxMessageBytes = 256 * 1024;
// Older packets than this are late duplicates; further back the camera restarted its numbering
constexpr int kMaxMisorder = 100;
// Far above any camera frame: a frame this big can only come from corrupt or endless data and is dropped
constexpr int kMaxAccessUnitBytes = 8 * 1024 * 1024;
constexpr char kUserAgent[] = "DhakshaGroundControl";

const QByteArray kStartCode("\x00\x00\x00\x01", 4);

quint16 read16(const uint8_t* data)
{
    return static_cast<quint16>((data[0] << 8) | data[1]);
}

quint32 read32(const uint8_t* data)
{
    return (quint32(data[0]) << 24) | (quint32(data[1]) << 16) | (quint32(data[2]) << 8) | quint32(data[3]);
}

void write32(uint8_t* data, quint32 value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

bool isH264KeyNal(int type)
{
    return (type == 5) || (type == 7) || (type == 8);
}

bool isH265KeyNal(int type)
{
    // IRAP pictures (BLA, IDR, CRA) and the parameter sets
    return ((type >= 16) && (type <= 21)) || ((type >= 32) && (type <= 34));
}

/// Reads Exp-Golomb coded fields of an SPS after removing its emulation prevention bytes
class BitReader
{
public:
    BitReader(const QByteArray& nal, int headerBytes)
    {
        int zeros = 0;
        for (int i = headerBytes; i < nal.size(); ++i) {
            const uint8_t byte = static_cast<uint8_t>(nal[i]);
            if ((zeros >= 2) && (byte == 3)) {
                zeros = 0;
                continue;
            }
            zeros = (byte == 0) ? (zeros + 1) : 0;
            _data.append(static_cast<char>(byte));
        }
    }

    bool ok() const { return !_overrun; }

    quint32 bit()
    {
        if (_position >= (static_cast<qint64>(_data.size()) * 8)) {
            _overrun = true;
            return 0;
        }
        const quint32 value = (static_cast<uint8_t>(_data[static_cast<int>(_position / 8)]) >> (7 - (_position % 8))) & 1;
        ++_position;
        return value;
    }

    quint32 bits(int count)
    {
        quint32 value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1) | bit();
        }
        return value;
    }

    void skip(int count) { _position += count; }

    quint32 ue()
    {
        int zeros = 0;
        while ((bit() == 0) && ok()) {
            if (++zeros > 31) {
                _overrun = true;
                return 0;
            }
        }
        return ((1u << zeros) - 1) + bits(zeros);
    }

    qint32 se()
    {
        const quint32 value = ue();
        return (value & 1) ? static_cast<qint32>((value + 1) / 2) : -static_cast<qint32>(value / 2);
    }

private:
    QByteArray _data;
    qint64 _position = 0;
    bool _overrun = false;
};

void skipScalingList(BitReader& reader, int size)
{
    int last = 8;
    int next = 8;
    for (int i = 0; i < size; ++i) {
        if (next != 0) {
            next = (last + reader.se() + 256) % 256;
        }
        last = (next == 0) ? last : next;
    }
}

bool parseH264Sps(const QByteArray& sps, int& width, int& height)
{
    BitReader reader(sps, 1);
    const quint32 profile = reader.bits(8);
    reader.skip(16);  // constraint flags, level
    (void) reader.ue();
    quint32 chroma = 1;
    bool separateColourPlanes = false;
    switch (profile) {
        case 100:
        case 110:
        case 122:
        case 244:
        case 44:
        case 83:
        case 86:
        case 118:
        case 128:
        case 138:
        case 139:
        case 134:
        case 135:
            chroma = reader.ue();
            if (chroma == 3) {
                separateColourPlanes = reader.bit();
            }
            (void) reader.ue();  // bit depths
            (void) reader.ue();
            reader.skip(1);
            if (reader.bit()) {
                for (int i = 0; i < ((chroma != 3) ? 8 : 12); ++i) {
                    if (reader.bit()) {
                        skipScalingList(reader, (i < 6) ? 16 : 64);
                    }
                }
            }
            break;
        default:
            break;
    }
    (void) reader.ue();  // log2_max_frame_num_minus4
    const quint32 pocType = reader.ue();
    if (pocType == 0) {
        (void) reader.ue();
    } else if (pocType == 1) {
        reader.skip(1);
        (void) reader.se();
        (void) reader.se();
        const quint32 cycle = qMin(reader.ue(), 255u);
        for (quint32 i = 0; i < cycle; ++i) {
            (void) reader.se();
        }
    }
    (void) reader.ue();  // max_num_ref_frames
    reader.skip(1);
    const quint32 widthMbs = reader.ue() + 1;
    const quint32 heightMapUnits = reader.ue() + 1;
    const quint32 frameMbsOnly = reader.bit();
    if (!frameMbsOnly) {
        reader.skip(1);
    }
    reader.skip(1);
    quint32 cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (reader.bit()) {
        cropLeft = reader.ue();
        cropRight = reader.ue();
        cropTop = reader.ue();
        cropBottom = reader.ue();
    }
    if (!reader.ok()) {
        return false;
    }
    quint32 cropUnitX = 1;
    quint32 cropUnitY = 2 - frameMbsOnly;
    if (!separateColourPlanes && (chroma == 1)) {
        cropUnitX = 2;
        cropUnitY *= 2;
    } else if (!separateColourPlanes && (chroma == 2)) {
        cropUnitX = 2;
    }
    width = static_cast<int>((widthMbs * 16) - ((cropLeft + cropRight) * cropUnitX));
    height = static_cast<int>(((2 - frameMbsOnly) * heightMapUnits * 16) - ((cropTop + cropBottom) * cropUnitY));
    return true;
}

bool parseH265Sps(const QByteArray& sps, int& width, int& height)
{
    BitReader reader(sps, 2);
    reader.skip(4);
    const int maxSubLayersMinus1 = static_cast<int>(reader.bits(3));
    reader.skip(1);
    reader.skip(96);  // general profile, tier and level
    bool profilePresent[8] = {};
    bool levelPresent[8] = {};
    for (int i = 0; i < maxSubLayersMinus1; ++i) {
        profilePresent[i] = reader.bit();
        levelPresent[i] = reader.bit();
    }
    if (maxSubLayersMinus1 > 0) {
        reader.skip(2 * (8 - maxSubLayersMinus1));
    }
    for (int i = 0; i < maxSubLayersMinus1; ++i) {
        reader.skip(profilePresent[i] ? 88 : 0);
        reader.skip(levelPresent[i] ? 8 : 0);
    }
    (void) reader.ue();
    const quint32 chroma = reader.ue();
    bool separateColourPlanes = false;
    if (chroma == 3) {
        separateColourPlanes = reader.bit();
    }
    const quint32 lumaWidth = reader.ue();
    const quint32 lumaHeight = reader.ue();
    quint32 left = 0, right = 0, top = 0, bottom = 0;
    if (reader.bit()) {
        left = reader.ue();
        right = reader.ue();
        top = reader.ue();
        bottom = reader.ue();
    }
    if (!reader.ok()) {
        return false;
    }
    const quint32 subWidth = (!separateColourPlanes && ((chroma == 1) || (chroma == 2))) ? 2 : 1;
    const quint32 subHeight = (!separateColourPlanes && (chroma == 1)) ? 2 : 1;
    width = static_cast<int>(lumaWidth - (subWidth * (left + right)));
    height = static_cast<int>(lumaHeight - (subHeight * (top + bottom)));
    return true;
}

}  // namespace

// ----------------------------------------------------------------------------------------------
// SDP and parameter sets
// ----------------------------------------------------------------------------------------------

DhakshaSdpVideo DhakshaRtsp::parseSdp(const QByteArray& sdp)
{
    DhakshaSdpVideo video;
    bool inMedia = false;
    bool inVideo = false;
    for (QByteArray line : sdp.split('\n')) {
        line = line.trimmed();
        if (line.startsWith("m=")) {
            inMedia = true;
            // Only the first video track is played
            inVideo = !video.found && line.startsWith("m=video");
            if (inVideo) {
                const QList<QByteArray> fields = line.simplified().split(' ');
                video.found = true;
                video.payloadType = (fields.size() > 3) ? fields[3].toInt() : 96;
                // A camera that leaves out the rtpmap line sends H.264
                video.encoding = "H264";
                video.supported = true;
            }
        } else if (line.startsWith("a=control:")) {
            const QByteArray control = line.mid(10).trimmed();
            if (!inMedia) {
                video.sessionControl = control;
            } else if (inVideo) {
                video.control = control;
            }
        } else if (inVideo && line.startsWith("a=rtpmap:")) {
            const QByteArray map = line.mid(9);
            const int space = map.indexOf(' ');
            if ((space > 0) && (map.left(space).toInt() == video.payloadType)) {
                video.encoding = map.mid(space + 1).split('/').first().trimmed().toUpper();
                video.h265 = (video.encoding == "H265") || (video.encoding == "HEVC");
                video.supported = video.h265 || (video.encoding == "H264");
            }
        } else if (inVideo && line.startsWith("a=fmtp:")) {
            const QByteArray fmtp = line.mid(7);
            const int space = fmtp.indexOf(' ');
            if ((space <= 0) || (fmtp.left(space).toInt() != video.payloadType)) {
                continue;
            }
            for (const QByteArray& parameter : fmtp.mid(space + 1).split(';')) {
                const int equals = parameter.indexOf('=');
                if (equals <= 0) {
                    continue;
                }
                const QByteArray key = parameter.left(equals).trimmed().toLower();
                const QByteArray value = parameter.mid(equals + 1).trimmed();
                if (key == "sprop-parameter-sets") {
                    for (const QByteArray& encoded : value.split(',')) {
                        const QByteArray nal = QByteArray::fromBase64(encoded.trimmed());
                        if (nal.isEmpty()) {
                            continue;
                        }
                        const int type = nal[0] & 0x1F;
                        if ((type == 7) && video.sps.isEmpty()) {
                            video.sps = nal;
                        } else if ((type == 8) && video.pps.isEmpty()) {
                            video.pps = nal;
                        }
                    }
                } else if (key == "sprop-vps") {
                    video.vps = QByteArray::fromBase64(value.split(',').first());
                } else if (key == "sprop-sps") {
                    video.sps = QByteArray::fromBase64(value.split(',').first());
                } else if (key == "sprop-pps") {
                    video.pps = QByteArray::fromBase64(value.split(',').first());
                }
            }
        }
    }
    return video;
}

QByteArray DhakshaRtsp::resolveControl(const QByteArray& baseUrl, const QByteArray& control)
{
    if (control.isEmpty() || (control == "*")) {
        return baseUrl;
    }
    if (control.toLower().startsWith("rtsp://") || control.toLower().startsWith("rtsps://")) {
        return control;
    }
    QByteArray url = baseUrl;
    if (!url.endsWith('/')) {
        url.append('/');
    }
    return url.append(control);
}

bool DhakshaRtsp::parseSpsSize(bool h265, const QByteArray& sps, int& width, int& height)
{
    int parsedWidth = 0;
    int parsedHeight = 0;
    if (!(h265 ? parseH265Sps(sps, parsedWidth, parsedHeight) : parseH264Sps(sps, parsedWidth, parsedHeight))) {
        return false;
    }
    if ((parsedWidth < 16) || (parsedHeight < 16) || (parsedWidth > 8192) || (parsedHeight > 8192)) {
        return false;
    }
    width = parsedWidth;
    height = parsedHeight;
    return true;
}

DhakshaVideoFormat DhakshaRtsp::makeFormat(bool h265, const QByteArray& vps, const QByteArray& sps,
                                           const QByteArray& pps)
{
    DhakshaVideoFormat format;
    format.h265 = h265;
    if (sps.isEmpty() || pps.isEmpty() || (h265 && vps.isEmpty())) {
        return format;
    }
    // H.265 decoders take all three parameter sets in csd-0; H.264 ones take the PPS in csd-1
    if (h265) {
        format.csd0 = kStartCode + vps + kStartCode + sps + kStartCode + pps;
    } else {
        format.csd0 = kStartCode + sps;
        format.csd1 = kStartCode + pps;
    }
    (void) parseSpsSize(h265, sps, format.width, format.height);
    return format;
}

// ----------------------------------------------------------------------------------------------
// DhakshaRtpDepacketizer
// ----------------------------------------------------------------------------------------------

void DhakshaRtpDepacketizer::setCodec(bool h265, int payloadType)
{
    _h265 = h265;
    _payloadType = payloadType;
    _haveSource = false;
    _haveTimestamp = false;
    _accessUnit.clear();
    _keyFrame = false;
    _fragment.clear();
    _fragmentActive = false;
    _received = 0;
    _cycles = 0;
}

void DhakshaRtpDepacketizer::push(const uint8_t* data, size_t size)
{
    if (!data || (size < 12) || ((data[0] >> 6) != 2)) {
        return;
    }
    const bool padding = data[0] & 0x20;
    const bool extension = data[0] & 0x10;
    const size_t csrcCount = data[0] & 0x0F;
    const bool marker = data[1] & 0x80;
    const int payloadType = data[1] & 0x7F;
    const quint16 sequence = read16(data + 2);
    const quint32 timestamp = read32(data + 4);
    const quint32 ssrc = read32(data + 8);
    if ((_payloadType >= 0) && (payloadType != _payloadType)) {
        return;
    }

    size_t offset = 12 + (csrcCount * 4);
    if (extension) {
        if (size < (offset + 4)) {
            return;
        }
        offset += 4 + (static_cast<size_t>(read16(data + offset + 2)) * 4);
    }
    // A corrupt header can claim more CSRC or extension words than the packet holds
    if (offset >= size) {
        return;
    }
    size_t end = size;
    if (padding) {
        const size_t paddingBytes = data[size - 1];
        if (paddingBytes >= (size - offset)) {
            return;
        }
        end -= paddingBytes;
    }
    if (offset >= end) {
        return;
    }

    if (!_haveSource || (ssrc != _ssrc)) {
        _haveSource = true;
        _ssrc = ssrc;
        _lastSequence = sequence;
        _cycles = 0;
        _baseSequence = sequence;
        _received = 0;
        _fragmentActive = false;
    } else {
        const int difference = static_cast<qint16>(static_cast<quint16>(sequence - _lastSequence));
        if (difference <= 0) {
            if (difference > -kMaxMisorder) {
                return;
            }
            // The camera restarted its numbering
            _cycles = 0;
            _baseSequence = sequence;
            _received = 0;
            _fragmentActive = false;
        } else if (difference > 1) {
            // A lost fragment would leave a broken NAL unit; the rest of the frame is still used
            _fragmentActive = false;
        }
        if (sequence < _lastSequence) {
            _cycles += 0x10000;
        }
        _lastSequence = sequence;
    }
    ++_received;

    if (_haveTimestamp && (timestamp != _timestamp)) {
        _flush();
    }
    _haveTimestamp = true;
    _timestamp = timestamp;

    if (_h265) {
        _pushH265(data + offset, end - offset);
    } else {
        _pushH264(data + offset, end - offset);
    }
    if (marker) {
        _flush();
    }
}

void DhakshaRtpDepacketizer::_pushH264(const uint8_t* payload, size_t size)
{
    const int type = payload[0] & 0x1F;
    if ((type >= 1) && (type <= 23)) {
        _addNal(payload, size);
    } else if (type == 24) {
        // STAP-A: 16-bit size before each NAL unit
        size_t position = 1;
        while ((position + 2) <= size) {
            const size_t nalSize = read16(payload + position);
            position += 2;
            if ((nalSize == 0) || ((position + nalSize) > size)) {
                break;
            }
            _addNal(payload + position, nalSize);
            position += nalSize;
        }
    } else if ((type == 28) && (size > 2)) {
        // FU-A
        const uint8_t header = payload[1];
        if (header & 0x80) {
            _fragment.clear();
            _fragment.append(static_cast<char>((payload[0] & 0xE0) | (header & 0x1F)));
            _fragmentActive = true;
        }
        if (_fragmentActive && ((_fragment.size() + static_cast<int>(size)) > kMaxAccessUnitBytes)) {
            qCWarning(DhakshaRtspLog) << "Dropped an oversized H.264 fragment";
            _fragment.clear();
            _fragmentActive = false;
        }
        if (_fragmentActive) {
            _fragment.append(reinterpret_cast<const char*>(payload + 2), static_cast<int>(size - 2));
            if (header & 0x40) {
                _addNal(reinterpret_cast<const uint8_t*>(_fragment.constData()), _fragment.size());
                _fragmentActive = false;
            }
        }
    }
}

void DhakshaRtpDepacketizer::_pushH265(const uint8_t* payload, size_t size)
{
    if (size < 3) {
        return;
    }
    const int type = (payload[0] >> 1) & 0x3F;
    if (type < 48) {
        _addNal(payload, size);
    } else if (type == 48) {
        // Aggregation packet: 16-bit size before each NAL unit
        size_t position = 2;
        while ((position + 2) <= size) {
            const size_t nalSize = read16(payload + position);
            position += 2;
            if ((nalSize == 0) || ((position + nalSize) > size)) {
                break;
            }
            _addNal(payload + position, nalSize);
            position += nalSize;
        }
    } else if ((type == 49) && (size > 3)) {
        // Fragmentation unit
        const uint8_t header = payload[2];
        if (header & 0x80) {
            _fragment.clear();
            _fragment.append(static_cast<char>((payload[0] & 0x81) | ((header & 0x3F) << 1)));
            _fragment.append(static_cast<char>(payload[1]));
            _fragmentActive = true;
        }
        if (_fragmentActive && ((_fragment.size() + static_cast<int>(size)) > kMaxAccessUnitBytes)) {
            qCWarning(DhakshaRtspLog) << "Dropped an oversized H.265 fragment";
            _fragment.clear();
            _fragmentActive = false;
        }
        if (_fragmentActive) {
            _fragment.append(reinterpret_cast<const char*>(payload + 3), static_cast<int>(size - 3));
            if (header & 0x40) {
                _addNal(reinterpret_cast<const uint8_t*>(_fragment.constData()), _fragment.size());
                _fragmentActive = false;
            }
        }
    }
}

void DhakshaRtpDepacketizer::_addNal(const uint8_t* nal, size_t size)
{
    if (size == 0) {
        return;
    }
    if ((_accessUnit.size() + static_cast<int>(size)) > kMaxAccessUnitBytes) {
        // Only corrupt or endless data gets here: drop what was collected so memory stays bounded
        qCWarning(DhakshaRtspLog) << "Dropped an oversized video frame";
        _accessUnit.clear();
        _keyFrame = false;
        return;
    }
    const QByteArray unit(reinterpret_cast<const char*>(nal), static_cast<int>(size));
    if (_h265) {
        const int type = (nal[0] >> 1) & 0x3F;
        _keyFrame = _keyFrame || isH265KeyNal(type);
        if (type == 32) {
            _vps = unit;
        } else if (type == 33) {
            _sps = unit;
        } else if (type == 34) {
            _pps = unit;
        }
    } else {
        const int type = nal[0] & 0x1F;
        _keyFrame = _keyFrame || isH264KeyNal(type);
        if (type == 7) {
            _sps = unit;
        } else if (type == 8) {
            _pps = unit;
        }
    }
    _accessUnit.append(kStartCode);
    _accessUnit.append(unit);
}

void DhakshaRtpDepacketizer::_flush()
{
    _fragmentActive = false;
    if (_accessUnit.isEmpty()) {
        return;
    }
    QByteArray accessUnit = std::move(_accessUnit);
    _accessUnit = QByteArray();
    const bool keyFrame = _keyFrame;
    _keyFrame = false;
    if (_handler) {
        _handler(std::move(accessUnit), keyFrame);
    }
}

// ----------------------------------------------------------------------------------------------
// DhakshaRtspSession: one connection to the camera, from OPTIONS to TEARDOWN
// ----------------------------------------------------------------------------------------------

#ifdef DHAKSHA_RTSP_SOCKETS

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

struct RtspResponse
{
    int status = 0;
    QList<QPair<QByteArray, QByteArray>> headers;  ///< Names in lower case
    QByteArray body;

    QByteArray header(const QByteArray& name) const
    {
        for (const auto& entry : headers) {
            if (entry.first == name) {
                return entry.second;
            }
        }
        return {};
    }
};

QByteArray md5Hex(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
}

/// key=value and key="value" pairs of a WWW-Authenticate challenge, after the scheme
QList<QPair<QByteArray, QByteArray>> parseAuthParameters(const QByteArray& text)
{
    QList<QPair<QByteArray, QByteArray>> parameters;
    int position = 0;
    while (position < text.size()) {
        while ((position < text.size()) && ((text[position] == ' ') || (text[position] == ','))) {
            ++position;
        }
        const int equals = text.indexOf('=', position);
        if (equals < 0) {
            break;
        }
        const QByteArray key = text.mid(position, equals - position).trimmed().toLower();
        position = equals + 1;
        QByteArray value;
        if ((position < text.size()) && (text[position] == '"')) {
            const int close = text.indexOf('"', position + 1);
            value = text.mid(position + 1, (close < 0) ? -1 : (close - position - 1));
            position = (close < 0) ? text.size() : (close + 1);
        } else {
            const int comma = text.indexOf(',', position);
            value = text.mid(position, (comma < 0) ? -1 : (comma - position)).trimmed();
            position = (comma < 0) ? text.size() : comma;
        }
        parameters.append({key, value});
    }
    return parameters;
}

QByteArray parameterValue(const QList<QPair<QByteArray, QByteArray>>& parameters, const QByteArray& key)
{
    for (const auto& parameter : parameters) {
        if (parameter.first == key) {
            return parameter.second;
        }
    }
    return {};
}

void closeSocket(int& fd)
{
    if (fd >= 0) {
        (void) ::close(fd);
        fd = -1;
    }
}

bool setNonBlocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return (flags >= 0) && (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

}  // namespace

class DhakshaRtspSession
{
public:
    explicit DhakshaRtspSession(DhakshaRtspClient& client) : _client(client) { _clock.start(); }

    ~DhakshaRtspSession()
    {
        _client._connected.store(false, std::memory_order_release);
        closeSocket(_tcp);
        closeSocket(_rtpUdp);
        closeSocket(_rtcpUdp);
    }

    void run();

private:
    enum class Auth
    {
        None,
        Basic,
        Digest,
    };

    bool _running() const { return _client._running.load(std::memory_order_acquire); }

    bool _connect();
    bool _openUdpPorts();
    bool _send(const QByteArray& data);
    bool _readTcp();
    bool _takeMessage(RtspResponse* response);
    void _compactReceived();
    bool _request(const QByteArray& method, const QByteArray& uri, const QByteArray& headers, RtspResponse& response);
    QByteArray _buildRequest(const QByteArray& method, const QByteArray& uri, const QByteArray& headers, int cseq);
    QByteArray _authorization(const QByteArray& method, const QByteArray& uri);
    bool _setAuthentication(const RtspResponse& response);
    bool _setup(const QByteArray& trackUrl, bool tcp, RtspResponse& response);
    void _play();
    void _drainUdp(int fd, bool rtp);
    void _sendReceiverReport();
    void _sendKeepalive();
    void _onAccessUnit(QByteArray accessUnit, bool keyFrame);

    DhakshaRtspClient& _client;
    QElapsedTimer _clock;
    QByteArray _url;
    QByteArray _host;
    quint16 _port = 554;
    QByteArray _user;
    QByteArray _password;
    QByteArray _baseUrl;
    int _tcp = -1;
    int _rtpUdp = -1;
    int _rtcpUdp = -1;
    quint16 _clientRtpPort = 0;
    sockaddr_storage _serverRtcp{};
    socklen_t _serverRtcpLength = 0;
    QByteArray _received;
    int _receivedPosition = 0;
    int _cseq = 0;
    QByteArray _session;
    int _sessionTimeoutS = 60;
    Auth _auth = Auth::None;
    QByteArray _realm;
    QByteArray _nonce;
    QByteArray _opaque;
    bool _qopAuth = false;
    int _nonceCount = 0;
    bool _tcpTransport = true;
    int _rtpChannel = 0;
    int _rtcpChannel = 1;
    DhakshaSdpVideo _sdp;
    DhakshaRtpDepacketizer _depacketizer;
    bool _formatSent = false;
    quint32 _clientSsrc = QRandomGenerator::global()->generate();
    quint32 _reportedExpected = 0;
    quint32 _reportedReceived = 0;
    qint64 _lastDataMs = 0;
    qint64 _lastKeepaliveMs = 0;
    qint64 _lastReportMs = 0;
    bool _sessionLost = false;
};

void DhakshaRtspSession::run()
{
    const QUrl url(_client._url);
    _host = url.host().toUtf8();
    _port = static_cast<quint16>(url.port(554));
    _user = url.userName(QUrl::FullyDecoded).toUtf8();
    _password = url.password(QUrl::FullyDecoded).toUtf8();
    _url = url.adjusted(QUrl::RemoveUserInfo).toString(QUrl::FullyEncoded).toUtf8();
    if (_host.isEmpty() || !_connect()) {
        return;
    }

    RtspResponse response;
    if (!_request("OPTIONS", _url, {}, response)) {
        return;
    }
    if (!_request("DESCRIBE", _url, "Accept: application/sdp\r\n", response)) {
        return;
    }
    if (response.status != 200) {
        qCWarning(DhakshaRtspLog) << "DESCRIBE" << _url << "answered" << response.status;
        return;
    }
    _baseUrl = response.header("content-base");
    if (_baseUrl.isEmpty()) {
        _baseUrl = response.header("content-location");
    }
    if (_baseUrl.isEmpty()) {
        _baseUrl = _url;
    }
    _sdp = DhakshaRtsp::parseSdp(response.body);
    if (!_sdp.found || !_sdp.supported) {
        qCWarning(DhakshaRtspLog) << "No H.264 or H.265 video in" << _url << _sdp.encoding;
        return;
    }
    _client._h265.store(_sdp.h265, std::memory_order_release);
    _depacketizer.setCodec(_sdp.h265, _sdp.payloadType);
    _depacketizer.setHandler(
        [this](QByteArray accessUnit, bool keyFrame) { _onAccessUnit(std::move(accessUnit), keyFrame); });

    const QByteArray trackUrl = DhakshaRtsp::resolveControl(_baseUrl, _sdp.control);
    const bool tcp = _client._transport == DhakshaRtspClient::Transport::Tcp;
    if (!_setup(trackUrl, tcp, response)) {
        return;
    }
    if ((response.status == 461) && tcp) {
        qCWarning(DhakshaRtspLog) << "Camera refused RTP over TCP, using UDP";
        if (!_setup(trackUrl, false, response)) {
            return;
        }
    }
    if (response.status != 200) {
        qCWarning(DhakshaRtspLog) << "SETUP" << trackUrl << "answered" << response.status;
        return;
    }
    const QByteArray session = response.header("session");
    _session = session.split(';').first().trimmed();
    for (const QByteArray& field : session.split(';')) {
        if (field.trimmed().startsWith("timeout=")) {
            _sessionTimeoutS = qMax(field.trimmed().mid(8).toInt(), 10);
        }
    }
    for (const QByteArray& field : response.header("transport").split(';')) {
        const QByteArray parameter = field.trimmed();
        if (parameter.startsWith("interleaved=")) {
            const QList<QByteArray> channels = parameter.mid(12).split('-');
            _rtpChannel = channels.first().toInt();
            _rtcpChannel = (channels.size() > 1) ? channels[1].toInt() : (_rtpChannel + 1);
        } else if (parameter.startsWith("server_port=") && !_tcpTransport) {
            const int serverRtcpPort = (parameter.mid(12).split('-').size() > 1)
                                           ? parameter.mid(12).split('-')[1].toInt()
                                           : (parameter.mid(12).split('-').first().toInt() + 1);
            sockaddr_storage peer{};
            socklen_t peerLength = sizeof(peer);
            if (getpeername(_tcp, reinterpret_cast<sockaddr*>(&peer), &peerLength) == 0) {
                if (peer.ss_family == AF_INET) {
                    reinterpret_cast<sockaddr_in*>(&peer)->sin_port = htons(static_cast<quint16>(serverRtcpPort));
                } else if (peer.ss_family == AF_INET6) {
                    reinterpret_cast<sockaddr_in6*>(&peer)->sin6_port = htons(static_cast<quint16>(serverRtcpPort));
                }
                _serverRtcp = peer;
                _serverRtcpLength = peerLength;
            }
        }
    }

    const QByteArray playUrl =
        _sdp.sessionControl.isEmpty() ? _baseUrl : DhakshaRtsp::resolveControl(_baseUrl, _sdp.sessionControl);
    if (!_request("PLAY", playUrl, "Range: npt=0.000-\r\n", response)) {
        return;
    }
    if (response.status != 200) {
        qCWarning(DhakshaRtspLog) << "PLAY" << playUrl << "answered" << response.status;
        return;
    }
    _play();
    if (!_running() && !_sessionLost) {
        (void) _send(_buildRequest("TEARDOWN", playUrl, {}, ++_cseq));
    }
}

bool DhakshaRtspSession::_connect()
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if ((getaddrinfo(_host.constData(), QByteArray::number(_port).constData(), &hints, &addresses) != 0) ||
        !addresses) {
        qCWarning(DhakshaRtspLog) << "Cannot resolve" << _host;
        return false;
    }
    for (addrinfo* address = addresses; address && (_tcp < 0) && _running(); address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if ((fd < 0) || !setNonBlocking(fd)) {
            closeSocket(fd);
            continue;
        }
        if ((::connect(fd, address->ai_addr, address->ai_addrlen) != 0) && (errno != EINPROGRESS)) {
            closeSocket(fd);
            continue;
        }
        const qint64 deadline = _clock.elapsed() + kConnectTimeoutMs;
        bool connected = false;
        while (_running() && (_clock.elapsed() < deadline)) {
            pollfd descriptor{fd, POLLOUT, 0};
            if (poll(&descriptor, 1, kPollMs) > 0) {
                int error = 0;
                socklen_t length = sizeof(error);
                connected = (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0) && (error == 0);
                break;
            }
        }
        if (!connected) {
            closeSocket(fd);
            continue;
        }
        const int enable = 1;
        (void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
#ifdef SO_NOSIGPIPE
        (void) setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
#endif
        (void) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &kSocketBufferBytes, sizeof(kSocketBufferBytes));
        _tcp = fd;
    }
    freeaddrinfo(addresses);
    if (_tcp < 0) {
        qCInfo(DhakshaRtspLog) << "Cannot connect to" << _host << _port;
        return false;
    }
    return true;
}

bool DhakshaRtspSession::_openUdpPorts()
{
    closeSocket(_rtpUdp);
    closeSocket(_rtcpUdp);
    // The camera makers' players receive on 8210-8211; another pair is used when those are taken
    for (int attempt = 0; attempt < kClientPortAttempts; ++attempt) {
        const quint16 port = static_cast<quint16>(kFirstClientPort + (attempt * 2));
        int fds[2] = {socket(AF_INET, SOCK_DGRAM, 0), socket(AF_INET, SOCK_DGRAM, 0)};
        bool ok = (fds[0] >= 0) && (fds[1] >= 0);
        for (int i = 0; ok && (i < 2); ++i) {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = htons(static_cast<quint16>(port + i));
            ok = (bind(fds[i], reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0) && setNonBlocking(fds[i]);
        }
        if (ok) {
            (void) setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &kSocketBufferBytes, sizeof(kSocketBufferBytes));
            _rtpUdp = fds[0];
            _rtcpUdp = fds[1];
            _clientRtpPort = port;
            return true;
        }
        closeSocket(fds[0]);
        closeSocket(fds[1]);
    }
    qCWarning(DhakshaRtspLog) << "No free UDP port pair for RTP";
    return false;
}

bool DhakshaRtspSession::_send(const QByteArray& data)
{
    int sent = 0;
    const qint64 deadline = _clock.elapsed() + kSendTimeoutMs;
    while (sent < data.size()) {
        const ssize_t count =
            ::send(_tcp, data.constData() + sent, static_cast<size_t>(data.size() - sent), kSendFlags);
        if (count > 0) {
            sent += count;
            continue;
        }
        if ((count < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) &&
            (_clock.elapsed() < deadline)) {
            pollfd descriptor{_tcp, POLLOUT, 0};
            (void) poll(&descriptor, 1, kPollMs);
            continue;
        }
        _sessionLost = true;
        return false;
    }
    return true;
}

bool DhakshaRtspSession::_readTcp()
{
    char buffer[64 * 1024];
    while (true) {
        const ssize_t count = ::recv(_tcp, buffer, sizeof(buffer), 0);
        if (count > 0) {
            _received.append(buffer, count);
            continue;
        }
        if ((count < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))) {
            return true;
        }
        qCInfo(DhakshaRtspLog) << "Camera closed the connection";
        _sessionLost = true;
        return false;
    }
}

bool DhakshaRtspSession::_takeMessage(RtspResponse* response)
{
    while (_receivedPosition < _received.size()) {
        const auto* data = reinterpret_cast<const uint8_t*>(_received.constData()) + _receivedPosition;
        const int available = _received.size() - _receivedPosition;
        if (data[0] == '$') {
            // Interleaved RTP or RTCP: '$', channel, 16-bit length
            if (available < 4) {
                return false;
            }
            const int length = read16(data + 2);
            if (available < (4 + length)) {
                return false;
            }
            if (data[1] == _rtpChannel) {
                _lastDataMs = _clock.elapsed();
                _depacketizer.push(data + 4, static_cast<size_t>(length));
            }
            _receivedPosition += 4 + length;
            continue;
        }
        if ((data[0] < 'A') || (data[0] > 'Z')) {
            // Not the start of a message: skip to the next one
            ++_receivedPosition;
            continue;
        }
        const int headerEnd = _received.indexOf("\r\n\r\n", _receivedPosition);
        if (headerEnd < 0) {
            if (available > kMaxMessageBytes) {
                _receivedPosition = _received.size();
            }
            return false;
        }
        const QList<QByteArray> lines = _received.mid(_receivedPosition, headerEnd - _receivedPosition).split('\n');
        RtspResponse message;
        int contentLength = 0;
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(':');
            if (colon <= 0) {
                continue;
            }
            const QByteArray name = lines[i].left(colon).trimmed().toLower();
            const QByteArray value = lines[i].mid(colon + 1).trimmed();
            if (name == "content-length") {
                contentLength = qBound<int>(0, value.toLongLong(), int(kMaxMessageBytes));
            }
            message.headers.append({name, value});
        }
        if ((_received.size() - (headerEnd + 4)) < contentLength) {
            return false;
        }
        message.body = _received.mid(headerEnd + 4, contentLength);
        _receivedPosition = headerEnd + 4 + contentLength;
        const QByteArray statusLine = lines.first().trimmed();
        if (statusLine.startsWith("RTSP/")) {
            const QList<QByteArray> fields = statusLine.split(' ');
            message.status = (fields.size() > 1) ? fields[1].toInt() : 0;
            if (response) {
                *response = std::move(message);
            }
            return true;
        }
        // A request from the camera (e.g. GET_PARAMETER): nothing needs it answered
    }
    return false;
}

void DhakshaRtspSession::_compactReceived()
{
    if (_receivedPosition > 0) {
        _received.remove(0, _receivedPosition);
        _receivedPosition = 0;
    }
}

QByteArray DhakshaRtspSession::_buildRequest(const QByteArray& method, const QByteArray& uri, const QByteArray& headers,
                                             int cseq)
{
    QByteArray request = method + ' ' + uri + " RTSP/1.0\r\nCSeq: " + QByteArray::number(cseq) +
                         "\r\nUser-Agent: " + kUserAgent + "\r\n";
    request += _authorization(method, uri);
    if (!_session.isEmpty()) {
        request += "Session: " + _session + "\r\n";
    }
    request += headers;
    request += "\r\n";
    return request;
}

QByteArray DhakshaRtspSession::_authorization(const QByteArray& method, const QByteArray& uri)
{
    switch (_auth) {
        case Auth::Basic:
            return "Authorization: Basic " + QByteArray(_user + ':' + _password).toBase64() + "\r\n";
        case Auth::Digest: {
            const QByteArray ha1 = md5Hex(_user + ':' + _realm + ':' + _password);
            const QByteArray ha2 = md5Hex(method + ':' + uri);
            QByteArray header = "Authorization: Digest username=\"" + _user + "\", realm=\"" + _realm + "\", nonce=\"" +
                                _nonce + "\", uri=\"" + uri + "\"";
            if (_qopAuth) {
                const QByteArray nc = QByteArray::number(++_nonceCount, 16).rightJustified(8, '0');
                const QByteArray cnonce = QByteArray::number(QRandomGenerator::global()->generate(), 16);
                header += ", response=\"" + md5Hex(ha1 + ':' + _nonce + ':' + nc + ':' + cnonce + ":auth:" + ha2) +
                          "\", qop=auth, nc=" + nc + ", cnonce=\"" + cnonce + "\"";
            } else {
                header += ", response=\"" + md5Hex(ha1 + ':' + _nonce + ':' + ha2) + "\"";
            }
            if (!_opaque.isEmpty()) {
                header += ", opaque=\"" + _opaque + "\"";
            }
            return header + "\r\n";
        }
        case Auth::None:
            break;
    }
    return {};
}

bool DhakshaRtspSession::_setAuthentication(const RtspResponse& response)
{
    bool basic = false;
    for (const auto& header : response.headers) {
        if (header.first != "www-authenticate") {
            continue;
        }
        if (header.second.toLower().startsWith("digest")) {
            const auto parameters = parseAuthParameters(header.second.mid(6));
            _realm = parameterValue(parameters, "realm");
            _nonce = parameterValue(parameters, "nonce");
            _opaque = parameterValue(parameters, "opaque");
            _qopAuth = false;
            for (const QByteArray& qop : parameterValue(parameters, "qop").split(',')) {
                _qopAuth = _qopAuth || (qop.trimmed() == "auth");
            }
            _nonceCount = 0;
            _auth = Auth::Digest;
            return true;
        }
        basic = basic || header.second.toLower().startsWith("basic");
    }
    if (basic) {
        _auth = Auth::Basic;
        return true;
    }
    return false;
}

bool DhakshaRtspSession::_request(const QByteArray& method, const QByteArray& uri, const QByteArray& headers,
                                  RtspResponse& response)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int cseq = ++_cseq;
        if (!_send(_buildRequest(method, uri, headers, cseq))) {
            return false;
        }
        const qint64 deadline = _clock.elapsed() + kResponseTimeoutMs;
        bool answered = false;
        while (_running() && !answered) {
            while (_takeMessage(&response)) {
                const QByteArray responseCseq = response.header("cseq");
                if (responseCseq.isEmpty() || (responseCseq.toInt() == cseq)) {
                    answered = true;
                    break;
                }
            }
            _compactReceived();
            if (answered) {
                break;
            }
            if (_clock.elapsed() > deadline) {
                qCWarning(DhakshaRtspLog) << "No answer to" << method << uri;
                return false;
            }
            pollfd descriptor{_tcp, POLLIN, 0};
            if ((poll(&descriptor, 1, kPollMs) > 0) && !_readTcp()) {
                return false;
            }
        }
        if (!answered) {
            return false;
        }
        if ((response.status == 401) && (attempt == 0) && !_user.isEmpty() && _setAuthentication(response)) {
            continue;
        }
        if (response.status == 401) {
            qCWarning(DhakshaRtspLog) << "Camera refused the user name or password for" << uri;
        }
        return true;
    }
    return true;
}

bool DhakshaRtspSession::_setup(const QByteArray& trackUrl, bool tcp, RtspResponse& response)
{
    _tcpTransport = tcp;
    QByteArray transport;
    if (tcp) {
        transport = "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
    } else {
        if (!_openUdpPorts()) {
            return false;
        }
        transport = "Transport: RTP/AVP;unicast;client_port=" + QByteArray::number(_clientRtpPort) + '-' +
                    QByteArray::number(_clientRtpPort + 1) + "\r\n";
    }
    return _request("SETUP", trackUrl, transport, response);
}

void DhakshaRtspSession::_play()
{
    _client._activeTransport.store(
        _tcpTransport ? DhakshaRtspClient::Transport::Tcp : DhakshaRtspClient::Transport::Udp,
        std::memory_order_release);
    _client._connected.store(true, std::memory_order_release);
    qCInfo(DhakshaRtspLog) << "Playing" << _url << (_sdp.h265 ? "H.265" : "H.264") << "over"
                            << (_tcpTransport ? "TCP" : "UDP");

    const qint64 keepaliveMs = qMin<qint64>(kKeepaliveMs, qMax(5, _sessionTimeoutS / 2) * 1000LL);
    _lastDataMs = _clock.elapsed();
    _lastKeepaliveMs = _lastDataMs;
    _lastReportMs = _lastDataMs;
    // Frames that arrived together with the PLAY answer
    while (_takeMessage(nullptr)) {
    }
    _compactReceived();

    while (_running()) {
        pollfd descriptors[3] = {{_tcp, POLLIN, 0}, {_rtpUdp, POLLIN, 0}, {_rtcpUdp, POLLIN, 0}};
        const nfds_t count = _tcpTransport ? 1 : 3;
        if (poll(descriptors, count, kPollMs) > 0) {
            if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!_readTcp()) {
                    return;
                }
                RtspResponse response;
                while (_takeMessage(&response)) {
                    if (response.status == 454) {
                        qCWarning(DhakshaRtspLog) << "Camera ended the session";
                        _sessionLost = true;
                        return;
                    }
                }
                _compactReceived();
            }
            if ((count > 1) && (descriptors[1].revents & POLLIN)) {
                _drainUdp(_rtpUdp, true);
            }
            if ((count > 2) && (descriptors[2].revents & POLLIN)) {
                _drainUdp(_rtcpUdp, false);
            }
        }
        const qint64 now = _clock.elapsed();
        if ((now - _lastDataMs) > kNoDataTimeoutMs) {
            qCWarning(DhakshaRtspLog) << "No video from" << _url << "for" << kNoDataTimeoutMs << "ms";
            _sessionLost = true;
            return;
        }
        if ((now - _lastKeepaliveMs) >= keepaliveMs) {
            _sendKeepalive();
        }
        if ((now - _lastReportMs) >= kReceiverReportMs) {
            _sendReceiverReport();
        }
    }
}

void DhakshaRtspSession::_drainUdp(int fd, bool rtp)
{
    uint8_t buffer[64 * 1024];
    while (true) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return;
        }
        if (rtp) {
            _lastDataMs = _clock.elapsed();
            _depacketizer.push(buffer, static_cast<size_t>(count));
        }
    }
}

void DhakshaRtspSession::_sendKeepalive()
{
    _lastKeepaliveMs = _clock.elapsed();
    // The answer arrives in the stream and is skipped there
    (void) _send(_buildRequest("OPTIONS", _url, {}, ++_cseq));
}

void DhakshaRtspSession::_sendReceiverReport()
{
    _lastReportMs = _clock.elapsed();
    if (!_depacketizer.haveSource()) {
        return;
    }
    const quint32 expected = _depacketizer.packetsExpected();
    const quint32 received = _depacketizer.packetsReceived();
    const qint64 lost = qMax<qint64>(0, static_cast<qint64>(expected) - received);
    const qint64 expectedInterval = static_cast<qint64>(expected) - _reportedExpected;
    const qint64 lostInterval = expectedInterval - (static_cast<qint64>(received) - _reportedReceived);
    _reportedExpected = expected;
    _reportedReceived = received;
    _client._packetsLost.store(static_cast<quint64>(lost), std::memory_order_release);

    // RTCP receiver report (RFC 3550 6.4.2) with one report block
    uint8_t packet[4 + 32] = {'$', static_cast<uint8_t>(_rtcpChannel), 0, 32};
    uint8_t* report = packet + 4;
    report[0] = 0x81;
    report[1] = 201;
    report[2] = 0;
    report[3] = 7;
    write32(report + 4, _clientSsrc);
    write32(report + 8, _depacketizer.sourceSsrc());
    const quint32 cumulativeLost = static_cast<quint32>(qMin<qint64>(lost, 0x7FFFFF));
    write32(report + 12, cumulativeLost);
    report[12] = ((expectedInterval > 0) && (lostInterval > 0))
                     ? static_cast<uint8_t>(qMin<qint64>(255, (lostInterval << 8) / expectedInterval))
                     : 0;
    write32(report + 16, _depacketizer.extendedHighestSequence());
    write32(report + 20, 0);  // jitter
    write32(report + 24, 0);  // no sender report used
    write32(report + 28, 0);

    if (_tcpTransport) {
        (void) _send(QByteArray(reinterpret_cast<const char*>(packet), sizeof(packet)));
    } else if ((_rtcpUdp >= 0) && (_serverRtcpLength > 0)) {
        (void) ::sendto(_rtcpUdp, report, 32, 0, reinterpret_cast<const sockaddr*>(&_serverRtcp), _serverRtcpLength);
    }
}

void DhakshaRtspSession::_onAccessUnit(QByteArray accessUnit, bool keyFrame)
{
    if (!_formatSent) {
        DhakshaVideoFormat format = DhakshaRtsp::makeFormat(_sdp.h265, _sdp.vps, _sdp.sps, _sdp.pps);
        if (!format.valid()) {
            format = DhakshaRtsp::makeFormat(_sdp.h265, _depacketizer.vps(), _depacketizer.sps(), _depacketizer.pps());
        }
        if (!format.valid()) {
            // The camera only sends its parameter sets in the stream: wait for them
            return;
        }
        _formatSent = true;
        qCInfo(DhakshaRtspLog) << "Video" << format.mime() << format.width << "x" << format.height;
        if (_client._callbacks.format) {
            _client._callbacks.format(format);
        }
    }
    if (_client._callbacks.accessUnit) {
        _client._callbacks.accessUnit(std::move(accessUnit), keyFrame);
    }
}

#endif  // DHAKSHA_RTSP_SOCKETS

// ----------------------------------------------------------------------------------------------
// DhakshaRtspClient
// ----------------------------------------------------------------------------------------------

DhakshaRtspClient::~DhakshaRtspClient()
{
    stop();
}

void DhakshaRtspClient::start(const QString& url, Transport transport, Callbacks callbacks)
{
    stop();
    _url = url;
    _transport = transport;
    _callbacks = std::move(callbacks);
    _packetsLost.store(0);
    _running.store(true, std::memory_order_release);
    _thread = std::thread(&DhakshaRtspClient::_run, this);
}

void DhakshaRtspClient::stop()
{
    _running.store(false, std::memory_order_release);
    if (_thread.joinable()) {
        _thread.join();
    }
    _connected.store(false, std::memory_order_release);
}

void DhakshaRtspClient::_run()
{
#ifdef DHAKSHA_RTSP_SOCKETS
    while (_running.load(std::memory_order_acquire)) {
        {
            DhakshaRtspSession session(*this);
            session.run();
        }
        // Reconnect a second after the connection was lost, as the camera makers' players do
        QElapsedTimer wait;
        wait.start();
        while (_running.load(std::memory_order_acquire) && (wait.elapsed() < kReconnectDelayMs)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
    }
#endif
}

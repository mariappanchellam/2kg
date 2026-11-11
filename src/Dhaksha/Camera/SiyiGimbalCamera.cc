/****************************************************************************
 *
 * DhakshaGroundControl: SIYI A8 mini gimbal camera on the RC's Ethernet.
 *
 ****************************************************************************/

#include "SiyiGimbalCamera.h"

#include <QUdpSocket>
#include <QtEndian>

namespace {
    constexpr quint8 kStx1      = 0x55;
    constexpr quint8 kStx2      = 0x66;
    constexpr quint8 kCtrlAck   = 0x01;     // as in SIYI's examples
    constexpr int    kHeaderSize = 8;       // stx 2, ctrl 1, length 2, sequence 2, command 1
    constexpr int    kCrcSize   = 2;

    QByteArray int16Bytes(qint16 value)
    {
        QByteArray bytes(2, 0);
        qToLittleEndian<qint16>(value, bytes.data());
        return bytes;
    }
}

SiyiGimbalCamera::SiyiGimbalCamera(const QString& cameraIp, const QString& rtspUrl, QObject* parent)
    : DhakshaGimbalCamera   (QStringLiteral("A8 mini"), rtspUrl, parent)
    , _socket               (new QUdpSocket(this))
    , _cameraAddress        (cameraIp)
{
    // The camera answers to the port the request came from, so any free local port works
    _socket->bind(QHostAddress(QHostAddress::AnyIPv4), 0);
    connect(_socket, &QUdpSocket::readyRead, this, &SiyiGimbalCamera::_readBytes);
    _logCommand(tr("A8 mini control to %1:%2").arg(cameraIp).arg(kControlPort));

    _pollTimer.setInterval(kPollIntervalMs);
    connect(&_pollTimer, &QTimer::timeout, this, &SiyiGimbalCamera::_pollStatus);
    _pollTimer.start();

    _rateTimer.setInterval(kRateResendMs);
    connect(&_rateTimer, &QTimer::timeout, this, &SiyiGimbalCamera::_resendRates);

    _recordCheckTimer.setSingleShot(true);
    _recordCheckTimer.setInterval(kRecordCheckMs);
    connect(&_recordCheckTimer, &QTimer::timeout, this, &SiyiGimbalCamera::_checkRecording);

    _pollStatus();
}

quint16 SiyiGimbalCamera::crc16(const QByteArray& bytes)
{
    quint16 crc = 0;
    for (const char c : bytes) {
        crc ^= static_cast<quint16>(static_cast<quint8>(c)) << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? static_cast<quint16>((crc << 1) ^ 0x1021) : static_cast<quint16>(crc << 1);
        }
    }
    return crc;
}

QByteArray SiyiGimbalCamera::buildFrame(quint8 command, const QByteArray& data, quint16 sequence)
{
    QByteArray frame;
    frame.append(static_cast<char>(kStx1));
    frame.append(static_cast<char>(kStx2));
    frame.append(static_cast<char>(kCtrlAck));
    frame.append(static_cast<char>(data.size() & 0xFF));
    frame.append(static_cast<char>((data.size() >> 8) & 0xFF));
    frame.append(static_cast<char>(sequence & 0xFF));
    frame.append(static_cast<char>(sequence >> 8));
    frame.append(static_cast<char>(command));
    frame.append(data);
    const quint16 crc = crc16(frame);
    frame.append(static_cast<char>(crc & 0xFF));
    frame.append(static_cast<char>(crc >> 8));
    return frame;
}

void SiyiGimbalCamera::_send(quint8 command, const QByteArray& data, const QString& description)
{
    const QByteArray frame = buildFrame(command, data, _sequence++);
    const bool sent = !_cameraAddress.isNull() && _socket->writeDatagram(frame, _cameraAddress, kControlPort) == frame.size();
    if (!description.isEmpty()) {
        _logCommand(QStringLiteral("%1 %2  %3").arg(sent ? QStringLiteral("TX") : tr("TX FAILED"),
                                                     QString::fromLatin1(frame.toHex(' ')), description));
    }
}

// ---------------------------------------------------------------------------- Camera

void SiyiGimbalCamera::takePhoto()
{
    if (!sdCardProblem().isEmpty()) {
        emit notice(tr("Photo not possible: %1").arg(sdCardProblem()));
        return;
    }
    _send(kCmdPhotoVideo, QByteArray(1, 0), tr("Take photo"));
}

void SiyiGimbalCamera::startRecording()
{
    _toggleRecording(true);
}

void SiyiGimbalCamera::stopRecording()
{
    _toggleRecording(false);
}

void SiyiGimbalCamera::_toggleRecording(bool start)
{
    if (start && !sdCardProblem().isEmpty()) {
        emit notice(tr("Recording not possible: %1").arg(sdCardProblem()));
        return;
    }
    _wantRecording = start;
    // One command starts and stops, so it is only sent when the camera is in the other state
    if (_cameraRecording != (start ? 1 : 0)) {
        _send(kCmdPhotoVideo, QByteArray(1, 2), start ? tr("Start recording") : tr("Stop recording"));
    }
    _recordCheckTimer.start();
}

void SiyiGimbalCamera::_checkRecording()
{
    _send(kCmdConfig, QByteArray());
    // The answer arrives in _handleFrame; give it a moment, then tell the operator if nothing changed
    QTimer::singleShot(500, this, [this]() {
        if (_cameraRecording >= 0 && (_cameraRecording == 1) != _wantRecording) {
            emit notice(_wantRecording ? tr("Recording did not start") : tr("Recording did not stop"));
        }
    });
}

// ---------------------------------------------------------------------------- Lens

void SiyiGimbalCamera::_sendZoom(int direction)
{
    _zoomDirection = direction;
    const QByteArray data(1, static_cast<char>(static_cast<qint8>(direction)));
    _send(kCmdManualZoom, data, direction > 0 ? tr("Zoom in") : direction < 0 ? tr("Zoom out") : tr("Zoom stop"));
    if (direction == 0) {
        // A lost stop would leave the lens zooming, so it goes twice
        _send(kCmdManualZoom, data);
    }
}

// ---------------------------------------------------------------------------- Gimbal

void SiyiGimbalCamera::gimbalSpeed(int yawPercent, int pitchPercent)
{
    const bool wasMoving = _yawSpeed != 0 || _pitchSpeed != 0;
    const float scale = kJoystickFullDegPerSec / kDegPerSecAtFull;
    _yawSpeed   = static_cast<qint8>(qBound(-100, qRound(qBound(-100, yawPercent, 100) * scale), 100));
    _pitchSpeed = static_cast<qint8>(qBound(-100, qRound(qBound(-100, pitchPercent, 100) * scale), 100));
    if (_yawSpeed == 0 && _pitchSpeed == 0) {
        gimbalStop();
        return;
    }
    _sendRates(!wasMoving);
    if (!_rateTimer.isActive()) {
        _rateTimer.start();
    }
}

void SiyiGimbalCamera::_sendRates(bool log)
{
    QByteArray data;
    data.append(static_cast<char>(_yawSpeed));
    data.append(static_cast<char>(_pitchSpeed));
    _send(kCmdRotate, data, log ? tr("Gimbal speed yaw %1 pitch %2").arg(_yawSpeed).arg(_pitchSpeed) : QString());
}

void SiyiGimbalCamera::_resendRates()
{
    _sendRates(false);
}

void SiyiGimbalCamera::gimbalStop()
{
    _rateTimer.stop();
    _yawSpeed = _pitchSpeed = 0;
    // A lost stop would leave the gimbal turning, so it goes twice
    _sendRates(true);
    _sendRates(false);
}

void SiyiGimbalCamera::gimbalCenter()
{
    _rateTimer.stop();
    _yawSpeed = _pitchSpeed = 0;
    _send(kCmdCenter, QByteArray(1, 1), tr("Gimbal centre"));
}

void SiyiGimbalCamera::gimbalDown()
{
    _rateTimer.stop();
    _yawSpeed = _pitchSpeed = 0;
    _send(kCmdSetAngles, int16Bytes(0) + int16Bytes(-900), tr("Gimbal down 90"));
}

// ---------------------------------------------------------------------------- Status

void SiyiGimbalCamera::_pollStatus()
{
    _send(kCmdConfig, QByteArray());
    _setConnected(_lastReceive.isValid() && _lastReceive.elapsed() < kConnectionTimeoutMs);
}

void SiyiGimbalCamera::_readBytes()
{
    while (_socket->hasPendingDatagrams()) {
        QByteArray datagram(static_cast<int>(_socket->pendingDatagramSize()), 0);
        _socket->readDatagram(datagram.data(), datagram.size());
        _rxBuffer.append(datagram);
    }

    while (_rxBuffer.size() >= kHeaderSize + kCrcSize) {
        const int start = _rxBuffer.indexOf(QByteArray("\x55\x66", 2));
        if (start < 0) {
            _rxBuffer.clear();
            return;
        }
        _rxBuffer.remove(0, start);
        if (_rxBuffer.size() < kHeaderSize + kCrcSize) {
            return;
        }
        const int dataSize = static_cast<quint8>(_rxBuffer.at(3)) | (static_cast<quint8>(_rxBuffer.at(4)) << 8);
        const int frameSize = kHeaderSize + dataSize + kCrcSize;
        if (_rxBuffer.size() < frameSize) {
            return;
        }
        const QByteArray frame = _rxBuffer.left(frameSize);
        const quint16 crc = static_cast<quint8>(frame.at(frameSize - 2)) | (static_cast<quint8>(frame.at(frameSize - 1)) << 8);
        if (crc16(frame.left(frameSize - kCrcSize)) != crc) {
            _rxBuffer.remove(0, 2);     // not a real frame start: look for the next one
            continue;
        }
        _rxBuffer.remove(0, frameSize);
        _lastReceive.restart();
        _setConnected(true);
        _handleFrame(static_cast<quint8>(frame.at(7)), frame.mid(kHeaderSize, dataSize));
    }
}

void SiyiGimbalCamera::_handleFrame(quint8 command, const QByteArray& data)
{
    switch (command) {
    case kCmdConfig:
        if (data.size() >= 4) {
            const int recordState = static_cast<quint8>(data.at(3));
            _setSdCardStatus(recordState == 2 || recordState == 3 ? QStringLiteral("none") : QStringLiteral("ok"));
            _cameraRecording = recordState == 1 ? 1 : 0;
            _setRecording(_cameraRecording == 1);
        }
        break;
    case kCmdFeedback:
        if (!data.isEmpty()) {
            switch (static_cast<quint8>(data.at(0))) {
            case 0:     emit notice(tr("Photo taken"));         break;
            case 1:     emit notice(tr("Photo failed"));        break;
            case 4:     emit notice(tr("Recording failed"));    break;
            default:    break;
            }
        }
        break;
    case kCmdManualZoom:
        // Only shown when the lens stopped, so holding the button does not flood the screen
        if (data.size() >= 2 && _zoomDirection == 0) {
            const int zoomTenths = static_cast<quint8>(data.at(0)) | (static_cast<quint8>(data.at(1)) << 8);
            if (zoomTenths > 0 && zoomTenths != _zoomShown) {
                _zoomShown = zoomTenths;
                emit notice(tr("Zoom %1x").arg(zoomTenths / 10.0, 0, 'f', 1));
            }
        }
        break;
    default:
        break;
    }
}

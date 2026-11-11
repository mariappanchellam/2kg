/****************************************************************************
 *
 * DhakshaGroundControl: SIYI A8 mini gimbal camera on the RC's Ethernet.
 *
 * Controlled directly with SIYI's own gimbal camera protocol, as the SIYI FPV
 * app does, over UDP to the camera (default 192.168.144.25:37260); nothing
 * goes through the flight controller. Frames:
 *
 *   55 66 | ctrl | data length (2, LE) | sequence (2, LE) | command | data | CRC16 (2, LE)
 *
 * CRC16 is CRC-16/XMODEM (polynomial 0x1021, initial 0) over everything before it,
 * e.g. "55 66 01 01 00 00 00 0C 00 34 CE" takes a picture.
 *
 ****************************************************************************/

#pragma once

#include "DhakshaGimbalCamera.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>

class QUdpSocket;

class SiyiGimbalCamera : public DhakshaGimbalCamera
{
    Q_OBJECT

public:
    SiyiGimbalCamera(const QString& cameraIp, const QString& rtspUrl, QObject* parent = nullptr);

    static constexpr quint16 kControlPort = 37260;

    bool controlSupported() const override { return true; }

    void takePhoto      () override;
    void startRecording () override;
    void stopRecording  () override;

    void zoomIn         () override { _sendZoom(1); }
    void zoomOut        () override { _sendZoom(-1); }
    void zoomStop       () override { _sendZoom(0); }

    void gimbalSpeed    (int yawPercent, int pitchPercent) override;
    void gimbalStop     () override;
    void gimbalCenter   () override;
    void gimbalDown     () override;

    /// One complete frame including its CRC (exposed for tests).
    static QByteArray buildFrame(quint8 command, const QByteArray& data, quint16 sequence = 0);
    static quint16 crc16(const QByteArray& bytes);

private slots:
    void _readBytes         ();
    void _pollStatus        ();
    void _resendRates       ();
    void _checkRecording    ();

private:
    void _send          (quint8 command, const QByteArray& data, const QString& description = QString());
    void _sendRates     (bool log);
    void _sendZoom      (int direction);
    void _toggleRecording(bool start);
    void _handleFrame   (quint8 command, const QByteArray& data);

    // SIYI gimbal camera commands
    static constexpr quint8 kCmdManualZoom      = 0x05;     ///< int8: 1 in, 0 stop, -1 out; reply uint16 zoom x10
    static constexpr quint8 kCmdRotate          = 0x07;     ///< int8 yaw, int8 pitch speeds -100..100 (positive right / up)
    static constexpr quint8 kCmdCenter          = 0x08;     ///< uint8 1
    static constexpr quint8 kCmdConfig          = 0x0A;     ///< reply: [3] record state 0 off, 1 on, 2 no TF card, 3 TF card error
    static constexpr quint8 kCmdFeedback        = 0x0B;     ///< from the camera: 0 photo taken, 1 photo failed, 4 recording failed
    static constexpr quint8 kCmdPhotoVideo      = 0x0C;     ///< uint8: 0 take picture, 2 start / stop recording
    static constexpr quint8 kCmdSetAngles       = 0x0E;     ///< int16 yaw, int16 pitch in 0.1 degrees

    /// The rotate command's 100 is about 90 degrees per second (ArduPilot's SIYI driver uses the same scale)
    static constexpr float  kDegPerSecAtFull    = 90.0f;
    /// The joystick's 100 percent, after the operator's gimbal speed setting
    static constexpr float  kJoystickFullDegPerSec = 60.0f;

    static constexpr int    kPollIntervalMs     = 1000;
    static constexpr int    kRateResendMs       = 200;
    static constexpr int    kRecordCheckMs      = 1500;
    static constexpr int    kConnectionTimeoutMs = 4000;

    QUdpSocket*     _socket = nullptr;
    QHostAddress    _cameraAddress;
    QTimer          _pollTimer;
    QTimer          _rateTimer;
    QTimer          _recordCheckTimer;
    QElapsedTimer   _lastReceive;
    QByteArray      _rxBuffer;
    quint16         _sequence       = 0;
    qint8           _yawSpeed       = 0;
    qint8           _pitchSpeed     = 0;
    int             _zoomDirection  = 0;        ///< last zoom command: 1 in, -1 out, 0 stopped
    int             _zoomShown      = 0;        ///< zoom x10 last shown to the operator
    int             _cameraRecording = -1;      ///< from the camera's own state, -1 not known yet
    bool            _wantRecording  = false;    ///< what the operator last asked for
};

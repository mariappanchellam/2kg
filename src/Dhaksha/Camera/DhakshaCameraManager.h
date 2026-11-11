/****************************************************************************
 *
 * DhakshaGroundControl: owns the single active gimbal camera.
 *
 * This build flies one camera, the SIYI A8 mini (a day camera on a 3-axis
 * gimbal). Its IP and RTSP URL are stored when applied and, on every start-up,
 * the manager creates the camera and points QGC's video stream at its RTSP URL
 * so video starts automatically. Sending the video to a server is done by the
 * DhakshaKey (Termux) app, not here.
 *
 ****************************************************************************/

#pragma once

#include "DhakshaGimbalCamera.h"

#include <QObject>
#include <QStringList>

class QQmlEngine;
class QJSEngine;

class DhakshaCameraManager : public QObject
{
    Q_OBJECT

public:
    enum Vendor {
        VendorNone = 0,
        VendorA8Mini,           ///< SIYI A8 mini on the RC's Ethernet, SIYI's own protocol
        VendorCount
    };
    Q_ENUM(Vendor)

    /// How the RTSP video is received
    enum VideoMode {
        VideoLowDelay = 0,      ///< UDP, 17 ms buffer (QGC's own setting)
        VideoSmooth,            ///< UDP, 150 ms buffer that absorbs radio jitter; older video is dropped
        VideoReliable,          ///< TCP: nothing lost, a little more delay when the link is poor
    };
    Q_ENUM(VideoMode)

    /// How the fast player receives the camera's video
    enum NativeTransport {
        NativeTransportTcp = 0, ///< Interleaved on the RTSP connection: nothing lost, the camera makers' default
        NativeTransportUdp,
    };
    Q_ENUM(NativeTransport)

    Q_PROPERTY(QStringList          vendorNames READ vendorNames                    CONSTANT)
    Q_PROPERTY(int                  vendor      READ vendor     WRITE setVendor     NOTIFY vendorChanged)
    Q_PROPERTY(QString              cameraIp    READ cameraIp   WRITE setCameraIp   NOTIFY cameraIpChanged)
    Q_PROPERTY(QString              rtspUrl     READ rtspUrl    WRITE setRtspUrl    NOTIFY rtspUrlChanged)
    Q_PROPERTY(DhakshaGimbalCamera* camera      READ camera                         NOTIFY cameraChanged)
    Q_PROPERTY(int  gimbalMaxSpeed  READ gimbalMaxSpeed WRITE setGimbalMaxSpeed NOTIFY gimbalMaxSpeedChanged)  ///< Degrees per second at full joystick, 10..60
    Q_PROPERTY(int  videoMode       READ videoMode      WRITE setVideoMode      NOTIFY videoModeChanged)       ///< VideoMode, saved per camera; used from the next apply()
    Q_PROPERTY(bool nativeVideoSupported READ nativeVideoSupported CONSTANT)                                    ///< The fast player works on this platform (Android)
    Q_PROPERTY(bool nativeVideo     READ nativeVideo    WRITE setNativeVideo    NOTIFY nativeVideoChanged)     ///< Fast player chosen in the settings, saved
    Q_PROPERTY(bool nativeVideoActive READ nativeVideoActive                    NOTIFY nativeVideoChanged)     ///< The Fly View plays the main camera with the fast player
    Q_PROPERTY(QString mainVideoUrl READ mainVideoUrl                           NOTIFY mainVideoUrlChanged)    ///< Camera stream on the main screen
    Q_PROPERTY(int  nativeTransport READ nativeTransport WRITE setNativeTransport NOTIFY nativeTransportChanged) ///< NativeTransport, saved; the fast player reconnects when it changes
    Q_PROPERTY(bool nativeTcp       READ nativeTcp                              NOTIFY nativeTransportChanged)
    Q_PROPERTY(QString nativeVideoStatus READ nativeVideoStatus WRITE setNativeVideoStatus NOTIFY nativeVideoStatusChanged) ///< What the fast player receives, set by the Fly View
    Q_PROPERTY(QString flightLogPath     READ flightLogPath     CONSTANT)   ///< This run's flight log file (opened before the UI)

    static DhakshaCameraManager* instance();
    static QObject* qmlSingletonFactory(QQmlEngine*, QJSEngine*);

    QStringList             vendorNames () const;
    int                     vendor      () const { return _vendor; }
    QString                 cameraIp    () const { return _cameraIp; }
    QString                 rtspUrl     () const { return _rtspUrl; }
    int                     videoMode() const { return _videoMode; }
    void                    setVideoMode(int mode);
    static int              defaultVideoMode(int vendor);
    int                     gimbalMaxSpeed() const;
    void                    setGimbalMaxSpeed(int degreesPerSecond);
    static constexpr int    kMinGimbalSpeed = 10;
    static constexpr int    kMaxGimbalSpeed = 60;
    DhakshaGimbalCamera*    camera      () const { return _camera; }
    static bool             nativeVideoSupported();
    bool                    nativeVideo () const { return _nativeVideo; }
    void                    setNativeVideo(bool native);
    bool                    nativeVideoActive() const { return _nativeVideoActive; }
    QString                 mainVideoUrl() const { return _mainVideoUrl; }
    int                     nativeTransport() const { return _nativeTransport; }
    void                    setNativeTransport(int transport);
    bool                    nativeTcp() const { return _nativeTransport == NativeTransportTcp; }
    QString                 nativeVideoStatus() const { return _nativeVideoStatus; }
    static QString          flightLogPath();
    void                    setNativeVideoStatus(const QString& status);

    /// Changing the vendor loads that vendor's saved IP / RTSP URL; call apply() to use them.
    void setVendor      (int vendor);
    void setCameraIp    (const QString& cameraIp);
    void setRtspUrl     (const QString& rtspUrl);

    /// Saves the settings, (re)creates the camera instance, and starts its RTSP video stream.
    Q_INVOKABLE void apply();

    /// Start-up: same as apply() with the stored settings, without saving them.
    void start();

    /// Restores the selected vendor's default IP and RTSP URL (not saved until apply()).
    Q_INVOKABLE void restoreVendorDefaults();

    static QString defaultCameraIp  (int vendor);
    static QString defaultRtspUrl   (int vendor);

signals:
    void vendorChanged  ();
    void cameraIpChanged();
    void rtspUrlChanged ();
    void gimbalMaxSpeedChanged();
    void videoModeChanged();
    void cameraChanged  ();
    void nativeVideoChanged();
    void mainVideoUrlChanged();
    void nativeTransportChanged();
    void nativeVideoStatusChanged();
    /// apply(): the fast player reconnects to the camera
    void videoRestartRequested();

private:
    explicit DhakshaCameraManager(QObject* parent = nullptr);

    void _loadVendorSettings    ();
    void _saveSettings          ();
    void _createCamera          ();
    void _startVideoStream      ();
    void _showVideo             (const QString& rtspUrl);
    void _applyNativeVideo      ();
    void _applyVideoMode        ();

    int                     _vendor     = VendorNone;
    QString                 _cameraIp;
    QString                 _rtspUrl;
    int                     _videoMode  = VideoSmooth;
    DhakshaGimbalCamera*    _camera     = nullptr;
    bool                    _nativeVideo = true;
    bool                    _nativeVideoActive = false;
    QString                 _mainVideoUrl;
    int                     _nativeTransport = NativeTransportTcp;
    QString                 _nativeVideoStatus;
};

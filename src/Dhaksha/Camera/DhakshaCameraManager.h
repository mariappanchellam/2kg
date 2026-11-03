/****************************************************************************
 *
 * DhakshaGroundControl: owns the single active gimbal camera.
 *
 * The operator picks the camera (Skydroid or SIYI ZR10) plus its IP and
 * RTSP URL once; the choice is stored when applied and, on every start-up,
 * the manager creates the matching DhakshaGimbalCamera and points QGC's video
 * stream at the camera's RTSP URL so video starts automatically. Until a
 * camera has been applied the vendor is None and QGC's own video settings are
 * left untouched. Sending the video to a server is done by the DhakshaKey
 * (Termux) app, not here.
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
        VendorSkydroid,
        VendorZr10,             ///< SIYI ZR10 on the RC's Ethernet, SIYI's own protocol
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
    Q_PROPERTY(QString              nightRtspUrl READ nightRtspUrl WRITE setNightRtspUrl NOTIFY nightRtspUrlChanged)   ///< Thermal camera; empty when there is none
    Q_PROPERTY(bool                 nightView   READ nightView                      NOTIFY nightViewChanged)    ///< Night video on screen
    Q_PROPERTY(bool     pipAvailable    READ pipAvailable                       NOTIFY pipAvailableChanged)     ///< The camera has a second (night / day) stream for the PiP box
    Q_PROPERTY(bool     pipCamera       READ pipCamera      WRITE setPipCamera  NOTIFY pipCameraChanged)        ///< PiP box shows the other camera (true) or the map (false)
    Q_PROPERTY(int  skydroidGimbalStyle READ skydroidGimbalStyle WRITE setSkydroidGimbalStyle NOTIFY skydroidGimbalStyleChanged) ///< 0 YPR (C13), 1 GSY/GSP (C10); used from the next apply()
    Q_PROPERTY(DhakshaGimbalCamera* camera      READ camera                         NOTIFY cameraChanged)
    Q_PROPERTY(int  gimbalMaxSpeed  READ gimbalMaxSpeed WRITE setGimbalMaxSpeed NOTIFY gimbalMaxSpeedChanged)  ///< Degrees per second at full joystick, 10..60
    Q_PROPERTY(QStringList thermalPaletteNames READ thermalPaletteNames CONSTANT)
    Q_PROPERTY(int  thermalPalette  READ thermalPalette WRITE setThermalPalette NOTIFY thermalPaletteChanged)  ///< Night camera palette, saved; sent when chosen and when the camera connects
    Q_PROPERTY(int  videoMode       READ videoMode      WRITE setVideoMode      NOTIFY videoModeChanged)       ///< VideoMode, saved per camera; used from the next apply()
    Q_PROPERTY(bool nativeVideoSupported READ nativeVideoSupported CONSTANT)                                    ///< The fast player works on this platform (Android)
    Q_PROPERTY(bool nativeVideo     READ nativeVideo    WRITE setNativeVideo    NOTIFY nativeVideoChanged)     ///< Fast player chosen in the settings, saved
    Q_PROPERTY(bool nativeVideoActive READ nativeVideoActive                    NOTIFY nativeVideoChanged)     ///< The Fly View plays the main camera with the fast player
    Q_PROPERTY(QString mainVideoUrl READ mainVideoUrl                           NOTIFY mainVideoUrlChanged)    ///< Camera stream on the main screen (day or night)
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
    QString                 nightRtspUrl() const { return _nightRtspUrl; }
    bool                    nightView   () const { return _nightView; }
    bool                    pipAvailable() const;
    bool                    pipCamera   () const { return _pipCamera; }
    void                    setPipCamera(bool camera);
    int                     skydroidGimbalStyle() const;
    void                    setSkydroidGimbalStyle(int style);
    int                     videoMode() const { return _videoMode; }
    void                    setVideoMode(int mode);
    static int              defaultVideoMode(int vendor);
    int                     gimbalMaxSpeed() const;
    void                    setGimbalMaxSpeed(int degreesPerSecond);
    static QStringList      thermalPaletteNames();
    int                     thermalPalette() const;
    void                    setThermalPalette(int palette);
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
    void setNightRtspUrl(const QString& rtspUrl);

    /// Switches the video on screen between the day and the night (thermal) stream; apply() goes back to day.
    Q_INVOKABLE void toggleNightView();

    /// Saves the settings, (re)creates the camera instance, and starts its RTSP video stream.
    Q_INVOKABLE void apply();

    /// Start-up: same as apply() with the stored settings, without saving them.
    void start();

    /// Restores the selected vendor's default IP and RTSP URL (not saved until apply()).
    Q_INVOKABLE void restoreVendorDefaults();

    static QString defaultCameraIp  (int vendor);
    static QString defaultRtspUrl   (int vendor);
    static QString defaultNightRtspUrl(int vendor);

signals:
    void vendorChanged  ();
    void cameraIpChanged();
    void rtspUrlChanged ();
    void nightRtspUrlChanged();
    void nightViewChanged();
    void pipAvailableChanged();
    void pipCameraChanged();
    void skydroidGimbalStyleChanged();
    void gimbalMaxSpeedChanged();
    void thermalPaletteChanged();
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
    void _updatePipVideo        ();

    int                     _vendor     = VendorNone;
    QString                 _cameraIp;
    QString                 _rtspUrl;
    QString                 _nightRtspUrl;
    int                     _videoMode  = VideoLowDelay;
    bool                    _nightView  = false;
    bool                    _pipCamera  = true;
    DhakshaGimbalCamera*    _camera     = nullptr;
    bool                    _nativeVideo = true;
    bool                    _nativeVideoActive = false;
    QString                 _mainVideoUrl;
    int                     _nativeTransport = NativeTransportTcp;
    QString                 _nativeVideoStatus;
};

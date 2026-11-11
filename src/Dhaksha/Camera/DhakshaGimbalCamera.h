/****************************************************************************
 *
 * DhakshaGroundControl: vendor-neutral gimbal camera interface.
 *
 * One interface for the gimbal camera (this build: the SIYI A8 mini).
 * The QML camera controls only talk to this class; each vendor subclass
 * translates the calls into its own control protocol. Only one camera is
 * connected to the drone at a time, so DhakshaCameraManager owns a single
 * instance.
 *
 ****************************************************************************/

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class DhakshaGimbalCamera : public QObject
{
    Q_OBJECT

public:
    explicit DhakshaGimbalCamera(const QString& vendorName, const QString& rtspUrl, QObject* parent = nullptr);

    Q_PROPERTY(QString  vendorName          READ vendorName         CONSTANT)
    Q_PROPERTY(QString  rtspUrl             READ rtspUrl            CONSTANT)
    Q_PROPERTY(bool     controlSupported    READ controlSupported   CONSTANT)   ///< false: video stream only
    Q_PROPERTY(bool     connected           READ connected          NOTIFY connectedChanged)
    Q_PROPERTY(bool     recording           READ recording          NOTIFY recordingChanged)
    Q_PROPERTY(QStringList commandLog       READ commandLog         NOTIFY commandLogChanged)  ///< Newest first; lets the operator verify commands without the camera
    Q_PROPERTY(bool     rangefinderSupported READ rangefinderSupported CONSTANT)   ///< Laser rangefinder (e.g. Skydroid C13)
    Q_PROPERTY(QString  distanceText        READ distanceText       NOTIFY distanceTextChanged) ///< Last laser range, e.g. "123.4 m"; empty before the first
    Q_PROPERTY(bool     rangingActive       READ rangingActive      NOTIFY rangingActiveChanged)    ///< Continuous laser ranging on
    Q_PROPERTY(QString  sdCardStatus        READ sdCardStatus       NOTIFY sdCardStatusChanged)     ///< "" unknown, "ok", "none" (missing or error), "full"
    Q_PROPERTY(qreal    screenZoom          READ screenZoom         NOTIFY screenZoomChanged)       ///< Extra magnification of the video on screen beyond the camera's own zoom

    QString vendorName  () const { return _vendorName; }
    QString rtspUrl     () const { return _rtspUrl; }
    bool    connected   () const { return _connected; }
    bool    recording   () const { return _recording; }
    QStringList commandLog() const { return _commandLog; }
    QString distanceText() const { return _distanceText; }
    bool    rangingActive() const { return _rangingActive; }
    QString sdCardStatus() const { return _sdCardStatus; }
    qreal   screenZoom  () const { return _screenZoom; }
    /// Why photos and recordings cannot be saved, or empty when the card is fine or unknown
    QString sdCardProblem() const;

    virtual bool controlSupported() const { return false; }
    virtual bool rangefinderSupported() const { return false; }

    Q_INVOKABLE void clearCommandLog();

    // Camera
    Q_INVOKABLE virtual void takePhoto          () {}
    Q_INVOKABLE virtual void startRecording     () {}
    Q_INVOKABLE virtual void stopRecording      () {}
    Q_INVOKABLE void         toggleRecording    () { _recording ? stopRecording() : startRecording(); }
    /// One laser range measurement; the answer arrives in distanceText
    Q_INVOKABLE virtual void measureDistance    () {}
    /// Continuous laser ranging on / off; the distance keeps updating in distanceText
    Q_INVOKABLE virtual void toggleRanging      () {}

    // Lens
    Q_INVOKABLE virtual void zoomIn             () {}
    Q_INVOKABLE virtual void zoomOut            () {}
    Q_INVOKABLE virtual void zoomStop           () {}
    Q_INVOKABLE virtual void focusIn            () {}
    Q_INVOKABLE virtual void focusOut           () {}
    Q_INVOKABLE virtual void focusStop          () {}
    Q_INVOKABLE virtual void autoFocus          () {}

    // Gimbal
    /// Rotate at the given speeds, each -100..100 (percent of maximum). Positive yaw = right, positive pitch = up.
    Q_INVOKABLE virtual void gimbalSpeed        (int /*yawPercent*/, int /*pitchPercent*/) {}
    Q_INVOKABLE virtual void gimbalStop         () {}
    Q_INVOKABLE virtual void gimbalCenter       () {}
    Q_INVOKABLE virtual void gimbalLock         () {}   ///< Hold earth-frame heading
    Q_INVOKABLE virtual void gimbalFollow       () {}   ///< Follow vehicle heading
    Q_INVOKABLE virtual void gimbalDown         () {}   ///< Point straight down (tilt -90)

signals:
    void connectedChanged   ();
    void recordingChanged   ();
    void commandLogChanged  ();
    void distanceTextChanged();
    void rangingActiveChanged();
    void sdCardStatusChanged();
    void screenZoomChanged  ();
    /// A short message for the operator, e.g. that recording did not start
    void notice(const QString& text);

protected:
    void _setConnected(bool connected);
    void _setRecording(bool recording);
    void _logCommand(const QString& entry);
    void _setDistanceText(const QString& text);
    void _setRangingActive(bool active);
    void _setSdCardStatus(const QString& status);
    void _setScreenZoom(qreal zoom);

private:
    QString _vendorName;
    QString _rtspUrl;
    bool    _connected = false;
    bool    _recording = false;
    QStringList _commandLog;
    QString _distanceText;
    bool    _rangingActive = false;
    QString _sdCardStatus;
    qreal   _screenZoom = 1.0;

    static constexpr int kMaxLogEntries = 50;
};

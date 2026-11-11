/****************************************************************************
 *
 * DhakshaGroundControl: gimbal camera controls on the Fly View.
 *
 * Laid out like the camera makers' own apps, for a 5.5 inch screen: round
 * icon buttons split around the video instead of one panel.
 *   left:        record and photo, with the gimbal joystick to their right
 *                (drag the knob; the further from the centre, the faster the
 *                gimbal turns, up to the speed set under settings)
 *   right:       centre, tilt down 90, zoom in / out and settings, just under
 *                the HUD
 *   top centre:  connection
 * A small label under the top row shows recording time and other
 * confirmations. Works with any DhakshaGimbalCamera; the vendor protocol
 * lives in the C++ camera classes.
 *
 ****************************************************************************/

import QtQuick          2.12
import QtQuick.Controls 2.4
import QtQuick.Layouts  1.12

import QGroundControl.Controls      1.0
import QGroundControl.DhakshaCamera 1.0
import QGroundControl.ScreenTools   1.0

Item {
    id:         root
    visible:    _camera && _camera.controlSupported

    // Space taken by other Fly View widgets, set by the parent layer
    property real leftInset:        0   ///< right edge of the tool strip
    property real topInset:         0
    property real rightTopInset:    0   ///< bottom of the instrument panel at the top right

    property var    _camera:        DhakshaCameraManager.camera
    property real   _margin:        ScreenTools.defaultFontPixelHeight / 2
    property real   _buttonSize:    ScreenTools.defaultFontPixelHeight * 1.9
    property real   _smallButton:   ScreenTools.defaultFontPixelHeight * 1.6
    property real   _joystickSize:  ScreenTools.defaultFontPixelHeight * 5.5
    property int    _recordSeconds: 0

    readonly property string _iconPath: "/res/DhakshaCamera/"
    readonly property color  _orange:   "#F26522"
    readonly property color  _red:      "#D0B71C1C"

    // ------------------------------------------------------------------ Building blocks

    // Round icon button; holdAction runs while pressed, releaseAction when let go
    component IconButton: Rectangle {
        id:             button
        width:          _buttonSize
        height:         width
        radius:         width / 2
        color:          active ? activeColor : (touch.down ? "#C0404040" : "#A0202020")
        border.color:   "#60FFFFFF"
        border.width:   1

        property string icon
        property bool   active:         false
        property color  activeColor:    _orange
        property var    clickAction:    null
        property var    holdAction:     null
        property var    releaseAction:  null

        QGCColoredImage {
            anchors.centerIn:   parent
            width:              parent.width * 0.5
            height:             width
            source:             _iconPath + button.icon + ".svg"
            sourceSize.height:  height
            fillMode:           Image.PreserveAspectFit
            color:              "white"
        }

        // Touch handling as in QGC's own on-screen joysticks: on Android a MouseArea here lost the
        // touch after a few milliseconds, so held buttons stopped at once
        MultiPointTouchArea {
            id:                 touch
            anchors.fill:       parent
            maximumTouchPoints: 1

            property bool down: false

            onPressed: {
                down = true
                if (button.holdAction) button.holdAction()
            }
            onReleased: {
                down = false
                if (button.releaseAction) button.releaseAction()
                var point = touchPoints[0]
                if (button.clickAction && point && point.x >= 0 && point.y >= 0 && point.x <= width && point.y <= height) {
                    button.clickAction()
                }
            }
            onCanceled: {
                down = false
                if (button.releaseAction) button.releaseAction()
            }
        }
    }

    function notify(text) {
        toastText.text = text
        toast.opacity = 1
        toastTimer.restart()
    }

    // ------------------------------------------------------------------ Status label timers

    Timer {
        id:             toastTimer
        interval:       2000
        onTriggered:    toast.opacity = 0
    }

    Timer {
        interval:       1000
        repeat:         true
        running:        _camera && _camera.recording
        onTriggered:    _recordSeconds++
    }

    Connections {
        target: _camera
        function onRecordingChanged() {
            _recordSeconds = 0
            notify(_camera.recording ? qsTr("Recording") : qsTr("Recording saved"))
        }
        function onNotice(text) {
            notify(text)
        }
    }

    // ------------------------------------------------------------------ Top centre

    Column {
        anchors.top:                parent.top
        anchors.topMargin:          topInset + _margin
        anchors.horizontalCenter:   parent.horizontalCenter
        spacing:                    _margin / 2

        Row {
            anchors.horizontalCenter:   parent.horizontalCenter
            spacing:                    _margin

            // Connection: green when the camera answers
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width:                  ScreenTools.defaultFontPixelHeight * 0.7
                height:                 width
                radius:                 width / 2
                color:                  _camera && _camera.connected ? "lime" : "red"
            }
        }

        // Recording time stays; other confirmations fade after 2 s
        Row {
            anchors.horizontalCenter:   parent.horizontalCenter
            spacing:                    _margin

            Rectangle {
                visible:        _camera && _camera.recording
                width:          recordText.width + height
                height:         recordText.height + _margin
                radius:         height / 2
                color:          _red

                QGCLabel {
                    id:                 recordText
                    anchors.centerIn:   parent
                    color:              "white"
                    font.bold:          true
                    text:               "● REC " + Math.floor(_recordSeconds / 60) + ":" + ("0" + (_recordSeconds % 60)).slice(-2)
                }
            }
            Rectangle {
                visible:        _camera && (_camera.sdCardStatus === "none" || _camera.sdCardStatus === "full")
                width:          sdText.width + height
                height:         sdText.height + _margin
                radius:         height / 2
                color:          "#C0B26A00"

                QGCLabel {
                    id:                 sdText
                    anchors.centerIn:   parent
                    color:              "white"
                    text:               _camera && _camera.sdCardStatus === "full" ? qsTr("SD card full") : qsTr("No SD card")
                }
            }
            Rectangle {
                id:             toast
                opacity:        0
                visible:        opacity > 0
                width:          toastText.width + height
                height:         toastText.height + _margin
                radius:         height / 2
                color:          "#C0202020"

                Behavior on opacity { NumberAnimation { duration: 250 } }

                QGCLabel {
                    id:                 toastText
                    anchors.centerIn:   parent
                    color:              "white"
                }
            }
        }
    }

    // ------------------------------------------------------------------ Left: record and photo, joystick to their right

    Column {
        id:                     captureButtons
        anchors.left:           parent.left
        anchors.leftMargin:     leftInset + _margin
        anchors.verticalCenter: joystick.verticalCenter
        spacing:                _margin

        IconButton {
            icon:           "record"
            active:         _camera && _camera.recording
            activeColor:    _red
            clickAction:    function() { _camera.toggleRecording() }
        }
        IconButton {
            icon:           "photo"
            clickAction:    function() { _camera.takePhoto() }
        }
    }

    Item {
        id:                     joystick
        width:                  _joystickSize
        height:                 width
        anchors.left:           captureButtons.right
        anchors.leftMargin:     _margin
        anchors.verticalCenter: parent.verticalCenter
        z:                      1

        property real   _range:     (width - knob.width) / 2
        property real   _dx:        0
        property real   _dy:        0
        property int    _yawSent:   0
        property int    _pitchSent: 0

        // Dead zone in the middle, then speed grows with the square of the distance for fine aiming
        function speed(fraction) {
            var magnitude = Math.max(0, (Math.abs(fraction) - 0.12) / 0.88)
            return Math.round((fraction < 0 ? -1 : 1) * magnitude * magnitude * 100)
        }

        // Full deflection = the gimbal speed set under settings (10..60 deg/s); 60 is the cameras' full speed
        function sendSpeed() {
            var limit = DhakshaCameraManager.gimbalMaxSpeed / 60
            var yaw   = Math.round(speed(_dx / _range) * limit)
            var pitch = Math.round(speed(-_dy / _range) * limit)
            if (yaw !== _yawSent || pitch !== _pitchSent) {
                _yawSent   = yaw
                _pitchSent = pitch
                _camera.gimbalSpeed(yaw, pitch)
            }
        }

        Rectangle {
            anchors.fill:   parent
            radius:         width / 2
            color:          "#40000000"
            border.color:   "white"
            border.width:   2
        }

        Rectangle {
            id:         knob
            width:      joystick.width * 0.18
            height:     width
            radius:     width / 2
            color:      _orange
            x:          (joystick.width - width) / 2 + joystick._dx
            y:          (joystick.height - height) / 2 + joystick._dy

            Behavior on x { enabled: !joystickArea.active; NumberAnimation { duration: 120 } }
            Behavior on y { enabled: !joystickArea.active; NumberAnimation { duration: 120 } }
        }

        // Touch handling as in QGC's own on-screen joysticks (JoystickThumbPad): a MouseArea lost the
        // touch on Android after about 80 ms, which stopped the gimbal right after every start
        MultiPointTouchArea {
            id:                 joystickArea
            anchors.fill:       parent
            minimumTouchPoints: 1
            maximumTouchPoints: 1
            touchPoints:        [ TouchPoint { id: thumb } ]

            property bool active: false

            function move(touchX, touchY) {
                var dx = touchX - width / 2
                var dy = touchY - height / 2
                var distance = Math.sqrt(dx * dx + dy * dy)
                if (distance > joystick._range) {
                    dx *= joystick._range / distance
                    dy *= joystick._range / distance
                }
                joystick._dx = dx
                joystick._dy = dy
                joystick.sendSpeed()
            }

            function release() {
                active = false
                joystick._dx = 0
                joystick._dy = 0
                joystick._yawSent = 0
                joystick._pitchSent = 0
                _camera.gimbalStop()
            }

            onPressed: {
                active = true
                move(thumb.x, thumb.y)
            }
            onUpdated:  if (active) move(thumb.x, thumb.y)
            onReleased: release()
            onCanceled: release()
        }
    }

    // ------------------------------------------------------------------ Right: gimbal and lens, just under the HUD

    Grid {
        anchors.right:          parent.right
        anchors.rightMargin:    _margin
        anchors.top:            parent.top
        anchors.topMargin:      (rightTopInset > 0 ? rightTopInset : topInset) + _margin / 2
        columns:                2
        spacing:                _margin

        IconButton {
            icon:           "center"
            clickAction:    function() { _camera.gimbalCenter(); notify(qsTr("Gimbal centre")) }
        }
        IconButton {
            icon:           "zoom_in"
            holdAction:     function() { _camera.zoomIn() }
            releaseAction:  function() { _camera.zoomStop() }
        }
        IconButton {
            icon:           "down"
            clickAction:    function() { _camera.gimbalDown(); notify(qsTr("Tilt down 90°")) }
        }
        IconButton {
            icon:           "zoom_out"
            holdAction:     function() { _camera.zoomOut() }
            releaseAction:  function() { _camera.zoomStop() }
        }
        IconButton {
            icon:           "settings"
            active:         speedSettings.visible
            clickAction:    function() { speedSettings.open() }
        }
    }

    // ------------------------------------------------------------------ Settings: gimbal speed

    Rectangle {
        id:             speedSettings
        anchors.fill:   parent
        visible:        false
        color:          "#80000000"
        z:              2

        property int _speed: DhakshaCameraManager.gimbalMaxSpeed

        function open() {
            _speed = DhakshaCameraManager.gimbalMaxSpeed
            visible = true
        }

        // Touches outside the panel close it without saving
        MouseArea {
            anchors.fill:   parent
            onClicked:      speedSettings.visible = false
        }

        Rectangle {
            anchors.centerIn:   parent
            width:              speedColumn.width + _margin * 4
            height:             speedColumn.height + _margin * 3
            radius:             _margin
            color:              "#E0202020"
            border.color:       _orange

            MouseArea { anchors.fill: parent }  // keep touches on the panel from closing it

            Column {
                id:                 speedColumn
                anchors.centerIn:   parent
                spacing:            _margin

                QGCLabel {
                    anchors.horizontalCenter:   parent.horizontalCenter
                    text:                       qsTr("Gimbal speed")
                    color:                      "white"
                    font.bold:                  true
                }
                QGCLabel {
                    anchors.horizontalCenter:   parent.horizontalCenter
                    text:                       qsTr("%1 °/s").arg(speedSettings._speed)
                    color:                      _orange
                    font.bold:                  true
                    font.pointSize:             ScreenTools.largeFontPointSize
                }
                Row {
                    spacing: _margin
                    QGCLabel { anchors.verticalCenter: parent.verticalCenter; text: "10"; color: "white" }
                    Slider {
                        width:      ScreenTools.defaultFontPixelWidth * 28
                        from:       10
                        to:         60
                        stepSize:   5
                        snapMode:   Slider.SnapAlways
                        value:      speedSettings._speed
                        onMoved:    speedSettings._speed = Math.round(value)
                    }
                    QGCLabel { anchors.verticalCenter: parent.verticalCenter; text: "60"; color: "white" }
                }
                Row {
                    anchors.horizontalCenter:   parent.horizontalCenter
                    spacing:                    _margin * 2
                    QGCButton {
                        text:       qsTr("Cancel")
                        onClicked:  speedSettings.visible = false
                    }
                    QGCButton {
                        text:       qsTr("Confirm")
                        primary:    true
                        onClicked: {
                            DhakshaCameraManager.gimbalMaxSpeed = speedSettings._speed
                            speedSettings.visible = false
                            notify(qsTr("Gimbal speed %1 °/s saved").arg(speedSettings._speed))
                        }
                    }
                }
            }
        }
    }
}

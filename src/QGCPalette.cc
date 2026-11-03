/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


/// @file
///     @author Don Gagne <don@thegagnes.com>

#include "QGCPalette.h"
#include "QGCApplication.h"
#include "QGCCorePlugin.h"

#include <QApplication>
#include <QPalette>

QList<QGCPalette*>   QGCPalette::_paletteObjects;

QGCPalette::Theme QGCPalette::_theme = QGCPalette::Dark;

QMap<int, QMap<int, QMap<QString, QColor>>> QGCPalette::_colorInfoMap;

QStringList QGCPalette::_colors;

QGCPalette::QGCPalette(QObject* parent) :
    QObject(parent),
    _colorGroupEnabled(true)
{
    if (_colorInfoMap.isEmpty()) {
        _buildMap();
    }

    // We have to keep track of all QGCPalette objects in the system so we can signal theme change to all of them
    _paletteObjects += this;
}

QGCPalette::~QGCPalette()
{
    bool fSuccess = _paletteObjects.removeOne(this);
    if (!fSuccess) {
        qWarning() << "Internal error";
    }
}

void QGCPalette::_buildMap()
{
    //                                      Light                 Dark
    //                                      Disabled   Enabled    Disabled   Enabled
    // Dhaksha skin: navy background, yellow (#dee21b) borders/highlights, white text; same for both themes
    DECLARE_QGC_COLOR(window,               "#0a1d4a", "#0a1d4a", "#0a1d4a", "#0a1d4a")
    DECLARE_QGC_COLOR(windowShadeLight,     "#2e4f8f", "#3a5fa8", "#2e4f8f", "#3a5fa8")
    DECLARE_QGC_COLOR(windowShade,          "#123578", "#123578", "#123578", "#123578")
    DECLARE_QGC_COLOR(windowShadeDark,      "#071538", "#071538", "#071538", "#071538")
    DECLARE_QGC_COLOR(text,                 "#8a96b8", "#ffffff", "#8a96b8", "#ffffff")
    DECLARE_QGC_COLOR(warningText,          "#ff5c66", "#ff5c66", "#ff5c66", "#ff5c66")
    DECLARE_QGC_COLOR(button,               "#142a5c", "#0d2660", "#142a5c", "#0d2660")
    DECLARE_QGC_COLOR(buttonText,           "#8a96b8", "#ffffff", "#8a96b8", "#ffffff")
    DECLARE_QGC_COLOR(buttonBorder,         "#6b6e2a", "#dee21b", "#6b6e2a", "#dee21b")
    DECLARE_QGC_COLOR(buttonHighlight,      "#4a4c1a", "#dee21b", "#4a4c1a", "#dee21b")
    DECLARE_QGC_COLOR(buttonHighlightText,  "#0a1d4a", "#0a1d4a", "#0a1d4a", "#0a1d4a")
    DECLARE_QGC_COLOR(primaryButton,        "#6b6e2a", "#dee21b", "#6b6e2a", "#dee21b")
    DECLARE_QGC_COLOR(primaryButtonText,    "#0a1d4a", "#0a1d4a", "#0a1d4a", "#0a1d4a")
    DECLARE_QGC_COLOR(textField,            "#b8c0d8", "#ffffff", "#b8c0d8", "#ffffff")
    DECLARE_QGC_COLOR(textFieldText,        "#4a5578", "#0a1d4a", "#4a5578", "#0a1d4a")
    DECLARE_QGC_COLOR(mapButton,            "#0a1d4a", "#0a1d4a", "#0a1d4a", "#0a1d4a")
    DECLARE_QGC_COLOR(mapButtonHighlight,   "#585858", "#be781c", "#585858", "#be781c")
    DECLARE_QGC_COLOR(mapIndicator,         "#585858", "#be781c", "#585858", "#be781c")
    DECLARE_QGC_COLOR(mapIndicatorChild,    "#585858", "#766043", "#585858", "#766043")
    DECLARE_QGC_COLOR(colorGreen,           "#009431", "#009431", "#00e04b", "#00e04b")
    DECLARE_QGC_COLOR(colorOrange,          "#b95604", "#b95604", "#de8500", "#de8500")
    DECLARE_QGC_COLOR(colorRed,             "#ed3939", "#ed3939", "#f32836", "#f32836")
    DECLARE_QGC_COLOR(colorGrey,            "#808080", "#808080", "#bfbfbf", "#bfbfbf")
    DECLARE_QGC_COLOR(colorBlue,            "#1a72ff", "#1a72ff", "#536dff", "#536dff")
    DECLARE_QGC_COLOR(alertBackground,      "#eecc44", "#eecc44", "#eecc44", "#eecc44")
    DECLARE_QGC_COLOR(alertBorder,          "#808080", "#808080", "#808080", "#808080")
    DECLARE_QGC_COLOR(alertText,            "#000000", "#000000", "#000000", "#000000")
    DECLARE_QGC_COLOR(missionItemEditor,    "#123578", "#123578", "#123578", "#123578")
    DECLARE_QGC_COLOR(toolStripHoverColor,  "#1d4594", "#1d4594", "#1d4594", "#1d4594")
    DECLARE_QGC_COLOR(statusFailedText,     "#8a96b8", "#ffffff", "#8a96b8", "#ffffff")
    DECLARE_QGC_COLOR(statusPassedText,     "#8a96b8", "#ffffff", "#8a96b8", "#ffffff")
    DECLARE_QGC_COLOR(statusPendingText,    "#8a96b8", "#ffffff", "#8a96b8", "#ffffff")
    DECLARE_QGC_COLOR(toolbarBackground,    "#071538", "#071538", "#071538", "#071538")

    // Colors not affecting by theming
    //                                              Disabled    Enabled
    DECLARE_QGC_NONTHEMED_COLOR(brandingPurple,     "#123578", "#123578")
    DECLARE_QGC_NONTHEMED_COLOR(brandingBlue,       "#48D6FF", "#6045c5")
    DECLARE_QGC_NONTHEMED_COLOR(toolStripFGColor,   "#707070", "#ffffff")

    // Colors not affecting by theming or enable/disable
    DECLARE_QGC_SINGLE_COLOR(mapWidgetBorderLight,          "#ffffff")
    DECLARE_QGC_SINGLE_COLOR(mapWidgetBorderDark,           "#000000")
    DECLARE_QGC_SINGLE_COLOR(mapMissionTrajectory,          "#be781c")
    DECLARE_QGC_SINGLE_COLOR(surveyPolygonInterior,         "green")
    DECLARE_QGC_SINGLE_COLOR(surveyPolygonTerrainCollision, "red")
}

void QGCPalette::setColorGroupEnabled(bool enabled)
{
    _colorGroupEnabled = enabled;
    emit paletteChanged();
}

void QGCPalette::setGlobalTheme(Theme newTheme)
{
    // Mobile build does not have themes
    if (_theme != newTheme) {
        _theme = newTheme;
        _signalPaletteChangeToAll();
    }
}

void QGCPalette::_signalPaletteChangeToAll()
{
    // Notify all objects of the new theme
    foreach (QGCPalette* palette, _paletteObjects) {
        palette->_signalPaletteChanged();
    }
}

void QGCPalette::_signalPaletteChanged()
{
    emit paletteChanged();
}

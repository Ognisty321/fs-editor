// Tinted SVG icons from app/icons (Tabler Icons, MIT).
#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

// `name` is the file name without .svg; an invalid colour draws in the palette text colour.
QIcon icon(const QString &name, const QColor &color = {});

// The program's own icon (app/appicon/fsed.svg, fsed-small.svg under 32 pixels), for the windows'
// title bars and the taskbar.
QIcon appIcon();

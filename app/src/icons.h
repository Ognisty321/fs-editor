// Tinted SVG icons from app/icons (Tabler Icons, MIT).
#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

// `name` is the file name without .svg; an invalid colour draws in the palette text colour.
QIcon icon(const QString &name, const QColor &color = {});

#include "icons.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

QIcon icon(const QString &name, const QColor &color) {
    static QHash<QString, QIcon> cache;
    const QColor c = color.isValid() ? color : QApplication::palette().color(QPalette::WindowText);
    const QString key = name + c.name();
    if (auto it = cache.constFind(key); it != cache.cend()) return *it;
    QFile f(":/icons/" + name + ".svg");
    if (!f.open(QIODevice::ReadOnly)) return {};
    QSvgRenderer svg(f.readAll().replace("currentColor", c.name().toLatin1()));
    QIcon ic;
    // pre-rendered sizes so menus, toolbars and HiDPI screens stay sharp
    for (int s : {16, 20, 24, 32, 40, 48}) {
        QPixmap px(s, s);
        px.fill(Qt::transparent);
        QPainter p(&px);
        svg.render(&p);
        p.end();
        ic.addPixmap(px);
    }
    cache.insert(key, ic);
    return ic;
}

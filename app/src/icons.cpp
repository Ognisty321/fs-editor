#include "icons.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStyle>
#include <QStyleOption>
#include <QSvgRenderer>

namespace {
// Renders the tinted SVG at the size and scale asked for, once each: Qt's own SVG
// engine cannot recolour, and pre-rendering six sizes of every icon slowed startup.
class TintedSvgEngine final : public QIconEngine {
public:
    explicit TintedSvgEngine(QByteArray svg) : m_svg(std::move(svg)) {}
    QIconEngine *clone() const override { return new TintedSvgEngine(*this); }
    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State, qreal scale) override {
        const QSize px = (QSizeF(size) * scale).toSize();
        const quint64 key = quint64(px.width()) << 32 | quint64(px.height()) << 8 | quint64(mode);
        if (auto it = m_cache.constFind(key); it != m_cache.cend()) return *it;
        QPixmap pm(px);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            QSvgRenderer(m_svg).render(&p);
        }
        // the disabled/selected looks QIcon makes for added pixmaps
        if (mode == QIcon::Disabled || mode == QIcon::Selected) {
            QStyleOption opt;
            opt.palette = QApplication::palette();
            pm = QApplication::style()->generatedIconPixmap(mode, pm, &opt);
        }
        pm.setDevicePixelRatio(scale);
        m_cache.insert(key, pm);
        return pm;
    }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        p->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, p->device()->devicePixelRatio()));
    }

private:
    QByteArray m_svg;
    QHash<quint64, QPixmap> m_cache;
};
} // namespace

QIcon icon(const QString &name, const QColor &color) {
    static QHash<QString, QIcon> cache;
    const QColor c = color.isValid() ? color : QApplication::palette().color(QPalette::WindowText);
    const QString key = name + c.name();
    if (auto it = cache.constFind(key); it != cache.cend()) return *it;
    QFile f(":/icons/" + name + ".svg");
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QIcon ic(new TintedSvgEngine(f.readAll().replace("currentColor", c.name().toLatin1())));
    cache.insert(key, ic);
    return ic;
}

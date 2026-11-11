// MainWindow's toolbar and status bar.
#include "mainwindow_p.h"

#include "icons.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QProxyStyle>
#include <QSettings>
#include <QStatusBar>
#include <QStyleOptionToolButton>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>

namespace {
// A toolbar button with a menu: the native style takes the arrow's room from the icon, which
// then sits left of the others; here the icon is drawn centred and the arrow in the corner.
class MenuButtonStyle final : public QProxyStyle {
public:
    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                            const QWidget *w) const override {
        const auto *tb = qstyleoption_cast<const QStyleOptionToolButton *>(opt);
        if (cc != CC_ToolButton || !tb || (tb->features & QStyleOptionToolButton::MenuButtonPopup) ||
            !(tb->features & QStyleOptionToolButton::HasMenu))
            return QProxyStyle::drawComplexControl(cc, opt, p, w);
        QStyleOptionToolButton o = *tb;
        o.features &= ~QStyleOptionToolButton::HasMenu;
        QProxyStyle::drawComplexControl(cc, &o, p, w);
        // QCommonStyle's corner arrow
        const int mbi = pixelMetric(PM_MenuButtonIndicator, tb, w);
        const QRect r = tb->rect;
        o.rect = visualRect(tb->direction, r, QRect(r.right() + 5 - mbi, r.bottom() + 5 - mbi, mbi - 6, mbi - 6));
        drawPrimitive(PE_IndicatorArrowDown, &o, p, w);
    }
};
} // namespace

void MainWindow::buildToolbar() {
    if (m_toolbar) {
        removeToolBar(m_toolbar);
        m_toolbar->deleteLater();
    }
    m_toolbar = new QToolBar(tr("Toolbar"), this);
    m_toolbar->setObjectName("toolbar");
    m_toolbar->setMovable(false);
    m_toolbar->setIconSize(QSize(22, 22) * QSettings().value("main/toolbarIconSize", 22).toInt() / 22);
    static const Qt::ToolBarArea areas[] = {Qt::LeftToolBarArea, Qt::TopToolBarArea, Qt::RightToolBarArea,
                                            Qt::BottomToolBarArea};
    auto *menuStyle = new MenuButtonStyle;
    menuStyle->setParent(m_toolbar);
    auto rebuild = [this, menuStyle] {
        m_toolbar->clear();
        for (const QString &id : QSettings().value("toolbar/ids", kToolbarDefault).toStringList()) {
            auto *a = action(id);
            if (!a) continue;
            m_toolbar->addAction(a);
            if (auto *b = qobject_cast<QToolButton *>(m_toolbar->widgetForAction(a)); b && a->menu()) {
                b->setPopupMode(QToolButton::InstantPopup);
                b->setStyle(menuStyle);
            }
        }
        // trailing customisation arrow
        auto *more = new QToolButton(m_toolbar);
        more->setIcon(icon("chevrons-right", {}));
        more->setToolTip(tr("Choose toolbar icons"));
        more->setPopupMode(QToolButton::InstantPopup);
        more->setStyle(menuStyle);
        auto *menu = new QMenu(more);
        more->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] {
            menu->clear();
            const QStringList ids = QSettings().value("toolbar/ids", kToolbarDefault).toStringList();
            auto *align = menu->addMenu(tr("Toolbar alignment"));
            const QString names[] = {tr("Left"), tr("Top"), tr("Right"), tr("Bottom")};
            for (int k = 0; k < 4; ++k) {
                auto *a = align->addAction(names[k], this, [this, k] {
                    QSettings().setValue("toolbar/alignment", k);
                    addToolBar(areas[k], m_toolbar);
                });
                a->setCheckable(true);
                a->setChecked(QSettings().value("toolbar/alignment", 0).toInt() == k);
            }
            menu->addSeparator();
            for (auto it = m_actions.cbegin(); it != m_actions.cend(); ++it) {
                if (it.key().startsWith("macro:")) continue;
                auto *a = menu->addAction(it.value()->icon(), commandName(it.value()));
                a->setCheckable(true);
                a->setChecked(ids.contains(it.key()));
                connect(a, &QAction::toggled, this, [this, id = it.key()](bool on) {
                    QStringList l = QSettings().value("toolbar/ids", kToolbarDefault).toStringList();
                    if (on) l << id;
                    else l.removeAll(id);
                    QSettings().setValue("toolbar/ids", l);
                    QTimer::singleShot(0, this, [this] { buildToolbar(); });
                });
            }
        });
        m_toolbar->addWidget(more);
    };
    rebuild();
    addToolBar(areas[std::clamp(QSettings().value("toolbar/alignment", 0).toInt(), 0, 3)], m_toolbar);
    m_toolbar->setVisible(QSettings().value("toolbar/visible", true).toBool());
}

void MainWindow::buildStatusBar() {
    for (int i = 0; i < 9; ++i) {
        m_statusFields[i] = new QLabel(this);
        m_statusFields[i]->setContentsMargins(6, 0, 6, 0);
        if (i == 0) statusBar()->addWidget(m_statusFields[i], 1);
        else statusBar()->addPermanentWidget(m_statusFields[i]);
        // a field with nothing to say is not there: without a video seven of them were frames
        // around nothing
        if (i) m_statusFields[i]->hide();
    }
    m_statusFields[1]->setToolTip(tr("Video scale"));
    m_statusFields[2]->setToolTip(tr("Video zoom"));
    m_statusFields[3]->setToolTip(tr("Video duration"));
    m_statusFields[4]->setToolTip(tr("FPS"));
    m_statusFields[5]->setToolTip(tr("Video resolution"));
    m_statusFields[6]->setToolTip(tr("Aspect ratio"));
    m_statusFields[7]->setToolTip(tr("Subtitle resolution"));
    m_statusFields[8]->setToolTip(tr("Video file"));
}

void MainWindow::updateStatus() {
    auto *tab = current();
    if (!tab) return;
    const auto fi = tab->s().file_info();
    auto *v = tab->video();
    const bool has = v->hasVideo();
    const auto info = v->info();
    // The fields are written only where they differ: this runs on every change of the lines, and
    // a label cleared and set again lays the status bar out (and paints it) for each key typed.
    QString fields[9], resolutionStyle;
    if (has && info.width > 0) {
        const QRectF r = v->view()->displayRect();
        const QRectF z = v->view()->zoom();
        fields[1] = QString("%1%").arg(qRound(r.width() * z.width() / info.width * 100));
        if (z.width() < 0.999) fields[2] = QString("%1%").arg(qRound(100 / z.width()));
        fields[3] = fs::fmtTime(info.duration_ms);
        fields[4] = QString::number(info.fps, 'f', 3) + (info.vfr ? " VFR" : "");
        fields[5] = QString("%1x%2").arg(info.width).arg(info.height);
        if (info.width != fi.play_res_x || info.height != fi.play_res_y) resolutionStyle = "color: #d04040;";
        const double sar = info.sar_den > 0 && info.sar_num > 0 ? double(info.sar_num) / info.sar_den : 1.0;
        fields[6] = QString::number(info.width * sar / info.height, 'f', 3);
        fields[8] = QFileInfo(v->path()).fileName();
    }
    fields[7] = QString("%1x%2").arg(fi.play_res_x).arg(fi.play_res_y);
    for (int i = 1; i < 9; ++i)
        if (m_statusFields[i]->text() != fields[i]) {
            m_statusFields[i]->setText(fields[i]);
            m_statusFields[i]->setVisible(!fields[i].isEmpty());
        }
    if (m_statusFields[5]->styleSheet() != resolutionStyle) m_statusFields[5]->setStyleSheet(resolutionStyle);
}

void MainWindow::status(const QString &msg) {
    m_statusFields[0]->setText(msg);
    QTimer::singleShot(8000, m_statusFields[0], [l = m_statusFields[0], msg] {
        if (l->text() == msg) l->clear();
    });
}

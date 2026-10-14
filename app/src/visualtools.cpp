#include "visualtools.h"

#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QToolButton>
#include <QtMath>
#include <map>

// ---- Drawing ------------------------------------------------------------------

Drawing Drawing::parse(const QString &s) {
    Drawing d;
    const QStringList tok = s.simplified().split(' ', Qt::SkipEmptyParts);
    char type = 0;
    std::vector<double> nums;
    auto flush = [&] {
        if (!type) return;
        Cmd c{type, {}};
        for (size_t i = 0; i + 1 < nums.size(); i += 2) c.pts.emplace_back(nums[i], nums[i + 1]);
        if (type == 'b') {
            // one cmd per curve segment so handles map 1:1 to commands
            for (size_t i = 0; i + 2 < c.pts.size(); i += 3)
                d.cmds.push_back({'b', {c.pts[i], c.pts[i + 1], c.pts[i + 2]}});
        } else if (type == 'l' || type == 'm' || type == 'n') {
            for (const auto &p : c.pts) d.cmds.push_back({type, {p}});
            if (c.pts.empty() && type == 'm') d.cmds.push_back(c);
        } else {
            d.cmds.push_back(c);
        }
        nums.clear();
    };
    for (const auto &t : tok) {
        bool ok;
        const double v = t.toDouble(&ok);
        if (ok) {
            nums.push_back(v);
        } else if (t.size() == 1 && QString("mnlbspc").contains(t[0].toLower())) {
            flush();
            type = char(t[0].toLower().toLatin1());
        }
    }
    flush();
    return d;
}

QString Drawing::toString() const {
    auto num = [](double v) {
        const double r = std::round(v * 100) / 100;
        return r == std::floor(r) ? QString::number(qint64(r)) : QString::number(r, 'f', 2).remove(QRegularExpression("0+$"));
    };
    QStringList out;
    char last = 0;
    for (const auto &c : cmds) {
        // repeat letters only when the command changes (l 1 2 3 4)
        if (c.type != last || c.type == 'm' || c.type == 'b') out << QString(QChar(c.type));
        last = c.type;
        for (const auto &p : c.pts) out << num(p.x()) << num(p.y());
    }
    return out.join(' ');
}

std::vector<std::pair<int, int>> Drawing::handles() const {
    std::vector<std::pair<int, int>> h;
    for (int i = 0; i < int(cmds.size()); ++i)
        for (int j = 0; j < int(cmds[i].pts.size()); ++j) h.emplace_back(i, j);
    return h;
}

QRectF Drawing::bounds() const {
    // QRectF::united skips zero-size rects, so track the extremes by hand
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool first = true;
    for (const auto &c : cmds)
        for (const auto &p : c.pts) {
            if (first) x0 = x1 = p.x(), y0 = y1 = p.y();
            x0 = std::min(x0, p.x()), x1 = std::max(x1, p.x()), y0 = std::min(y0, p.y()), y1 = std::max(y1, p.y());
            first = false;
        }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

// ---- tools --------------------------------------------------------------------

namespace {
const QColor kHandle(255, 255, 255), kLine(240, 200, 60), kAccent(230, 70, 70), kShadow(0, 0, 0, 160);

QString n2(double v) {
    const double r = std::round(v * 100) / 100;
    return r == std::floor(r) ? QString::number(qint64(r)) : QString::number(r, 'f', 2);
}

void handle(QPainter &p, QPointF c, bool round = false, QColor col = kHandle) {
    p.setPen(QPen(kShadow, 3));
    const QRectF r(c.x() - 4, c.y() - 4, 8, 8);
    round ? p.drawEllipse(r) : p.drawRect(r);
    p.setPen(QPen(col, 1.5));
    round ? p.drawEllipse(r) : p.drawRect(r);
}

void line(QPainter &p, QPointF a, QPointF b, QColor c = kLine, Qt::PenStyle st = Qt::SolidLine) {
    p.setPen(QPen(kShadow, 3));
    p.drawLine(a, b);
    p.setPen(QPen(c, 1.2, st));
    p.drawLine(a, b);
}

bool near(QPointF a, QPointF b, double r = 7) { return QLineF(a, b).length() <= r; }
} // namespace

VisualTools::VisualTools(VideoBox *box) : QObject(box), m_box(box) {}

QPointF VisualTools::w(QPointF p) const { return m_box->view()->toWidget(p); }
QPointF VisualTools::s(QPointF p) const { return m_box->view()->toScript(p); }

QWidget *VisualTools::toolRow(QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(1);
    const char *icons[] = {"⌖", "P", "M", "S", "Z", "XY", "▭", "✂", "✎", "⇄"};
    const char *tips[] = {"Position pointer", "Text positioning", "Text moving", "Text scaling", "Z rotation",
                          "X/Y rotation", "Rectangle clipping", "Vector clipping", "Vector drawing",
                          "Position shifter"};
    auto *group = new QButtonGroup(row);
    for (int t = 0; t < ToolCount; ++t) {
        auto *b = new QToolButton(row);
        b->setText(QString::fromUtf8(icons[t]));
        b->setToolTip(tr(tips[t]));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFixedSize(26, 26);
        group->addButton(b, t);
        h->addWidget(b);
        m_buttons.push_back(b);
    }
    // shortcut: Hydra (multi-line tag gradients) is not implemented; the button is shown disabled for layout parity.
    auto *hydra = new QToolButton(row);
    hydra->setText("H");
    hydra->setToolTip(tr("Hydra"));
    hydra->setEnabled(false);
    hydra->setFixedSize(26, 26);
    h->addWidget(hydra);
    m_buttons[0]->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, &VisualTools::setTool);

    m_sub = new QStackedWidget(row);
    m_sub->addWidget(new QWidget(m_sub)); // empty
    auto *vec = new QWidget(m_sub);
    auto *vh = new QHBoxLayout(vec);
    vh->setContentsMargins(6, 0, 0, 0);
    vh->setSpacing(1);
    auto *modes = new QButtonGroup(vec);
    const char *mt[] = {"Move points", "Add line", "Add Bézier curve", "Add separate point (move)", "Delete point"};
    const char *mi[] = {"↔", "╱", "∿", "•", "✕"};
    for (int k = 0; k < 5; ++k) {
        auto *b = new QToolButton(vec);
        b->setText(QString::fromUtf8(mi[k]));
        b->setToolTip(tr(mt[k]));
        b->setCheckable(true);
        b->setAutoRaise(true);
        modes->addButton(b, k);
        vh->addWidget(b);
        if (k == 0) b->setChecked(true);
    }
    connect(modes, &QButtonGroup::idClicked, this, [this](int k) { m_vecMode = k; });
    m_sub->addWidget(vec);
    auto *invert = new QToolButton(row);
    invert->setText(tr("Invert clip"));
    invert->setAutoRaise(true);
    connect(invert, &QToolButton::clicked, this, [this] {
        if (!m_valid || !m_v.clip) return;
        m_v.inverse = !m_v.inverse;
        m_amend = QString("visual-invert-%1").arg(++m_dragId);
        apply(clipTag());
    });
    vh->addWidget(invert);
    m_sub->addWidget(new QWidget(m_sub));
    h->addWidget(m_sub);
    h->addStretch();
    m_invert = invert;
    return row;
}

void VisualTools::setTool(int t) {
    m_tool = t;
    if (t < int(m_buttons.size())) m_buttons[t]->setChecked(true);
    if (m_sub) {
        m_sub->setCurrentIndex(t == VClip || t == Draw || t == RClip ? 1 : 0);
        m_invert->setVisible(t == VClip || t == RClip);
        for (auto *b : m_sub->widget(1)->findChildren<QToolButton *>())
            if (b != m_invert) b->setVisible(t != RClip);
    }
    reload();
    m_box->view()->setCursor(t == Pointer ? Qt::ArrowCursor : Qt::CrossCursor);
    m_box->view()->updateOverlay();
}

void VisualTools::reload() {
    auto *tab = m_box->tab();
    const int row = tab->active();
    m_valid = row >= 0 && size_t(row) < tab->s().row_count() && m_box->hasVideo();
    if (!m_valid) return;
    m_v = tab->s().visual(size_t(row));
    if (m_drag) return; // keep the vector and its frame stable while dragging
    if (m_tool == VClip) m_vec = m_v.clip == 2 ? Drawing::parse(fs::q(m_v.clip_drawing)) : Drawing{};
    if (m_tool == Draw) {
        m_vec = Drawing::parse(fs::q(m_v.drawing));
        drawingFrame(m_drawOrigin, m_drawScale);
    }
}

void VisualTools::drawingFrame(QPointF &origin, QPointF &scale) const {
    const double k = std::pow(2.0, -(std::max(1, m_v.drawing_scale) - 1));
    scale = QPointF(m_v.fscx / 100 * k, m_v.fscy / 100 * k);
    const QRectF b = m_vec.bounds();
    const double wsz = b.width() * scale.x(), hsz = b.height() * scale.y();
    const int an = m_v.align;
    const double ox = an % 3 == 1 ? 0 : an % 3 == 2 ? -wsz / 2 : -wsz;
    const double oy = an >= 7 ? 0 : an >= 4 ? -hsz / 2 : -hsz;
    // shortcut: rotation and \org are ignored for drawings, and the origin assumes libass aligns the point bounding box; fine for upright shapes.
    origin = QPointF(m_v.x + ox - b.left() * scale.x(), m_v.y + oy - b.top() * scale.y());
}

void VisualTools::apply(const QString &tags, bool allSelected) {
    auto *tab = m_box->tab();
    std::vector<size_t> rows = allSelected ? tab->selection() : std::vector<size_t>{size_t(tab->active())};
    tab->s().set_tags(fs::slice(rows), fs::Utf8(tags), fs::Utf8(m_amend));
    tab->changed(TabPanel::FromVideo);
}

void VisualTools::applyPos(QPointF p) {
    apply(QString("\\pos(%1,%2)").arg(n2(p.x()), n2(p.y())));
}

QString VisualTools::clipTag() const {
    const QString name = m_v.inverse ? "iclip" : "clip";
    if (m_v.clip == 1)
        return QString("\\%1(%2,%3,%4,%5)").arg(name).arg(qRound(m_v.clip_x1)).arg(qRound(m_v.clip_y1))
            .arg(qRound(m_v.clip_x2)).arg(qRound(m_v.clip_y2));
    const QString scale = m_v.clip_scale > 1 ? QString::number(m_v.clip_scale) + "," : QString();
    return QString("\\%1(%2%3)").arg(name, scale, m_vec.toString());
}

void VisualTools::writeVector() {
    auto *tab = m_box->tab();
    if (m_tool == VClip) {
        if (m_v.clip != 2) m_v.clip_scale = 1;
        m_v.clip = 2;
        apply(clipTag());
    } else {
        tab->s().set_drawing(size_t(tab->active()), fs::Utf8(m_vec.toString()), fs::Utf8(m_amend));
        tab->changed(TabPanel::FromVideo);
    }
}

int VisualTools::hitHandle(QPointF wp) const {
    const auto hs = m_vec.handles();
    for (int i = int(hs.size()) - 1; i >= 0; --i) {
        const QPointF pt = m_vec.cmds[hs[i].first].pts[hs[i].second];
        QPointF sp;
        if (m_tool == VClip) sp = pt / std::pow(2.0, std::max(1, m_v.clip_scale) - 1);
        else sp = m_drawOrigin + QPointF(pt.x() * m_drawScale.x(), pt.y() * m_drawScale.y());
        if (near(w(sp), wp)) return i;
    }
    return -1;
}

// ---- painting -----------------------------------------------------------------

void VisualTools::paint(QPainter &p) {
    const QRectF d = m_box->view()->displayRect();
    if (m_tool == Pointer) {
        const QPointF m = m_box->view()->mapFromGlobal(QCursor::pos()).toPointF();
        if (!d.contains(m)) return;
        line(p, {d.left(), m.y()}, {d.right(), m.y()}, QColor(255, 255, 255, 120), Qt::DashLine);
        line(p, {m.x(), d.top()}, {m.x(), d.bottom()}, QColor(255, 255, 255, 120), Qt::DashLine);
        const QPointF sp = s(m);
        const QString t = QString("%1, %2").arg(qRound(sp.x())).arg(qRound(sp.y()));
        p.setPen(kShadow);
        p.drawText(m + QPointF(9, -7), t);
        p.setPen(kHandle);
        p.drawText(m + QPointF(8, -8), t);
        return;
    }
    if (!m_valid) return;
    p.setClipRect(d);
    const QPointF pos = w({m_v.x, m_v.y});
    switch (m_tool) {
    case Pos:
    case Shifter:
        handle(p, pos, !m_v.has_pos, m_v.has_pos ? kHandle : kLine);
        break;
    case Move: {
        const QPointF end = w({m_v.x2, m_v.y2});
        line(p, pos, end);
        handle(p, pos);
        handle(p, end, true, kAccent);
        break;
    }
    case Scale: {
        const QPointF ex = pos + QPointF(m_v.fscx, 0), ey = pos - QPointF(0, m_v.fscy);
        line(p, pos, ex);
        line(p, pos, ey);
        handle(p, ex);
        handle(p, ey);
        handle(p, pos, true);
        break;
    }
    case RotZ: {
        const QPointF org = w({m_v.org_x, m_v.org_y});
        const double a = qDegreesToRadians(m_v.frz);
        const QPointF tip = org + QPointF(std::cos(a), -std::sin(a)) * 70;
        p.setPen(QPen(kLine, 1, Qt::DashLine));
        p.drawEllipse(org, 70, 70);
        line(p, org, tip);
        line(p, org, pos, QColor(255, 255, 255, 100), Qt::DotLine);
        handle(p, tip, true);
        handle(p, org, false, kAccent);
        handle(p, pos, true);
        break;
    }
    case RotXY: {
        p.save();
        p.translate(pos);
        // the grid is foreshortened like the rotated text plane
        p.scale(std::cos(qDegreesToRadians(m_v.fry)), std::cos(qDegreesToRadians(m_v.frx)));
        p.setPen(QPen(kLine, 1));
        for (int k = -3; k <= 3; ++k) {
            p.drawLine(QPointF(k * 20, -60), QPointF(k * 20, 60));
            p.drawLine(QPointF(-60, k * 20), QPointF(60, k * 20));
        }
        p.restore();
        handle(p, pos, true);
        break;
    }
    case RClip:
        if (m_v.clip == 1) {
            const QRectF r(w({m_v.clip_x1, m_v.clip_y1}), w({m_v.clip_x2, m_v.clip_y2}));
            if (m_v.inverse) p.fillRect(r, QColor(0, 0, 0, 90));
            else {
                QPainterPath outside;
                outside.addRect(d);
                outside.addRect(r.normalized());
                p.fillPath(outside, QColor(0, 0, 0, 90));
            }
            p.setPen(QPen(kLine, 1));
            p.drawRect(r);
            for (QPointF c : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()}) handle(p, c);
        }
        break;
    case VClip:
    case Draw: {
        auto map = [this](QPointF pt) {
            if (m_tool == VClip) return w(pt / std::pow(2.0, std::max(1, m_v.clip_scale) - 1));
            return w(m_drawOrigin + QPointF(pt.x() * m_drawScale.x(), pt.y() * m_drawScale.y()));
        };
        QPainterPath path;
        QPointF last;
        for (const auto &c : m_vec.cmds) {
            if (c.pts.empty()) continue;
            if (c.type == 'm' || c.type == 'n') path.moveTo(map(c.pts[0]));
            else if (c.type == 'b' && c.pts.size() == 3) path.cubicTo(map(c.pts[0]), map(c.pts[1]), map(c.pts[2]));
            else for (const auto &pt : c.pts) path.lineTo(map(pt));
        }
        if (m_tool == VClip && !m_vec.cmds.empty()) {
            QPainterPath shade;
            shade.addRect(d);
            shade.addPath(path);
            if (!m_v.inverse) p.fillPath(shade, QColor(0, 0, 0, 90));
            else p.fillPath(path, QColor(0, 0, 0, 90));
        }
        p.setPen(QPen(kShadow, 3));
        p.drawPath(path);
        p.setPen(QPen(kLine, 1.2));
        p.drawPath(path);
        for (const auto &c : m_vec.cmds) {
            if (c.type == 'b' && c.pts.size() == 3) {
                line(p, last, map(c.pts[0]), QColor(255, 255, 255, 120), Qt::DashLine);
                line(p, map(c.pts[1]), map(c.pts[2]), QColor(255, 255, 255, 120), Qt::DashLine);
                handle(p, map(c.pts[0]), true);
                handle(p, map(c.pts[1]), true);
            }
            if (!c.pts.empty()) {
                last = map(c.pts.back());
                handle(p, last, false, c.type == 'm' ? kAccent : kHandle);
            }
        }
        if (m_tool == Draw) handle(p, pos, true, kAccent);
        break;
    }
    }
}

// ---- mouse --------------------------------------------------------------------

bool VisualTools::press(QMouseEvent *e) {
    m_mouseW = e->position();
    auto *tab = m_box->tab();
    if (m_tool == Pointer) {
        if (m_valid && (e->button() == Qt::MiddleButton ||
                        (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)))) {
            m_amend = QString("visual-%1").arg(++m_dragId);
            applyPos(s(e->position()));
        }
        return true;
    }
    if (!m_valid) return false;
    m_button = e->button();
    m_pressW = e->position();
    m_pressS = s(m_pressW);
    m_start = m_v;
    m_amend = QString("visual-%1").arg(++m_dragId);
    m_drag = 1;
    switch (m_tool) {
    case Move:
        if (m_button == Qt::RightButton) m_drag = 2;
        break;
    case RotZ:
        if (near(w({m_v.org_x, m_v.org_y}), m_pressW, 9)) m_drag = 2;
        break;
    case RClip:
        if (m_v.clip == 1) {
            const QRectF r(w({m_v.clip_x1, m_v.clip_y1}), w({m_v.clip_x2, m_v.clip_y2}));
            const QPointF cs[] = {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
            for (int k = 0; k < 4; ++k)
                if (near(cs[k], m_pressW)) m_drag = 10 + k;
        }
        break;
    case VClip:
    case Draw: {
        const int h = hitHandle(m_pressW);
        const auto hs = m_vec.handles();
        // new point in drawing units
        QPointF pt = m_pressS;
        if (m_tool == VClip) pt *= std::pow(2.0, std::max(1, m_v.clip_scale) - 1);
        else {
            if (m_vec.cmds.empty()) {
                // a fresh drawing starts at the line position
                m_drawScale = QPointF(m_v.fscx / 100, m_v.fscy / 100);
                m_drawOrigin = QPointF(m_v.x, m_v.y);
            }
            pt = QPointF((m_pressS.x() - m_drawOrigin.x()) / m_drawScale.x(),
                         (m_pressS.y() - m_drawOrigin.y()) / m_drawScale.y());
        }
        if (m_vecMode == VecDelete) {
            m_drag = 0;
            if (h < 0) return false;
            m_vec.cmds.erase(m_vec.cmds.begin() + hs[h].first);
            if (!m_vec.cmds.empty() && m_vec.cmds[0].type != 'm') m_vec.cmds[0] = {'m', {m_vec.cmds[0].pts.back()}};
            writeVector();
            return true;
        }
        if (h >= 0 && (m_vecMode == VecMove || e->button() != Qt::LeftButton)) {
            m_vecHandle = hs[h];
            return true;
        }
        if (m_vec.cmds.empty() || m_vecMode == VecNewFigure) {
            m_vec.cmds.push_back({'m', {pt}});
        } else if (m_vecMode == VecBezier) {
            const QPointF last = m_vec.cmds.back().pts.back();
            m_vec.cmds.push_back({'b', {last + (pt - last) / 3, last + (pt - last) * 2 / 3, pt}});
        } else {
            m_vec.cmds.push_back({'l', {pt}});
        }
        m_vecHandle = {int(m_vec.cmds.size()) - 1, int(m_vec.cmds.back().pts.size()) - 1};
        writeVector();
        return true;
    }
    default:
        break;
    }
    Q_UNUSED(tab);
    return move(e);
}

bool VisualTools::move(QMouseEvent *e) {
    m_mouseW = e->position();
    if (m_tool == Pointer) return true;
    if (!m_drag || !m_valid) return false;
    const QPointF now = s(e->position());
    QPointF delta = now - m_pressS;
    if (e->modifiers() & Qt::ShiftModifier) {
        // constrain to the dominant axis
        if (std::abs(delta.x()) > std::abs(delta.y())) delta.setY(0);
        else delta.setX(0);
    }
    const QPointF dw = e->position() - m_pressW;
    auto *tab = m_box->tab();
    switch (m_tool) {
    case Pos: {
        const auto sel = tab->selection();
        if (sel.size() > 1) {
            // every selected line moves by the same delta from where it started
            static std::map<size_t, QPointF> starts;
            if (m_drag == 1) {
                starts.clear();
                for (size_t r : sel) {
                    const auto v = tab->s().visual(r);
                    starts[r] = {v.x, v.y};
                }
                m_drag = 3;
            }
            for (size_t r : sel) {
                const QPointF p = starts[r] + delta;
                const size_t row = r;
                tab->s().set_tags({&row, 1}, fs::Utf8(QString("\\pos(%1,%2)").arg(n2(p.x()), n2(p.y()))), fs::Utf8(m_amend));
            }
            tab->changed(TabPanel::FromVideo);
        } else {
            applyPos(QPointF(m_start.x, m_start.y) + delta);
        }
        break;
    }
    case Move: {
        QPointF a(m_start.x, m_start.y), b(m_start.x2, m_start.y2);
        if (m_drag == 2) b = m_start.has_move || near(w(b), m_pressW, 9) ? b + delta : now;
        else a += delta;
        QString t = QString("\\move(%1,%2,%3,%4").arg(n2(a.x()), n2(a.y()), n2(b.x()), n2(b.y()));
        if (m_start.t2 > m_start.t1) t += QString(",%1,%2").arg(m_start.t1).arg(m_start.t2);
        apply(t + ")");
        break;
    }
    case Scale: {
        double sx = m_start.fscx, sy = m_start.fscy;
        if (m_button == Qt::LeftButton) sx = std::max(0.0, sx + dw.x());
        else if (m_button == Qt::RightButton) sy = std::max(0.0, sy - dw.y());
        else {
            const double f = std::max(0.0, 1 + dw.x() / 100);
            sx *= f;
            sy *= f;
        }
        apply(QString("\\fscx%1\\fscy%2").arg(n2(sx), n2(sy)));
        break;
    }
    case RotZ: {
        if (m_drag == 2) {
            const QPointF o = QPointF(m_start.org_x, m_start.org_y) + delta;
            apply(QString("\\org(%1,%2)").arg(n2(o.x()), n2(o.y())));
            break;
        }
        const QPointF c = w({m_start.org_x, m_start.org_y});
        const double a0 = std::atan2(-(m_pressW.y() - c.y()), m_pressW.x() - c.x());
        const double a1 = std::atan2(-(e->position().y() - c.y()), e->position().x() - c.x());
        double frz = std::fmod(m_start.frz + qRadiansToDegrees(a1 - a0) + 720, 360);
        apply(QString("\\frz%1").arg(n2(frz)));
        break;
    }
    case RotXY: {
        double frx = m_start.frx, fry = m_start.fry;
        if (m_button != Qt::RightButton) fry = m_start.fry + dw.x() * 0.5;
        if (m_button != Qt::LeftButton) frx = m_start.frx - dw.y() * 0.5;
        apply(QString("\\frx%1\\fry%2").arg(n2(frx), n2(fry)));
        break;
    }
    case RClip: {
        if (m_drag == 1) {
            m_v.clip = 1;
            m_v.clip_x1 = std::min(m_pressS.x(), now.x());
            m_v.clip_y1 = std::min(m_pressS.y(), now.y());
            m_v.clip_x2 = std::max(m_pressS.x(), now.x());
            m_v.clip_y2 = std::max(m_pressS.y(), now.y());
        } else {
            const int k = m_drag - 10;
            (k % 2 ? m_v.clip_x2 : m_v.clip_x1) = (k % 2 ? m_start.clip_x2 : m_start.clip_x1) + delta.x();
            (k / 2 ? m_v.clip_y2 : m_v.clip_y1) = (k / 2 ? m_start.clip_y2 : m_start.clip_y1) + delta.y();
        }
        apply(clipTag());
        break;
    }
    case VClip:
    case Draw: {
        if (m_vecHandle.first < 0) break;
        QPointF pt = now;
        if (m_tool == VClip) pt *= std::pow(2.0, std::max(1, m_v.clip_scale) - 1);
        else pt = QPointF((now.x() - m_drawOrigin.x()) / m_drawScale.x(), (now.y() - m_drawOrigin.y()) / m_drawScale.y());
        // Alt snaps to another point
        if (e->modifiers() & Qt::AltModifier)
            for (const auto &h : m_vec.handles())
                if (h != m_vecHandle && QLineF(m_vec.at(h), pt).length() < 6 / m_box->view()->scriptScale())
                    pt = m_vec.at(h);
        m_vec.at(m_vecHandle) = pt;
        writeVector();
        break;
    }
    case Shifter: {
        static QPointF applied;
        if (m_drag == 1) {
            applied = {};
            m_drag = 3;
        }
        const QPointF step = delta - applied;
        applied = delta;
        tab->s().shift_positions(fs::slice(tab->selection()), step.x(), step.y(), fs::Utf8(m_amend));
        tab->changed(TabPanel::FromVideo);
        break;
    }
    }
    return true;
}

bool VisualTools::release(QMouseEvent *e) {
    if (!m_drag) return m_tool == Pointer;
    m_drag = 0;
    m_vecHandle = {-1, -1};
    m_button = Qt::NoButton;
    Q_UNUSED(e);
    reload();
    return true;
}

bool VisualTools::key(QKeyEvent *e) {
    if (!m_valid || (m_tool != Pos && m_tool != Move)) return false;
    const double st = e->modifiers() & Qt::ShiftModifier ? 0.1 : 1;
    QPointF d;
    switch (e->key()) {
    case Qt::Key_W: d = {0, -st}; break;
    case Qt::Key_S: d = {0, st}; break;
    case Qt::Key_A: d = {-st, 0}; break;
    case Qt::Key_D: d = {st, 0}; break;
    default: return false;
    }
    m_amend = QString("visual-nudge-%1").arg(m_box->tab()->active());
    if (m_tool == Pos) applyPos(QPointF(m_v.x, m_v.y) + d);
    else {
        QString t = QString("\\move(%1,%2,%3,%4").arg(n2(m_v.x + d.x()), n2(m_v.y + d.y()), n2(m_v.x2), n2(m_v.y2));
        if (m_v.t2 > m_v.t1) t += QString(",%1,%2").arg(m_v.t1).arg(m_v.t2);
        apply(t + ")");
    }
    reload();
    return true;
}

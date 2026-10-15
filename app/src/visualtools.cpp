#include "visualtools.h"

#include "icons.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QSettings>
#include <QToolButton>
#include <QtMath>

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

QPointF Drawing::last() const {
    for (auto c = cmds.rbegin(); c != cmds.rend(); ++c)
        if (!c->pts.empty()) return c->pts.back();
    return {};
}

void Drawing::translate(QPointF d) {
    for (auto &c : cmds)
        for (auto &p : c.pts) p += d;
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
    return QString::number(r == 0 ? 0.0 : r, 'g', 12); // 12.5, not 12.50; 0, not -0
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

// Shapes for the vector tools, drawn in a 0..100 box.
const std::pair<const char *, const char *> kShapes[] = {
    {QT_TRANSLATE_NOOP("VisualTools", "Rectangle"), "m 0 0 l 100 0 100 100 0 100"},
    {QT_TRANSLATE_NOOP("VisualTools", "Ellipse"),
     "m 50 0 b 78 0 100 22 100 50 b 100 78 78 100 50 100 b 22 100 0 78 0 50 b 0 22 22 0 50 0"},
    {QT_TRANSLATE_NOOP("VisualTools", "Rounded rectangle"),
     "m 15 0 l 85 0 b 93 0 100 7 100 15 l 100 85 b 100 93 93 100 85 100 l 15 100 "
     "b 7 100 0 93 0 85 l 0 15 b 0 7 7 0 15 0"},
    {QT_TRANSLATE_NOOP("VisualTools", "Triangle"), "m 50 0 l 100 100 0 100"},
    {QT_TRANSLATE_NOOP("VisualTools", "Star"), "m 50 0 l 61 35 98 35 68 57 79 91 50 70 21 91 32 57 2 35 39 35"},
};
} // namespace

VisualTools::VisualTools(VideoBox *box)
    : QObject(box), m_box(box), m_shiftWhat(QSettings().value("visual/shiftWhat", 47).toInt()),
      m_scaleWhat(QSettings().value("visual/scaleWhat", 5).toInt()) {}

QPointF VisualTools::w(QPointF p) const { return m_box->view()->toWidget(p); }
QPointF VisualTools::s(QPointF p) const { return m_box->view()->toScript(p); }

static const ToolInfo kTools[] = {
    {"toolPointer", "pointer", QT_TRANSLATE_NOOP("VisualTools", "Position pointer")},
    {"toolPos", "crosshair", QT_TRANSLATE_NOOP("VisualTools", "Text positioning")},
    {"toolMove", "route", QT_TRANSLATE_NOOP("VisualTools", "Text moving")},
    {"toolScale", "arrows-diagonal", QT_TRANSLATE_NOOP("VisualTools", "Text scaling")},
    {"toolRotZ", "rotate-clockwise", QT_TRANSLATE_NOOP("VisualTools", "Z rotation")},
    {"toolRotXY", "rotate-3d", QT_TRANSLATE_NOOP("VisualTools", "X/Y rotation")},
    {"toolRClip", "crop", QT_TRANSLATE_NOOP("VisualTools", "Rectangle clipping")},
    {"toolVClip", "scissors", QT_TRANSLATE_NOOP("VisualTools", "Vector clipping")},
    {"toolDraw", "vector-bezier", QT_TRANSLATE_NOOP("VisualTools", "Vector drawing")},
    {"toolShifter", "arrows-move", QT_TRANSLATE_NOOP("VisualTools", "Position shifter")},
    {"toolAllTags", "typography", QT_TRANSLATE_NOOP("VisualTools", "All tags (drag left/right to change the value)")},
};
static_assert(std::size(kTools) == VisualTools::ToolCount);

std::span<const ToolInfo> VisualTools::tools() { return kTools; }

QWidget *VisualTools::toolRow(QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(1);
    auto *group = new QButtonGroup(row);
    for (int t = 0; t < ToolCount; ++t) {
        auto *b = new QToolButton(row);
        b->setIcon(icon(kTools[t].icon, QColor(t == AllTags ? 0xf2665c : t >= 6 && t <= 8 ? 0xb48cff : 0x4aa3ff)));
        b->setToolTip(tr(kTools[t].name));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFixedSize(26, 26);
        group->addButton(b, t);
        h->addWidget(b);
        m_buttons.push_back(b);
    }
    m_buttons[0]->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, &VisualTools::setTool);

    m_sub = new QStackedWidget(row);
    m_sub->addWidget(new QWidget(m_sub)); // empty
    auto *vec = new QWidget(m_sub);
    auto *vh = new QHBoxLayout(vec);
    vh->setContentsMargins(6, 0, 0, 0);
    vh->setSpacing(1);
    auto *modes = new QButtonGroup(vec);
    const char *mt[] = {QT_TR_NOOP("Move points"), QT_TR_NOOP("Add line"), QT_TR_NOOP("Add Bézier curve"),
                        QT_TR_NOOP("Add separate point (move)"), QT_TR_NOOP("Delete point"),
                        QT_TR_NOOP("Draw freehand"), QT_TR_NOOP("Insert a point into a segment"),
                        QT_TR_NOOP("Convert a segment between line and Bézier"), QT_TR_NOOP("Add B-spline point")};
    const char *mi[] = {"arrows-move", "line",    "vector-bezier-2", "point",        "x",
                        "scribble",    "circle-plus", "replace",      "vector-spline"};
    for (int k = 0; k < 9; ++k) {
        auto *b = new QToolButton(vec);
        b->setIcon(icon(mi[k], QColor(k == 4 ? 0xf2665c : 0xb48cff)));
        b->setToolTip(tr(mt[k]));
        b->setCheckable(true);
        b->setAutoRaise(true);
        modes->addButton(b, k);
        vh->addWidget(b);
    }
    m_vecMode = std::clamp(QSettings().value("visual/vecMode", 0).toInt(), 0, 8);
    modes->button(m_vecMode)->setChecked(true);
    connect(modes, &QButtonGroup::idClicked, this, [this](int k) {
        m_vecMode = k;
        QSettings().setValue("visual/vecMode", k);
    });
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
    m_shapeBox = new QComboBox(vec);
    m_shapeBox->setToolTip(tr("Ready shape: pick one, then drag a rectangle on the video (Shift keeps it square)"));
    // the ready shapes, then the saved ones by name
    const int ready = 1 + int(std::size(kShapes));
    auto fill = [this] {
        m_shapeBox->clear();
        m_shapeBox->addItem(tr("Shape..."));
        for (auto [name, path] : kShapes) m_shapeBox->addItem(tr(name), QString(path));
        const QVariantMap own = QSettings().value("visual/shapes").toMap();
        for (auto it = own.begin(); it != own.end(); ++it) m_shapeBox->addItem(it.key(), it.value());
    };
    fill();
    auto *keep = new QToolButton(vec), *drop = new QToolButton(vec);
    connect(m_shapeBox, &QComboBox::currentIndexChanged, this, [drop, ready](int k) {
        QSettings().setValue("visual/shape", k);
        drop->setEnabled(k >= ready);
    });
    m_shapeBox->setCurrentIndex(QSettings().value("visual/shape", 0).toInt());
    drop->setEnabled(m_shapeBox->currentIndex() >= ready);
    vh->addWidget(m_shapeBox);
    // Kainote's "Get shape from line": the line's drawing (or vector clip) joins the shapes, fitted
    // into the same 0..100 box with its proportions kept
    keep->setIcon(icon("device-floppy", QColor(0xb48cff)));
    keep->setToolTip(tr("Save the line's drawing as a shape"));
    keep->setAutoRaise(true);
    vh->addWidget(keep);
    connect(keep, &QToolButton::clicked, this, [this, fill, ready] {
        const QRectF r = m_vec.bounds();
        if (!m_valid || m_vec.cmds.empty() || std::max(r.width(), r.height()) <= 0) return;
        bool ok = false;
        const QString name =
            QInputDialog::getText(m_box, tr("Save shape"), tr("Shape name:"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        Drawing d = m_vec;
        d.translate(-r.topLeft());
        for (auto &c : d.cmds)
            for (QPointF &q : c.pts) q *= 100 / std::max(r.width(), r.height());
        QSettings st;
        QVariantMap own = st.value("visual/shapes").toMap();
        own[name] = d.toString();
        st.setValue("visual/shapes", own);
        fill();
        m_shapeBox->setCurrentIndex(ready + int(std::distance(own.begin(), own.find(name))));
    });
    drop->setIcon(icon("trash", QColor(0xf2665c)));
    drop->setToolTip(tr("Remove the saved shape"));
    drop->setAutoRaise(true);
    vh->addWidget(drop);
    connect(drop, &QToolButton::clicked, this, [this, fill, ready] {
        if (m_shapeBox->currentIndex() < ready) return;
        QSettings st;
        QVariantMap own = st.value("visual/shapes").toMap();
        own.remove(m_shapeBox->currentText());
        st.setValue("visual/shapes", own);
        fill(); // back on "Shape..."
    });
    vh->addStretch();
    // \pos / \move tools: Aegisub's "Toggle between \move and \pos" for the selected lines
    auto *drag = new QWidget(m_sub);
    auto *dh = new QHBoxLayout(drag);
    dh->setContentsMargins(6, 0, 0, 0);
    auto *conv = new QToolButton(drag);
    conv->setIcon(icon("arrows-exchange", QColor(0x4aa3ff)));
    conv->setToolTip(tr("Toggle between \\move and \\pos"));
    conv->setAutoRaise(true);
    dh->addWidget(conv);
    dh->addStretch();
    connect(conv, &QToolButton::clicked, this, &VisualTools::toggleMove);
    m_sub->addWidget(drag);
    // position shifter: which tags move (Kainote's toggles), bits as in Session::shift_positions
    auto *shift = new QWidget(m_sub);
    auto *sh = new QHBoxLayout(shift);
    sh->setContentsMargins(6, 0, 0, 0);
    sh->setSpacing(1);
    const char *what[] = {QT_TR_NOOP("\\pos"), QT_TR_NOOP("\\move start"), QT_TR_NOOP("\\move end"),
                          QT_TR_NOOP("clips"), QT_TR_NOOP("drawings"), QT_TR_NOOP("\\org")};
    for (int k = 0; k < 6; ++k) {
        auto *b = new QToolButton(shift);
        b->setText(tr(what[k]));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setChecked(m_shiftWhat & (1 << k));
        connect(b, &QToolButton::toggled, this, [this, k](bool on) {
            m_shiftWhat = on ? m_shiftWhat | (1 << k) : m_shiftWhat & ~(1 << k);
            QSettings().setValue("visual/shiftWhat", m_shiftWhat);
        });
        sh->addWidget(b);
    }
    sh->addStretch();
    m_sub->addWidget(shift);
    // all tags (Kainote's tool): one numeric tag over the selected lines
    auto *tags = new QWidget(m_sub);
    auto *th = new QHBoxLayout(tags);
    th->setContentsMargins(6, 0, 0, 0);
    m_tagName = new QComboBox(tags);
    for (const auto &t : fsb::adjustable_tags()) m_tagName->addItem("\\" + fs::q(t), fs::q(t));
    m_tagName->setCurrentIndex(std::max(0, m_tagName->findData(QSettings().value("visual/allTagsTag", "blur"))));
    m_tagMode = new QComboBox(tags);
    for (const char *m : {QT_TR_NOOP("Add"), QT_TR_NOOP("Set"), QT_TR_NOOP("Multiply by line number"),
                          QT_TR_NOOP("Gradient text increasing"), QT_TR_NOOP("Gradient text decreasing"),
                          QT_TR_NOOP("Gradient lines increasing"), QT_TR_NOOP("Gradient lines decreasing")})
        m_tagMode->addItem(tr(m));
    m_tagMode->setCurrentIndex(QSettings().value("visual/allTagsMode", 0).toInt());
    m_tagValue = new QDoubleSpinBox(tags);
    m_tagValue->setRange(-10000, 10000);
    m_tagValue->setDecimals(2);
    m_tagValue->setToolTip(tr("Value; Enter applies it to the selected lines"));
    th->addWidget(m_tagName);
    th->addWidget(m_tagMode);
    th->addWidget(m_tagValue);
    th->addStretch();
    connect(m_tagName, &QComboBox::currentIndexChanged, this,
            [this] { QSettings().setValue("visual/allTagsTag", m_tagName->currentData()); });
    connect(m_tagMode, &QComboBox::currentIndexChanged, this,
            [this](int k) { QSettings().setValue("visual/allTagsMode", k); });
    connect(m_tagValue, &QDoubleSpinBox::editingFinished, this, [this] {
        if (m_drag || !m_valid) return;
        auto *tab = m_box->tab();
        m_tagRows = tab->selection();
        m_tagBase.clear();
        for (size_t r : m_tagRows) m_tagBase.push_back(tab->s().row(r).text);
        m_amend = QString("visual-tags-%1").arg(++m_dragId);
        applyAllTags(m_tagValue->value());
    });
    m_sub->addWidget(tags);
    // scale tool (Kainote): which axes a left drag scales
    auto *scale = new QWidget(m_sub);
    auto *ch = new QHBoxLayout(scale);
    ch->setContentsMargins(6, 0, 0, 0);
    ch->setSpacing(1);
    const char *si[] = {"arrow-autofit-width", "link", "arrow-autofit-height"};
    const char *st[] = {QT_TR_NOOP("Scale width"), QT_TR_NOOP("Keep proportions"), QT_TR_NOOP("Scale height")};
    for (int k = 0; k < 3; ++k) {
        auto *b = new QToolButton(scale);
        b->setIcon(icon(si[k], QColor(0x4aa3ff)));
        b->setToolTip(tr(st[k]));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setChecked(m_scaleWhat & (1 << k));
        connect(b, &QToolButton::toggled, this, [this, k](bool on) {
            m_scaleWhat = on ? m_scaleWhat | (1 << k) : m_scaleWhat & ~(1 << k);
            QSettings().setValue("visual/scaleWhat", m_scaleWhat);
        });
        ch->addWidget(b);
    }
    ch->addStretch();
    m_sub->addWidget(scale);
    // Z rotation (Kainote): the angle from two points, e.g. along a sign's edge
    auto *rot = new QWidget(m_sub);
    auto *rh = new QHBoxLayout(rot);
    rh->setContentsMargins(6, 0, 0, 0);
    auto *two = new QToolButton(rot);
    two->setIcon(icon("line", QColor(0x4aa3ff)));
    two->setToolTip(tr("Set the angle from 2 points: drag along an edge in the video"));
    two->setCheckable(true);
    two->setAutoRaise(true);
    connect(two, &QToolButton::toggled, this, [this](bool on) { m_twoPoints = on; });
    rh->addWidget(two);
    rh->addStretch();
    m_sub->addWidget(rot);
    // A stack wants the width of its widest page, shown or not: only the shown page counts, and
    // a narrow video clips it rather than widening the window.
    m_sub->setMinimumWidth(1);
    connect(m_sub, &QStackedWidget::currentChanged, m_sub, [sub = m_sub](int shown) {
        for (int i = 0; i < sub->count(); ++i)
            sub->widget(i)->setSizePolicy(i == shown ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                                          QSizePolicy::Preferred);
    });
    emit m_sub->currentChanged(0);
    m_sub->hide(); // the pointer has no options
    h->addStretch();
    m_invert = invert;
    return row;
}

QWidget *VisualTools::options() const { return m_sub; }

void VisualTools::setTool(int t) {
    m_tool = t;
    m_vecSel.clear();
    if (t < int(m_buttons.size())) m_buttons[t]->setChecked(true);
    if (m_sub) {
        m_sub->setCurrentIndex(t == VClip || t == Draw || t == RClip ? 1 : t == Pos || t == Move ? 2 : t == Shifter ? 3 : t == AllTags ? 4 : t == Scale ? 5 : t == RotZ ? 6 : 0);
        m_sub->setVisible(m_sub->currentIndex() != 0);
        m_invert->setVisible(t == VClip || t == RClip);
        for (auto *b : m_sub->widget(1)->findChildren<QToolButton *>())
            if (b != m_invert) b->setVisible(t != RClip);
        m_shapeBox->setVisible(t != RClip);
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
    const auto r = tab->s().row(size_t(row));
    m_first = r.comment ? 0 : m_box->frameAtStart(r.start);
    m_last = r.comment ? -1 : m_box->frameAtEnd(r.end);
    if (m_drag) return; // keep the vector and its frame stable while dragging
    if (row != m_vecRow) m_vecSel.clear();
    m_vecRow = row;
    if (m_tool == VClip)
        m_vec = m_v.clip == 2   ? Drawing::parse(fs::q(m_v.clip_drawing))
                : m_v.clip == 1 ? Drawing::parse(QString("m %1 %2 l %3 %2 %3 %4 %1 %4") // a rectangle's corners (Aegisub)
                                                     .arg(n2(m_v.clip_x1), n2(m_v.clip_y1), n2(m_v.clip_x2), n2(m_v.clip_y2)))
                                : Drawing{};
    if (m_tool == Draw) {
        // m_vec holds the points as offsets from \pos; libass also shifts them by the alignment
        m_vec = Drawing::parse(fs::q(m_v.drawing));
        const double k = std::pow(2.0, -(std::max(1, m_v.drawing_scale) - 1));
        m_drawScale = QPointF(m_v.fscx / 100 * k, m_v.fscy / 100 * k);
        m_drawOrigin = QPointF(m_v.x, m_v.y);
        m_vec.translate(-alignShift(m_vec));
    }
    std::erase_if(m_vecSel, [this](std::pair<int, int> h) {
        return h.first >= int(m_vec.cmds.size()) || h.second >= int(m_vec.cmds[size_t(h.first)].pts.size());
    });
}

QPointF VisualTools::toVec(QPointF sp) const {
    return m_tool == VClip ? sp * std::pow(2.0, std::max(1, m_v.clip_scale) - 1) : scriptToDraw(sp);
}

std::vector<std::pair<size_t, QPointF>> VisualTools::others() const {
    std::vector<std::pair<size_t, QPointF>> out;
    auto *tab = m_box->tab();
    if (m_tool != Pos || !m_box->hasVideo()) return out;
    for (size_t r : tab->s().rows_at(m_box->time()))
        if (int(r) != tab->active() && tab->isSelected(r)) {
            const auto v = tab->s().visual(r);
            out.emplace_back(r, w({v.x, v.y}));
        }
    return out;
}

bool VisualTools::shown() const {
    return m_valid && m_box->frame() >= m_first && m_box->frame() <= m_last;
}

void VisualTools::grabLines() {
    auto *tab = m_box->tab();
    m_rows = tab->selection();
    if (std::find(m_rows.begin(), m_rows.end(), size_t(tab->active())) == m_rows.end()) m_rows = {size_t(tab->active())};
    m_starts.clear();
    for (size_t r : m_rows) m_starts.push_back(tab->s().visual(r));
}

// Aegisub's drag tool moves a line whole: \pos (a line without one gets it), both \move points and
// its \org, so a rotated line keeps its shape.
void VisualTools::moveLines(QPointF d) {
    QStringList tags;
    for (const auto &v : m_starts) {
        const QString at = QString("%1,%2").arg(n2(v.x + d.x()), n2(v.y + d.y()));
        QString t = v.has_move ? QString("\\move(%1,%2,%3").arg(at, n2(v.x2 + d.x()), n2(v.y2 + d.y())) +
                                     (v.t2 > v.t1 ? QString(",%1,%2").arg(v.t1).arg(v.t2) : QString()) + ")"
                               : QString("\\pos(%1)").arg(at);
        if (v.has_org) t += QString("\\org(%1,%2)").arg(n2(v.org_x + d.x()), n2(v.org_y + d.y()));
        tags << t;
    }
    auto *tab = m_box->tab();
    const auto t = fs::rvec(tags);
    tab->s().set_each_tags(fs::slice(m_rows), {t.data(), t.size()}, fs::Utf8(m_amend));
    tab->changed(TabPanel::FromVideo);
}

// libass puts drawing point (0,0) at \pos minus the alignment share of the points' bounding box
// (measured: \an7 keeps it at \pos, \an2 moves it left by width/2 and up by height).
QPointF VisualTools::alignShift(const Drawing &d) const {
    const QRectF b = d.bounds();
    const int an = m_v.align;
    return {b.width() * (an % 3 == 1 ? 0 : an % 3 == 2 ? 0.5 : 1), b.height() * (an >= 7 ? 0 : an >= 4 ? 0.5 : 1)};
}

void VisualTools::apply(const QString &tags, bool allSelected) {
    auto *tab = m_box->tab();
    std::vector<size_t> rows = allSelected ? tab->selection() : std::vector<size_t>{size_t(tab->active())};
    tab->s().set_tags(fs::slice(rows), fs::Utf8(tags), fs::Utf8(m_amend));
    tab->changed(TabPanel::FromVideo);
}

// A \move's times, from the line's start: none means the whole line (libass)
static std::pair<qint64, qint64> moveTimes(const fsb::Visual &v, const fsb::Row &r) {
    if (v.t1 <= 0 && v.t2 <= 0) return {0, r.end - r.start};
    return {v.t1, v.t2};
}

void VisualTools::toggleMove() {
    auto *tab = m_box->tab();
    auto &s = tab->s();
    const auto rows = tab->selection();
    QStringList tags;
    for (size_t r : rows) {
        const fsb::Visual v = s.visual(r);
        const fsb::Row row = s.row(r);
        tags << (v.has_move ? QString("\\pos(%1,%2)").arg(n2(v.x), n2(v.y))
                            : QString("\\move(%1,%2,%1,%2,0,%3)").arg(n2(v.x), n2(v.y)).arg(row.end - row.start));
    }
    const auto t = fs::rvec(tags);
    s.set_each_tags(fs::slice(rows), {t.data(), t.size()}, fs::Utf8(QString("visual-toggle-move-%1").arg(++m_dragId)));
    tab->changed(TabPanel::FromVideo);
    reload();
}

void VisualTools::applyAllTags(double value) {
    auto *tab = m_box->tab();
    tab->s().adjust_tag(fs::slice(m_tagRows), {m_tagBase.data(), m_tagBase.size()},
                        fs::Utf8(m_tagName->currentData().toString()), uint8_t(m_tagMode->currentIndex()), value,
                        fs::Utf8(m_amend));
    tab->changed(TabPanel::FromVideo);
}

// The active line to p, the other selected lines along by as much (Aegisub's double-click)
void VisualTools::applyPos(QPointF p) {
    grabLines();
    moveLines(p - QPointF(m_v.x, m_v.y));
}

QString VisualTools::clipTag() const {
    const QString name = m_v.inverse ? "iclip" : "clip";
    if (m_v.clip == 1)
        return QString("\\%1(%2,%3,%4,%5)").arg(name).arg(qRound(m_v.clip_x1)).arg(qRound(m_v.clip_y1))
            .arg(qRound(m_v.clip_x2)).arg(qRound(m_v.clip_y2));
    const QString scale = m_v.clip_scale > 1 ? QString::number(m_v.clip_scale) + "," : QString();
    return QString("\\%1(%2%3)").arg(name, scale, m_vec.toString());
}

void VisualTools::writeRectClip() {
    const QString tag = clipTag(), rect = tag.mid(tag.indexOf('('));
    QStringList tags;
    for (const auto &v : m_starts) tags << (v.inverse ? "\\iclip" : "\\clip") + rect;
    const auto t = fs::rvec(tags);
    auto *tab = m_box->tab();
    tab->s().set_each_tags(fs::slice(m_rows), {t.data(), t.size()}, fs::Utf8(m_amend));
    tab->changed(TabPanel::FromVideo);
}

void VisualTools::writeVector() {
    auto *tab = m_box->tab();
    if (m_tool == VClip) {
        if (m_v.clip != 2) m_v.clip_scale = 1;
        m_v.clip = 2;
        apply(clipTag(), true);
    } else {
        Drawing out = m_vec;
        out.translate(alignShift(out));
        tab->s().set_drawing(size_t(tab->active()), fs::Utf8(out.toString()), fs::Utf8(m_amend));
        tab->changed(TabPanel::FromVideo);
    }
}

QPointF VisualTools::vecW(QPointF pt) const {
    if (m_tool == VClip) return w(pt / std::pow(2.0, std::max(1, m_v.clip_scale) - 1));
    return w(drawToScript(pt));
}

namespace {
// p turned by deg around o the way libass applies \frz (counterclockwise on screen)
QPointF turn(QPointF p, QPointF o, double deg) {
    if (deg == 0) return p;
    const double a = qDegreesToRadians(deg), c = std::cos(a), s = std::sin(a);
    const QPointF d = p - o;
    return o + QPointF(d.x() * c + d.y() * s, -d.x() * s + d.y() * c);
}
} // namespace

// libass aligns the unturned drawing at \pos, then turns it around \org
// shortcut: \frx, \fry and \fax/\fay are ignored for drawings; add the 3D transform if people edit tilted shapes.
QPointF VisualTools::drawToScript(QPointF pt) const {
    const QPointF s = m_drawOrigin + QPointF(pt.x() * m_drawScale.x(), pt.y() * m_drawScale.y());
    return turn(s, {m_v.org_x, m_v.org_y}, m_v.frz);
}

QPointF VisualTools::scriptToDraw(QPointF s) const {
    const QPointF d = turn(s, {m_v.org_x, m_v.org_y}, -m_v.frz) - m_drawOrigin;
    return {d.x() / m_drawScale.x(), d.y() / m_drawScale.y()};
}

int VisualTools::hitHandle(QPointF wp) const {
    const auto hs = m_vec.handles();
    for (int i = int(hs.size()) - 1; i >= 0; --i)
        if (near(vecW(m_vec.cmds[hs[i].first].pts[hs[i].second]), wp)) return i;
    return -1;
}

int VisualTools::hitSegment(QPointF wp, double &t) const {
    int best = -1;
    double bestD = 7;
    for (int i = 1; i < int(m_vec.cmds.size()); ++i) {
        const auto &c = m_vec.cmds[i];
        if ((c.type != 'l' && c.type != 'b') || m_vec.cmds[i - 1].pts.empty()) continue;
        const QPointF p0 = m_vec.cmds[i - 1].pts.back();
        // sample the segment; plenty for picking
        for (int k = 0; k <= 32; ++k) {
            const double u = k / 32.0, v = 1 - u;
            const QPointF p = c.type == 'l' ? p0 * v + c.pts[0] * u
                                            : p0 * (v * v * v) + c.pts[0] * (3 * v * v * u) + c.pts[1] * (3 * v * u * u) +
                                                  c.pts[2] * (u * u * u);
            const double d = QLineF(vecW(p), wp).length();
            if (d < bestD) bestD = d, best = i, t = u;
        }
    }
    return best;
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
    // the other selected lines on this frame (Kainote), a click makes one the active line (Aegisub)
    const auto oth = others();
    for (const auto &o : oth) handle(p, o.second, true, kLine);
    if (!shown()) {
        // why the tool does nothing (Kainote)
        if (m_valid && oth.empty()) {
            const bool comment = m_box->tab()->s().row(size_t(m_box->tab()->active())).comment;
            const QString t = comment ? tr("The visual tools do not work on comments")
                                      : tr("The line is not visible on this frame\nor lasts no time");
            p.setPen(kShadow);
            p.drawText(d.translated(1, 1), Qt::AlignCenter, t);
            p.setPen(QColor(255, 90, 90));
            p.drawText(d, Qt::AlignCenter, t);
        }
        return;
    }
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
        if (m_v.has_move) { // where the line is on this frame (Kainote)
            const auto r = m_box->tab()->s().row(size_t(m_box->tab()->active()));
            const auto [t1, t2] = moveTimes(m_v, r);
            const double k = std::clamp(double(m_box->time() - r.start - t1) / double(std::max<qint64>(1, t2 - t1)), 0.0, 1.0);
            p.setPen(Qt::NoPen);
            p.setBrush(kAccent);
            p.drawEllipse(pos + (end - pos) * k, 3, 3);
            p.setBrush(Qt::NoBrush);
        }
        break;
    }
    case Scale: {
        // along the line's own axes, turned by rz (Aegisub)
        // shortcut: rx and ry are left out, the axes stay flat; Aegisub turns them in 3D
        const double a = qDegreesToRadians(m_v.frz);
        const QPointF ax(std::cos(a), -std::sin(a)), ay(std::sin(a), std::cos(a));
        const QPointF ex = pos + ax * m_v.fscx, ey = pos - ay * m_v.fscy;
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
        if (m_drag == 4) {
            line(p, m_pressW, m_mouseW, kAccent);
            handle(p, m_pressW, false, kAccent);
            handle(p, m_mouseW, false, kAccent);
        }
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
        const QPointF org = w({m_v.org_x, m_v.org_y});
        line(p, org, pos, QColor(255, 255, 255, 100), Qt::DotLine);
        handle(p, pos, true);
        handle(p, org, false, kAccent);
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
        auto map = [this](QPointF pt) { return vecW(pt); };
        QPainterPath path;
        QPointF last;
        bool started = false; // libass starts a contour at its first segment, not at the m point
        for (size_t i = 0; i < m_vec.cmds.size(); ++i) {
            const auto &c = m_vec.cmds[i];
            if (c.type == 's' && !c.pts.empty()) {
                // libass: a uniform cubic B-spline over the previous point, the s points, any p
                // extensions and, after c, the first three again
                QPointF prev = c.pts[0];
                for (size_t k = i; k-- > 0;)
                    if (!m_vec.cmds[k].pts.empty()) {
                        prev = m_vec.cmds[k].pts.back();
                        break;
                    }
                std::vector<QPointF> ctl{prev};
                ctl.insert(ctl.end(), c.pts.begin(), c.pts.end());
                size_t j = i + 1;
                for (; j < m_vec.cmds.size() && m_vec.cmds[j].type == 'p'; ++j)
                    ctl.insert(ctl.end(), m_vec.cmds[j].pts.begin(), m_vec.cmds[j].pts.end());
                if (j < m_vec.cmds.size() && m_vec.cmds[j].type == 'c' && ctl.size() >= 3)
                    ctl.insert(ctl.end(), {ctl[0], ctl[1], ctl[2]});
                for (size_t k = 0; k + 3 < ctl.size(); ++k) {
                    const QPointF a = ctl[k], b = ctl[k + 1], cc = ctl[k + 2], d4 = ctl[k + 3];
                    if (started) path.lineTo(map((a + 4 * b + cc) / 6));
                    else path.moveTo(map((a + 4 * b + cc) / 6)), started = true;
                    path.cubicTo(map((2 * b + cc) / 3), map((b + 2 * cc) / 3), map((b + 4 * cc + d4) / 6));
                }
                i = j - 1;
                continue;
            }
            if (c.pts.empty()) continue;
            started = c.type != 'm' && c.type != 'n';
            if (!started) path.moveTo(map(c.pts[0]));
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
            if (c.type == 's' || c.type == 'p')
                for (const auto &pt : c.pts) handle(p, map(pt), true);
            if (!c.pts.empty()) {
                last = map(c.pts.back());
                handle(p, last, false, c.type == 'm' ? kAccent : kHandle);
            }
        }
        if (m_tool == Draw) handle(p, pos, true, kAccent);
        p.setPen(Qt::NoPen);
        p.setBrush(kAccent);
        for (const auto &h : m_vecSel) p.drawRect(QRectF(map(m_vec.at(h)) - QPointF(3, 3), QSizeF(6, 6)));
        p.setBrush(Qt::NoBrush);
        if (m_drag == 7) {
            p.setPen(QPen(kLine, 1, Qt::DashLine));
            p.drawRect(QRectF(m_pressW, m_mouseW).normalized());
        }
        break;
    }
    }
}

// ---- mouse --------------------------------------------------------------------

bool VisualTools::press(QMouseEvent *e) {
    m_mouseW = e->position();
    auto *tab = m_box->tab();
    if (m_tool == Pointer) {
        if (shown() && (e->button() == Qt::MiddleButton ||
                        (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)))) {
            m_amend = QString("visual-%1").arg(++m_dragId);
            applyPos(s(e->position()));
        }
        return true;
    }
    if (m_tool == Pos && e->button() == Qt::LeftButton && !(shown() && near(w({m_v.x, m_v.y}), e->position())))
        for (const auto &[r, at] : others())
            if (near(at, e->position())) {
                tab->setActive(int(r), false, false); // reloads m_v; the video stays on this frame
                break;
            }
    if (!shown()) return false;
    m_button = e->button();
    m_pressW = e->position();
    m_pressS = s(m_pressW);
    m_start = m_v;
    m_amend = QString("visual-%1").arg(++m_dragId);
    m_drag = 1;
    switch (m_tool) {
    case Pos: grabLines(); break;
    case Move:
        if (m_button == Qt::RightButton) m_drag = 2;
        break;
    case AllTags:
        m_tagRows = tab->selection();
        m_tagBase.clear();
        for (size_t r : m_tagRows) m_tagBase.push_back(tab->s().row(r).text);
        m_tagStart = m_tagMode->currentIndex() == 1 ? m_tagValue->value() : 0;
        break;
    case RotZ:
    case RotXY:
        if (near(w({m_v.org_x, m_v.org_y}), m_pressW, 9)) {
            m_drag = 2;
            grabLines();
        } else if (m_tool == RotZ && m_twoPoints) m_drag = 4;
        break;
    case RClip:
        grabLines();
        if (m_v.clip == 1) {
            // a corner, else an edge, else inside moves the whole rectangle; outside starts a new one (Kainote)
            const QRectF r = QRectF(w({m_v.clip_x1, m_v.clip_y1}), w({m_v.clip_x2, m_v.clip_y2})).normalized();
            const QPointF cs[] = {w({m_v.clip_x1, m_v.clip_y1}), w({m_v.clip_x2, m_v.clip_y1}),
                                  w({m_v.clip_x1, m_v.clip_y2}), w({m_v.clip_x2, m_v.clip_y2})};
            const QPointF at = m_pressW;
            const bool inX = at.x() > r.left() - 5 && at.x() < r.right() + 5;
            const bool inY = at.y() > r.top() - 5 && at.y() < r.bottom() + 5;
            const double ex[] = {cs[0].x(), cs[1].x()}, ey[] = {cs[0].y(), cs[2].y()};
            for (int k = 0; k < 4; ++k)
                if (near(cs[k], at)) m_drag = 10 + k;
            for (int k = 0; k < 2 && m_drag == 1; ++k)
                if (inY && std::abs(at.x() - ex[k]) <= 5) m_drag = 20 + k; // the x1 or x2 edge
                else if (inX && std::abs(at.y() - ey[k]) <= 5) m_drag = 22 + k; // y1 or y2
            if (m_drag == 1 && r.contains(at)) m_drag = 30;
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
            pt = scriptToDraw(m_pressS);
        }
        if (m_vecMode == VecDelete) {
            m_drag = 0;
            if (h < 0) return false;
            m_vec.cmds.erase(m_vec.cmds.begin() + hs[h].first);
            if (!m_vec.cmds.empty() && m_vec.cmds[0].type != 'm') m_vec.cmds[0] = {'m', {m_vec.cmds[0].pts.back()}};
            writeVector();
            return true;
        }
        if (m_vecMode == VecMove && e->button() == Qt::LeftButton && !m_vec.cmds.empty()) {
            // Aegisub: Ctrl+click picks a point more or less, a point dragged takes the picked ones
            // along, a box dragged from nowhere picks the points in it
            const bool ctrl = e->modifiers() & Qt::ControlModifier;
            if (h >= 0 && ctrl) {
                if (!m_vecSel.erase(hs[h])) m_vecSel.insert(hs[h]);
                m_drag = 0;
                return true;
            }
            if (h >= 0) {
                if (!m_vecSel.contains(hs[h])) m_vecSel = {hs[h]};
                m_vecHandle = hs[h];
                m_vecStart.clear();
                for (const auto &s : m_vecSel) m_vecStart.emplace_back(s, m_vec.at(s));
                return true;
            }
            if (!ctrl) m_vecSel.clear();
            m_drag = 7;
            return true;
        }
        if (h >= 0 && (m_vecMode == VecMove || m_vecMode >= VecFreehand || e->button() != Qt::LeftButton)) {
            m_vecHandle = hs[h];
            return true;
        }
        if (m_vecMode == VecMove && !m_vec.cmds.empty()) {
            m_drag = 0; // a miss adds nothing; only an empty shape starts with a line, as Aegisub opens it
            return true;
        }
        if (m_vecMode == VecInsert || m_vecMode == VecConvert) {
            // Aegisub's vector clip insert / convert modes act on the clicked segment
            double t = 0;
            const int i = hitSegment(m_pressW, t);
            if (i < 0) {
                m_drag = 0;
                return false;
            }
            auto &c = m_vec.cmds[i];
            const QPointF p0 = m_vec.cmds[i - 1].pts.back();
            if (m_vecMode == VecConvert) {
                const QPointF end = c.pts.back();
                c = c.type == 'l' ? Drawing::Cmd{'b', {p0 + (end - p0) / 3, p0 + (end - p0) * 2 / 3, end}}
                                  : Drawing::Cmd{'l', {end}};
                m_drag = 0;
            } else if (c.type == 'l') {
                m_vec.cmds.insert(m_vec.cmds.begin() + i, {'l', {p0 + (c.pts[0] - p0) * t}});
                m_vecHandle = {i, 0};
            } else {
                // de Casteljau split at t
                auto lerp = [t](QPointF a, QPointF b) { return a + (b - a) * t; };
                const QPointF a = lerp(p0, c.pts[0]), b = lerp(c.pts[0], c.pts[1]), d = lerp(c.pts[1], c.pts[2]);
                const QPointF ab = lerp(a, b), bd = lerp(b, d), mid = lerp(ab, bd);
                const QPointF end = c.pts[2];
                c = {'b', {bd, d, end}};
                m_vec.cmds.insert(m_vec.cmds.begin() + i, {'b', {a, ab, mid}});
                m_vecHandle = {i, 2};
            }
            writeVector();
            return true;
        }
        if (m_vecMode == VecFreehand && e->button() == Qt::LeftButton) {
            m_vec.cmds.push_back({'m', {pt}});
            m_drag = 6;
            writeVector();
            return true;
        }
        if (m_shapeBox->currentIndex() > 0 && e->button() == Qt::LeftButton) {
            m_shapeFrom = m_vec.cmds.size();
            m_shapeA = pt;
            m_drag = 5;
            return true;
        }
        if (m_vec.cmds.empty() || m_vecMode == VecNewFigure) {
            m_vec.cmds.push_back({'m', {pt}});
        } else if (m_vecMode == VecSpline) {
            // Kainote's spline mode: extend the spline being drawn, or start one as a straight
            // three-point spline so libass always gets a valid s command
            if (m_vec.cmds.back().type == 's') {
                m_vec.cmds.back().pts.push_back(pt);
            } else if (m_vec.cmds.back().type == 'p') {
                m_vec.cmds.push_back({'p', {pt}});
            } else {
                const QPointF last = m_vec.last();
                m_vec.cmds.push_back({'s', {last + (pt - last) / 3, last + (pt - last) * 2 / 3, pt}});
            }
        } else if (m_vecMode == VecBezier) {
            const QPointF last = m_vec.last();
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
    QPointF delta = now - m_pressS, dw = e->position() - m_pressW;
    if (e->modifiers() & Qt::ShiftModifier) {
        // constrain to the dominant axis
        if (std::abs(delta.x()) > std::abs(delta.y())) delta.setY(0);
        else delta.setX(0);
        if (std::abs(dw.x()) > std::abs(dw.y())) dw.setY(0);
        else dw.setX(0);
    }
    auto *tab = m_box->tab();
    switch (m_tool) {
    case Pos:
        if (m_drag == 1 && delta.isNull()) break; // a click alone writes nothing
        m_drag = 3;
        moveLines(delta); // every selected line by the same delta
        break;
    case Move: {
        QPointF a(m_start.x, m_start.y), b(m_start.x2, m_start.y2);
        if (m_drag == 2) b = m_start.has_move || near(w(b), m_pressW, 9) ? b + delta : now;
        else a += delta;
        QString t = QString("\\move(%1,%2,%3,%4").arg(n2(a.x()), n2(a.y()), n2(b.x()), n2(b.y()));
        qint64 t1 = m_start.t1, t2 = m_start.t2;
        if (m_start.has_move) {
            // the end dragged is where the line is at this frame: its time is the frame's (Aegisub)
            const auto r = tab->s().row(size_t(tab->active()));
            std::tie(t1, t2) = moveTimes(m_start, r);
            (m_drag == 2 ? t2 : t1) = m_box->time() - r.start;
        }
        if (t2 > t1) t += QString(",%1,%2").arg(t1).arg(t2);
        apply(t + ")");
        break;
    }
    case Scale: {
        double sx = m_start.fscx, sy = m_start.fscy;
        // the move along the line's turned axes, as the handles are drawn
        const double a = qDegreesToRadians(m_start.frz);
        dw = {dw.x() * std::cos(a) - dw.y() * std::sin(a), dw.x() * std::sin(a) + dw.y() * std::cos(a)};
        if (m_button == Qt::LeftButton) {
            if (m_scaleWhat & 2) { // keep proportions: one factor from the larger move
                const double d = std::abs(dw.x()) >= std::abs(dw.y()) ? dw.x() : -dw.y();
                const double f = std::max(0.0, 1 + d / std::max(1.0, (m_scaleWhat & 1) ? sx : sy));
                sx *= f;
                sy *= f;
            } else {
                if (m_scaleWhat & 1) sx = std::max(0.0, sx + dw.x());
                if (m_scaleWhat & 4) sy = std::max(0.0, sy - dw.y());
            }
        } else if (m_button == Qt::RightButton) sy = std::max(0.0, sy - dw.y());
        else {
            const double f = std::max(0.0, 1 + dw.x() / 100);
            sx *= f;
            sy *= f;
        }
        // whole percents, Ctrl in steps of 25 (Aegisub)
        const double step = e->modifiers() & Qt::ControlModifier ? 25 : 1;
        sx = std::round(sx / step) * step;
        sy = std::round(sy / step) * step;
        apply(QString("\\fscx%1\\fscy%2").arg(sx).arg(sy), true);
        break;
    }
    case RotZ:
    case RotXY: {
        if (m_drag == 2) {
            // every selected line's origin by as much (Aegisub)
            QStringList tags;
            for (const auto &v : m_starts) tags << QString("\\org(%1,%2)").arg(n2(v.org_x + delta.x()), n2(v.org_y + delta.y()));
            const auto t = fs::rvec(tags);
            tab->s().set_each_tags(fs::slice(m_rows), {t.data(), t.size()}, fs::Utf8(m_amend));
            tab->changed(TabPanel::FromVideo);
            break;
        }
        // Ctrl snaps to 30 degrees (Aegisub)
        const auto angle = [e](double a) {
            if (e->modifiers() & Qt::ControlModifier) a = std::round(a / 30) * 30;
            return std::fmod(std::fmod(a, 360) + 360, 360);
        };
        if (m_tool == RotXY) {
            double frx = m_start.frx, fry = m_start.fry;
            // two degrees a pixel (Aegisub)
            if (m_button != Qt::RightButton) fry = angle(m_start.fry + dw.x() * 2);
            if (m_button != Qt::LeftButton) frx = angle(m_start.frx - dw.y() * 2);
            apply(QString("\\frx%1\\fry%2").arg(n2(frx), n2(fry)), true);
            break;
        }
        if (m_drag == 4) {
            const QPointF d = now - m_pressS;
            if (QLineF(m_pressW, e->position()).length() < 4) break;
            apply(QString("\\frz%1").arg(n2(angle(qRadiansToDegrees(std::atan2(-d.y(), d.x()))))), true);
            m_box->view()->updateOverlay();
            break;
        }
        const QPointF c = w({m_start.org_x, m_start.org_y});
        const double a0 = std::atan2(-(m_pressW.y() - c.y()), m_pressW.x() - c.x());
        const double a1 = std::atan2(-(e->position().y() - c.y()), e->position().x() - c.x());
        apply(QString("\\frz%1").arg(n2(angle(m_start.frz + qRadiansToDegrees(a1 - a0)))), true);
        break;
    }
    case RClip: {
        if (m_drag == 1) {
            m_v.clip = 1;
            m_v.clip_x1 = std::min(m_pressS.x(), now.x());
            m_v.clip_y1 = std::min(m_pressS.y(), now.y());
            m_v.clip_x2 = std::max(m_pressS.x(), now.x());
            m_v.clip_y2 = std::max(m_pressS.y(), now.y());
        } else if (m_drag == 30) {
            m_v.clip_x1 = m_start.clip_x1 + delta.x();
            m_v.clip_x2 = m_start.clip_x2 + delta.x();
            m_v.clip_y1 = m_start.clip_y1 + delta.y();
            m_v.clip_y2 = m_start.clip_y2 + delta.y();
        } else if (m_drag >= 20) {
            const int k = m_drag - 20;
            if (k < 2) (k ? m_v.clip_x2 : m_v.clip_x1) = (k ? m_start.clip_x2 : m_start.clip_x1) + delta.x();
            else (k == 3 ? m_v.clip_y2 : m_v.clip_y1) = (k == 3 ? m_start.clip_y2 : m_start.clip_y1) + delta.y();
        } else {
            const int k = m_drag - 10;
            (k % 2 ? m_v.clip_x2 : m_v.clip_x1) = (k % 2 ? m_start.clip_x2 : m_start.clip_x1) + delta.x();
            (k / 2 ? m_v.clip_y2 : m_v.clip_y1) = (k / 2 ? m_start.clip_y2 : m_start.clip_y1) + delta.y();
        }
        writeRectClip();
        break;
    }
    case VClip:
    case Draw: {
        if (m_vecHandle.first < 0 && m_drag != 5 && m_drag != 6) break;
        if (m_vecStart.size() > 1) { // the picked points by as much
            const QPointF by = toVec(m_pressS + delta) - toVec(m_pressS);
            for (const auto &[h, at] : m_vecStart) m_vec.at(h) = at + by;
            writeVector();
            break;
        }
        QPointF pt = m_drag == 5 ? now : m_pressS + delta; // Shift keeps a point on one axis
        if (m_tool == VClip) pt *= std::pow(2.0, std::max(1, m_v.clip_scale) - 1);
        else pt = scriptToDraw(now);
        if (m_drag == 5) {
            // the picked shape fitted into the dragged rectangle
            QPointF d = pt - m_shapeA;
            if (e->modifiers() & Qt::ShiftModifier) {
                const double m = std::max(std::abs(d.x()), std::abs(d.y()));
                d = QPointF(d.x() < 0 ? -m : m, d.y() < 0 ? -m : m);
            }
            m_vec.cmds.resize(m_shapeFrom);
            Drawing shape = Drawing::parse(m_shapeBox->currentData().toString());
            for (auto &c : shape.cmds)
                for (QPointF &q : c.pts) q = m_shapeA + QPointF(q.x() / 100 * d.x(), q.y() / 100 * d.y());
            m_vec.cmds.insert(m_vec.cmds.end(), shape.cmds.begin(), shape.cmds.end());
            writeVector();
            break;
        }
        if (m_drag == 6) {
            // freehand: a new line point every 30 screen pixels (Aegisub's freehand mode)
            if (QLineF(vecW(m_vec.last()), e->position()).length() >= 30) {
                m_vec.cmds.push_back({'l', {pt}});
                writeVector();
            }
            break;
        }
        // Alt snaps to another point
        if (e->modifiers() & Qt::AltModifier)
            for (const auto &h : m_vec.handles())
                if (h != m_vecHandle && QLineF(m_vec.at(h), pt).length() < 6 / m_box->view()->scriptScale())
                    pt = m_vec.at(h);
        m_vec.at(m_vecHandle) = pt;
        writeVector();
        break;
    }
    case AllTags: {
        // value per pixel dragged, scaled to the tag's usual range
        const QString t = m_tagName->currentData().toString();
        const double step = t.endsWith('a')                            ? 1    // alpha, 1a, 3a, 4a
                            : t.startsWith("fa")                       ? 0.002
                            : t == "fsp"                               ? 0.1
                            : t.startsWith("fs") || t.startsWith("fr") ? 0.5
                                                                       : 0.05;
        const double v = std::round((m_tagStart + dw.x() * step) * 100) / 100;
        const QSignalBlocker block(m_tagValue);
        m_tagValue->setValue(v);
        applyAllTags(v);
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
        tab->s().shift_positions(fs::slice(tab->selection()), step.x(), step.y(), uint8_t(m_shiftWhat),
                                   fs::Utf8(m_amend));
        tab->changed(TabPanel::FromVideo);
        break;
    }
    }
    return true;
}

bool VisualTools::release(QMouseEvent *e) {
    if (!m_drag) return m_tool == Pointer;
    if (m_drag == 7) { // the selection box
        const QRectF box = QRectF(m_pressW, e->position()).normalized();
        for (const auto &h : m_vec.handles())
            if (box.contains(vecW(m_vec.at(h)))) m_vecSel.insert(h);
    }
    m_drag = 0;
    m_vecHandle = {-1, -1};
    m_vecStart.clear();
    m_button = Qt::NoButton;
    reload();
    return true;
}

bool VisualTools::vecKey(QKeyEvent *e) {
    if (e->matches(QKeySequence::SelectAll)) {
        for (const auto &h : m_vec.handles()) m_vecSel.insert(h);
        return true;
    }
    if (m_vecSel.empty()) return false;
    if (e->key() == Qt::Key_Delete) {
        // their commands go, as the delete mode's click; the first one left starts the shape
        std::set<int> cmds;
        for (const auto &h : m_vecSel) cmds.insert(h.first);
        for (auto it = cmds.rbegin(); it != cmds.rend(); ++it) m_vec.cmds.erase(m_vec.cmds.begin() + *it);
        if (!m_vec.cmds.empty() && m_vec.cmds[0].type != 'm') m_vec.cmds[0] = {'m', {m_vec.cmds[0].pts.back()}};
        m_vecSel.clear();
        m_amend = QString("visual-%1").arg(++m_dragId);
        writeVector();
        return true;
    }
    // WASD by a pixel of the script, Shift a tenth (Kainote)
    const double st = e->modifiers() & Qt::ShiftModifier ? 0.1 : 1;
    QPointF d;
    switch (e->key()) {
    case Qt::Key_W: d = {0, -st}; break;
    case Qt::Key_S: d = {0, st}; break;
    case Qt::Key_A: d = {-st, 0}; break;
    case Qt::Key_D: d = {st, 0}; break;
    default: return false;
    }
    const QPointF by = toVec(d) - toVec({});
    for (const auto &h : m_vecSel) m_vec.at(h) += by;
    m_amend = QString("visual-nudge-vec-%1").arg(m_box->tab()->active());
    writeVector();
    return true;
}

// Aegisub's drag tool: a double-click puts the active line there, the other selected lines go
// along by as much.
bool VisualTools::doubleClick(QMouseEvent *e) {
    if ((m_tool != Pos && m_tool != Move) || e->button() != Qt::LeftButton || !shown()) return false;
    m_amend = QString("visual-%1").arg(++m_dragId);
    applyPos(s(e->position()));
    reload();
    return true;
}

bool VisualTools::key(QKeyEvent *e) {
    if ((m_tool == VClip || m_tool == Draw) && shown()) return vecKey(e);
    const bool rect = m_tool == RClip && m_v.clip == 1;
    if (!shown() || (m_tool != Pos && m_tool != Move && !rect)) return false;
    const double st = e->modifiers() & Qt::ShiftModifier ? (rect ? 10 : 0.1) : 1;
    QPointF d;
    switch (e->key()) {
    case Qt::Key_W: d = {0, -st}; break;
    case Qt::Key_S: d = {0, st}; break;
    case Qt::Key_A: d = {-st, 0}; break;
    case Qt::Key_D: d = {st, 0}; break;
    default: return false;
    }
    m_amend = QString("visual-nudge-%1").arg(m_box->tab()->active());
    if (rect) {
        grabLines();
        m_v.clip_x1 += d.x(), m_v.clip_x2 += d.x(), m_v.clip_y1 += d.y(), m_v.clip_y2 += d.y();
        writeRectClip();
    } else if (m_tool == Pos) applyPos(QPointF(m_v.x, m_v.y) + d);
    else {
        QString t = QString("\\move(%1,%2,%3,%4").arg(n2(m_v.x + d.x()), n2(m_v.y + d.y()), n2(m_v.x2), n2(m_v.y2));
        if (m_v.t2 > m_v.t1) t += QString(",%1,%2").arg(m_v.t1).arg(m_v.t2);
        apply(t + ")");
    }
    reload();
    return true;
}

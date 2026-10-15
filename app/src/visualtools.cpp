#include "visualtools.h"

#include "editbox.h"
#include "icons.h"
#include "theme.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QApplication>
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
#include <QToolTip>
#include <QToolButton>
#include <QWheelEvent>
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
const QColor kShadow(0, 0, 0, 160);
QColor handleColor() { return theme::color(theme::VisualHandle); }
QColor lineColor() { return theme::color(theme::VisualLine); }
QColor accentColor() { return theme::color(theme::VisualAccent); }

QString n2(double v) {
    const double r = std::round(v * 100) / 100;
    return QString::number(r == 0 ? 0.0 : r, 'g', 12); // 12.5, not 12.50; 0, not -0
}

void handle(QPainter &p, QPointF c, bool round = false, QColor col = handleColor()) {
    p.setPen(QPen(kShadow, 3));
    const QRectF r(c.x() - 4, c.y() - 4, 8, 8);
    round ? p.drawEllipse(r) : p.drawRect(r);
    p.setPen(QPen(col, 1.5));
    round ? p.drawEllipse(r) : p.drawRect(r);
}

void line(QPainter &p, QPointF a, QPointF b, QColor c = lineColor(), Qt::PenStyle st = Qt::SolidLine) {
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
      m_scaleWhat(QSettings().value("visual/scaleWhat", 5).toInt()),
      m_rotWhat(QSettings().value("visual/rotWhat", 0).toInt()),
      m_posWhat(QSettings().value("visual/posWhat", 6).toInt()) {}

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
    {"toolAllTags", "typography", QT_TRANSLATE_NOOP("VisualTools", "All tags: sliders over the video; Shift+wheel picks the tag, right-drag moves the sliders")},
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
    m_posOptions = new QWidget(drag);
    auto *ph = new QHBoxLayout(m_posOptions);
    ph->setContentsMargins(6, 0, 0, 0);
    ph->setSpacing(1);
    dh->addWidget(m_posOptions);
    dh->addStretch();
    connect(conv, &QToolButton::clicked, this, &VisualTools::toggleMove);
    auto *track = new QToolButton(drag);
    track->setIcon(icon("focus-2", QColor(0x4aa3ff)));
    track->setToolTip(tr("Movement from 2 points and the video's time: click a point of the moving thing in the "
                         "video, seek a few frames on, click the same point again. The selected lines follow it "
                         "through their \\move times. Drag a point to correct it, right-click to start over."));
    track->setCheckable(true);
    track->setAutoRaise(true);
    connect(track, &QToolButton::toggled, this, [this](bool on) {
        m_track = on;
        m_trackPts = 0;
        m_box->view()->updateOverlay();
    });
    dh->addWidget(track);
    m_trackButton = track;
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
    // all tags (Kainote's Hydra): a tag of the user's list over the selected lines, its values on
    // sliders over the video
    auto *tags = new QWidget(m_sub);
    auto *th = new QHBoxLayout(tags);
    th->setContentsMargins(6, 0, 0, 0);
    m_tagName = new QComboBox(tags);
    m_tagName->setToolTip(tr("The tag; Shift+wheel on the video picks another"));
    m_tagMode = new QComboBox(tags);
    for (const char *m : hydra::modes()) m_tagMode->addItem(tr(m));
    m_tagMode->setToolTip(tr("How the value changes the selected lines: Insert puts it at the edit box's cursor "
                             "(around the selected tags for \\t) or the line's start, Step gives each line the "
                             "active line's value and the change times its place in the selection"));
    const auto fillTags = [this](int at) {
        const QSignalBlocker quiet(m_tagName);
        m_tagName->clear();
        for (const auto &t : m_hydra) m_tagName->addItem(QString("%1 (\\%2)").arg(t.name, t.tag));
        m_tagName->setCurrentIndex(std::clamp(at, 0, m_tagName->count() - 1));
    };
    m_hydra = hydra::load();
    const QString lastTag = QSettings().value("visual/allTagsTag", "blur").toString();
    const auto found = std::ranges::find(m_hydra, lastTag, &hydra::Tag::tag);
    fillTags(found == m_hydra.end() ? 0 : int(found - m_hydra.begin()));
    m_tagMode->setCurrentIndex(
        std::clamp(QSettings().value("visual/allTagsMode", htag().mode).toInt(), 0, m_tagMode->count() - 1));
    th->addWidget(m_tagName);
    th->addWidget(m_tagMode);
    for (auto *&v : m_tagValues) {
        v = new QDoubleSpinBox(tags);
        v->setRange(-100000, 100000);
        v->setToolTip(tr("Value; Enter applies it to the selected lines"));
        th->addWidget(v);
        connect(v, &QDoubleSpinBox::editingFinished, this, [this, v] {
            const int i = int(std::ranges::find(m_tagValues, v) - std::begin(m_tagValues));
            if (m_drag || !shown() || v->value() == m_hv[i]) return; // the focus left with nothing typed
            grabTexts();
            m_hv[i] = v->value();
            m_amend = QString("visual-tags-%1").arg(++m_dragId);
            applyHydra();
        });
    }
    auto *editTags = new QToolButton(tags);
    editTags->setIcon(icon("edit", QColor(0xf2665c)));
    editTags->setToolTip(tr("Edit the list of tags"));
    editTags->setAutoRaise(true);
    th->addWidget(editTags);
    th->addStretch();
    connect(m_tagName, &QComboBox::currentIndexChanged, this, [this](int k) {
        if (k < 0) return;
        QSettings().setValue("visual/allTagsTag", htag().tag);
        m_tagMode->setCurrentIndex(htag().mode);
        hydraValues();
        m_box->view()->updateOverlay();
    });
    connect(m_tagMode, &QComboBox::currentIndexChanged, this,
            [this](int k) { QSettings().setValue("visual/allTagsMode", k); });
    connect(editTags, &QToolButton::clicked, this, [this, fillTags] {
        HydraDialog dlg(m_box, m_hydra, m_tagName->currentIndex());
        if (dlg.exec() != QDialog::Accepted) return;
        m_hydra = dlg.tags();
        hydra::save(m_hydra);
        fillTags(dlg.current());
        emit m_tagName->currentIndexChanged(m_tagName->currentIndex());
    });
    m_sub->addWidget(tags);
    // Kainote's option toggles: bits of *bits, kept under key
    struct Option {
        int bit;
        const char *icon, *tip;
    };
    struct Rules {
        int forcing = 0, forced = 0; // while one of `forcing` is on, `forced` is on and stays so
        int needing = 0, needed = 0; // these work only while `needed` is on
        int oneOf = 0;               // one of these stays on
    };
    const auto options = [this](QWidget *page, QHBoxLayout *lay, int *bits, const char *key,
                                std::initializer_list<Option> list, Rules rules) {
        std::vector<std::pair<int, QToolButton *>> made;
        for (const Option &o : list) {
            auto *b = new QToolButton(page);
            b->setIcon(icon(o.icon, QColor(0x4aa3ff)));
            b->setToolTip(tr(o.tip));
            b->setCheckable(true);
            b->setAutoRaise(true);
            lay->addWidget(b);
            made.emplace_back(o.bit, b);
        }
        const auto sync = [this, bits, made, rules] {
            for (auto [bit, b] : made) {
                const QSignalBlocker quiet(b);
                b->setChecked(*bits & bit);
                b->setEnabled(!(bit == rules.forced && (*bits & rules.forcing)) &&
                              !((bit & rules.needing) && !(*bits & rules.needed)));
            }
            m_box->view()->updateOverlay(); // a rectangle turned on or off
        };
        for (auto [bit, b] : made)
            connect(b, &QToolButton::toggled, this, [=](bool on) {
                *bits = on ? *bits | bit : *bits & ~bit;
                if (*bits & rules.forcing) *bits |= rules.forced;
                if (rules.oneOf && !(*bits & rules.oneOf)) *bits |= rules.oneOf & ~bit;
                QSettings().setValue(key, *bits);
                sync();
            });
        sync();
    };
    // position tool (Kainote): fit the line into a rectangle drawn on the video
    options(m_posOptions, ph, &m_posWhat, "visual/posWhat",
            {{1, "frame", QT_TR_NOOP("Position by a rectangle: drag a rectangle on the video and the line goes into "
                                     "it by the chosen alignment, measured as libass draws it; the other selected lines "
                                     "move along. Drag its corners, edges or inside to change it")},
             {2, "arrow-autofit-width", QT_TR_NOOP("Position on the X axis")},
             {4, "arrow-autofit-height", QT_TR_NOOP("Position on the Y axis")}},
            {0, 0, 2 | 4, 1, 2 | 4});
    m_posAlign = new QComboBox(m_posOptions);
    for (const char *a : {QT_TR_NOOP("Left-bottom"), QT_TR_NOOP("Center-bottom"), QT_TR_NOOP("Right-bottom"),
                          QT_TR_NOOP("Left-middle"), QT_TR_NOOP("Center"), QT_TR_NOOP("Right-middle"),
                          QT_TR_NOOP("Left-top"), QT_TR_NOOP("Center-top"), QT_TR_NOOP("Right-top"),
                          QT_TR_NOOP("Left-below"), QT_TR_NOOP("Center-below"), QT_TR_NOOP("Right-below"),
                          QT_TR_NOOP("Left-above"), QT_TR_NOOP("Center-above"), QT_TR_NOOP("Right-above"),
                          QT_TR_NOOP("Before-top"), QT_TR_NOOP("Before-middle"), QT_TR_NOOP("Before-bottom"),
                          QT_TR_NOOP("After-top"), QT_TR_NOOP("After-middle"), QT_TR_NOOP("After-bottom")})
        m_posAlign->addItem(tr(a));
    m_posAlign->setToolTip(tr("Where the line goes against the rectangle"));
    m_posAlign->setCurrentIndex(std::clamp(QSettings().value("visual/posAlign", 4).toInt(), 0, 20));
    ph->addWidget(m_posAlign);
    connect(m_posAlign, &QComboBox::currentIndexChanged, this, [this](int k) {
        QSettings().setValue("visual/posAlign", k);
        if (m_tool != Pos || !(m_posWhat & 1) || !m_fitOn || !shown() || !measure()) return;
        grabLines();
        m_amend = QString("visual-fit-%1").arg(++m_dragId);
        fitPos();
        reload();
    });
    // scale tool (Kainote): which axes a left drag scales, and how far the change reaches
    auto *scale = new QWidget(m_sub);
    auto *ch = new QHBoxLayout(scale);
    ch->setContentsMargins(6, 0, 0, 0);
    ch->setSpacing(1);
    options(scale, ch, &m_scaleWhat, "visual/scaleWhat",
            {{1, "arrow-autofit-width", QT_TR_NOOP("Scale width")},
             {2, "link", QT_TR_NOOP("Keep proportions")},
             {4, "arrow-autofit-height", QT_TR_NOOP("Scale height")},
             {8, "stack-2", QT_TR_NOOP("Change all scale tags: every \\fscx and \\fscy of the selected lines, also "
                                       "inside \\t, by the same factor")},
             {16, "aspect-ratio",
              QT_TR_NOOP("Preserve proportions: the places of the selected lines and their clips are scaled around the "
                         "active line too, so text and vector drawings keep their positions against each other")},
             {64, "frame",
              QT_TR_NOOP("Scale by a rectangle: drag a rectangle on the video and the text, measured as libass draws "
                         "it, is scaled to fill it on the chosen axes, its borders kept as they are")},
             {32, "square",
              QT_TR_NOOP("Your own rectangle for the text at its present scale, dragged with the right mouse button, "
                         "where the measured one does not fit what the text should fill")}},
            {16 | 64, 8, 32, 64, 1 | 4});
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
    options(rot, rh, &m_rotWhat, "visual/rotWhat",
            {{1, "stack-2", QT_TR_NOOP("Change all Z rotation tags: every \\frz of the selected lines, also inside \\t, "
                                       "turns by the same angle")},
             {2, "aspect-ratio",
              QT_TR_NOOP("Preserve proportions: the places of the selected lines and their clips turn around the active "
                         "line's origin too, so text and vector drawings keep their positions against each other")}},
            {2, 1});
    rh->addStretch();
    m_sub->addWidget(rot);
    auto *rotXY = new QWidget(m_sub);
    auto *xh = new QHBoxLayout(rotXY);
    xh->setContentsMargins(6, 0, 0, 0);
    options(rotXY, xh, &m_rotWhat, "visual/rotWhat",
            {{4, "stack-2", QT_TR_NOOP("Change all X/Y rotation tags: every \\frx and \\fry of the selected lines, also "
                                       "inside \\t, turns by the same angle")}},
            {});
    xh->addStretch();
    m_sub->addWidget(rotXY);
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
        m_sub->setCurrentIndex(t == VClip || t == Draw || t == RClip ? 1 : t == Pos || t == Move ? 2 : t == Shifter ? 3 : t == AllTags ? 4 : t == Scale ? 5 : t == RotZ ? 6 : t == RotXY ? 7 : 0);
        m_sub->setVisible(m_sub->currentIndex() != 0);
        m_invert->setVisible(t == VClip || t == RClip);
        m_trackButton->setVisible(t == Move);
        m_posOptions->setVisible(t == Pos);
        for (auto *b : m_sub->widget(1)->findChildren<QToolButton *>())
            if (b != m_invert) b->setVisible(t != RClip);
        m_shapeBox->setVisible(t != RClip);
    }
    m_trackPts = 0;
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
    if (row != m_trackRow) m_trackPts = 0; // the points belong to the line they were set for
    if (m_tool == AllTags) hydraValues();
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

void VisualTools::applyHydra() {
    auto *tab = m_box->tab();
    const hydra::Tag &t = htag();
    const size_t n = size_t(hydra::count(t));
    tab->s().hydra(fs::slice(m_tagRows), size_t(tab->active()), {m_tagBase.data(), m_tagBase.size()}, fs::Utf8(t.tag),
                   uint8_t(m_tagMode->currentIndex()), rust::Slice<const double>(m_hv, n),
                   rust::Slice<const double>(m_hFrom, n), t.atStart ? -1 : m_tagSel.first,
                   t.atStart ? -1 : m_tagSel.second, fs::Utf8(m_amend));
    tab->changed(TabPanel::FromVideo);
    for (size_t i = 0; i < n; ++i) {
        const QSignalBlocker quiet(m_tagValues[i]);
        m_tagValues[i]->setValue(m_hv[i]);
    }
}

void VisualTools::hydraValues() {
    if (m_hydra.empty() || !m_valid) return;
    const hydra::Tag &t = htag();
    const int n = hydra::count(t);
    auto *tab = m_box->tab();
    const auto v = tab->s().tag_values(size_t(tab->active()), fs::Utf8(t.tag), size_t(n));
    for (int i = 0; i < 4; ++i) {
        // the line's value, else the list's default; the range takes it in (Kainote)
        const double x = i < int(v.size()) ? v[size_t(i)] : t.values.value(i);
        m_hv[i] = m_hFrom[i] = x;
        m_hMin[i] = std::min(t.min, x);
        m_hMax[i] = std::max(t.max, x);
        QDoubleSpinBox *s = m_tagValues[i];
        const QSignalBlocker quiet(s);
        s->setVisible(i < n);
        s->setDecimals(t.digits);
        s->setSingleStep(t.step);
        s->setValue(x);
    }
}

const hydra::Tag &VisualTools::htag() const {
    return m_hydra[size_t(std::clamp(m_tagName->currentIndex(), 0, int(m_hydra.size()) - 1))];
}

QLineF VisualTools::sliderLine(int i) const {
    const QRectF d = m_box->view()->displayRect();
    const double y = d.top() + m_sliderY + i * 28;
    return {d.left() + 30, y, std::max(d.left() + 60, d.right() - 30), y};
}

int VisualTools::sliderAt(QPointF at) const {
    for (int i = 0; i < hydra::count(htag()); ++i) {
        const QLineF l = sliderLine(i);
        if (std::abs(at.y() - l.y1()) <= 10 && at.x() >= l.x1() - 8 && at.x() <= l.x2() + 8) return i;
    }
    return -1;
}

double VisualTools::snapped(int i, double v) const {
    const hydra::Tag &t = htag();
    const double k = std::pow(10.0, t.digits);
    v = m_hMin[i] + std::round((v - m_hMin[i]) / t.step) * t.step;
    return std::clamp(std::round(v * k) / k, m_hMin[i], m_hMax[i]);
}

// Kainote's movement from two points, without its assumption that the first point was set at the
// line's start: the thing tracked moves from P1 at T1 to P2 at T2, and each grabbed line goes along
// from where it was at T1, over its own \move times (the whole line without them)
void VisualTools::writeTrack() {
    const double dt = double(m_trackT[1] - m_trackT[0]);
    if (dt == 0 || m_rows.empty()) return;
    const QPointF v = (m_trackP[1] - m_trackP[0]) / dt;
    auto &s = m_box->tab()->s();
    QStringList tags;
    for (size_t k = 0; k < m_rows.size(); ++k) {
        const fsb::Visual &st = m_starts[k];
        const fsb::Row r = s.row(m_rows[k]);
        const auto [t1, t2] = moveTimes(st, r);
        QPointF at(st.x, st.y);
        if (st.has_move) {
            const double u = std::clamp(double(m_trackT[0] - r.start - t1) / double(std::max<qint64>(1, t2 - t1)), 0.0, 1.0);
            at += (QPointF(st.x2, st.y2) - at) * u;
        }
        const QPointF a = at + v * double(r.start + t1 - m_trackT[0]), b = at + v * double(r.start + t2 - m_trackT[0]);
        tags << QString("\\move(%1,%2,%3,%4,%5,%6)").arg(n2(a.x()), n2(a.y()), n2(b.x()), n2(b.y())).arg(t1).arg(t2);
    }
    const auto t = fs::rvec(tags);
    s.set_each_tags(fs::slice(m_rows), {t.data(), t.size()}, fs::Utf8(m_amend)); // one step a drag
    m_box->tab()->changed(TabPanel::FromVideo);
}

void VisualTools::grabTexts() {
    auto *tab = m_box->tab();
    m_tagRows = tab->selection();
    m_tagBase.clear();
    for (size_t r : m_tagRows) m_tagBase.push_back(tab->s().row(r).text);
    int64_t cursor = 0, from = 0, to = 0;
    tab->edit()->scriptCursor(cursor, from, to); // 1-based
    m_tagSel = {from - 1, to - 1};
}

void VisualTools::adjustAll(const char *tags, uint8_t mode, std::initializer_list<double> deltas) {
    auto *tab = m_box->tab();
    tab->s().adjust_tag(fs::slice(m_tagRows), {m_tagBase.data(), m_tagBase.size()}, tags, mode,
                        rust::Slice<const double>(deltas.begin(), deltas.size()), fs::Utf8(m_amend));
    tab->changed(TabPanel::FromVideo);
}

void VisualTools::keepPlaces(QPointF around, double sx, double sy, double turn) {
    if (m_tagRows.size() < 2) return; // one line keeps its place (Kainote)
    auto *tab = m_box->tab();
    tab->s().transform_places(fs::slice(m_tagRows), around.x(), around.y(), sx, sy, turn, fs::Utf8(m_amend));
    tab->changed(TabPanel::FromVideo);
}

int VisualTools::rectHit(QRectF sr, QPointF at) const {
    const QPointF a = w(sr.topLeft()), b = w(sr.bottomRight());
    const QRectF r = QRectF(a, b).normalized();
    const QPointF cs[] = {a, {b.x(), a.y()}, {a.x(), b.y()}, b};
    for (int k = 0; k < 4; ++k)
        if (near(cs[k], at)) return 10 + k;
    const bool inX = at.x() > r.left() - 5 && at.x() < r.right() + 5;
    const bool inY = at.y() > r.top() - 5 && at.y() < r.bottom() + 5;
    for (int k = 0; k < 2; ++k)
        if (inY && std::abs(at.x() - (k ? b.x() : a.x())) <= 5) return 20 + k;
        else if (inX && std::abs(at.y() - (k ? b.y() : a.y())) <= 5) return 22 + k;
    return r.contains(at) ? 30 : 1;
}

QRectF VisualTools::rectDrag(QRectF r, int code, QPointF by, QPointF from, QPointF to) {
    if (code == 1) return QRectF(from, to).normalized();
    if (code == 30) return r.translated(by);
    const bool x2 = code >= 20 ? code == 21 : (code - 10) % 2, y2 = code >= 20 ? code == 23 : (code - 10) / 2;
    if (code < 22) x2 ? r.setRight(r.right() + by.x()) : r.setLeft(r.left() + by.x()); // a corner or an x edge
    if (code < 20 || code >= 22) y2 ? r.setBottom(r.bottom() + by.y()) : r.setTop(r.top() + by.y());
    return r;
}

void VisualTools::paintRect(QPainter &p, QRectF sr, QColor c) const {
    const QRectF r(w(sr.topLeft()), w(sr.bottomRight()));
    p.setPen(QPen(kShadow, 3));
    p.drawRect(r);
    p.setPen(QPen(c, 1.5));
    p.drawRect(r);
    for (QPointF k : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()}) handle(p, k, false, c);
}

bool VisualTools::measure() {
    auto *tab = m_box->tab();
    try {
        if (!m_measurer || m_measurerVideo != m_box->path()) {
            // shortcut: the video's attached fonts are read on the GUI thread, once a video; move it to a
            // worker if a file with huge attachments makes the first rectangle stall
            auto r = fsb::new_renderer();
            r->load_attached_fonts(fs::Utf8(m_box->path()));
            m_measurer.emplace(std::move(r));
            m_measurerVideo = m_box->path();
        }
        const auto b = (*m_measurer)->line_box(tab->s(), size_t(tab->active()), m_box->time());
        m_fitBox.assign(b.begin(), b.end());
    } catch (const rust::Error &) {
        m_fitBox.clear();
    }
    if (m_fitBox.size() < 10)
        QToolTip::showText(QCursor::pos(), tr("Nothing of the line shows on this frame to be measured"));
    return m_fitBox.size() >= 10;
}

void VisualTools::fitPos() {
    const QRectF r = m_fit.normalized();
    const double rect[] = {r.left(), r.top(), r.right(), r.bottom()};
    const auto at = fsb::fit_anchor(rust::Slice<const double>(m_fitBox.data(), 4), rust::Slice<const double>(rect, 4),
                                    uint8_t(m_posAlign->currentIndex()));
    moveLines({m_posWhat & 2 ? at[0] - m_fitBox[8] : 0, m_posWhat & 4 ? at[1] - m_fitBox[9] : 0});
}

// Kainote's scale by rectangle: the text alone grows to fill the red rectangle less the borders, which
// \fscx and \fscy leave as they are; the blue one, when there is one, stands for the text as it is
void VisualTools::fitScale() {
    const QRectF red = m_fit.normalized(), own = m_orig.normalized();
    const double tw = m_fitBox[6] - m_fitBox[4], th = m_fitBox[7] - m_fitBox[5];
    const double bw = m_fitBox[2] - m_fitBox[0] - tw, bh = m_fitBox[3] - m_fitBox[1] - th;
    const bool blue = (m_scaleWhat & 32) && m_origOn;
    const double gw = blue ? own.width() - bw : tw, gh = blue ? own.height() - bh : th;
    double fx = gw > 0 ? std::max(0.0, red.width() - bw) / gw : 1;
    double fy = gh > 0 ? std::max(0.0, red.height() - bh) / gh : 1;
    if (m_scaleWhat & 2) fx = fy = (m_scaleWhat & 1) ? fx : fy;
    else {
        if (!(m_scaleWhat & 1)) fx = 1;
        if (!(m_scaleWhat & 4)) fy = 1;
    }
    adjustAll("fscx,fscy", 7, {fx, fy});
    if (m_scaleWhat & 16) keepPlaces({m_start.x, m_start.y}, fx, fy, 0);
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
        p.setPen(handleColor());
        p.drawText(m + QPointF(8, -8), t);
        return;
    }
    // the other selected lines on this frame (Kainote), a click makes one the active line (Aegisub)
    const auto oth = others();
    for (const auto &o : oth) handle(p, o.second, true, lineColor());
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
    if ((m_tool == Pos && (m_posWhat & 1)) || (m_tool == Scale && (m_scaleWhat & 64))) {
        // Kainote's colours: red the rectangle to fill, blue the text as it is
        if (m_fitOn) paintRect(p, m_fit, QColor(255, 60, 60));
        if (m_tool == Scale && (m_scaleWhat & 32) && m_origOn) paintRect(p, m_orig, QColor(60, 140, 255));
    }
    if (m_tool == AllTags && !m_hydra.empty()) {
        // Kainote's sliders: a tick a step (fifty at most), the value at the thumb
        const hydra::Tag &t = htag();
        const int n = hydra::count(t);
        const auto text = [&p](QPointF at, const QString &s) {
            p.setPen(kShadow);
            p.drawText(at + QPointF(1, 1), s);
            p.setPen(handleColor());
            p.drawText(at, s);
        };
        for (int i = 0; i < n; ++i) {
            const QLineF l = sliderLine(i);
            const double range = m_hMax[i] - m_hMin[i];
            if (range <= 0) continue;
            line(p, l.p1(), l.p2());
            const int steps = int(range / t.step), every = std::max(1, int(std::ceil(steps / 50.0)));
            p.setPen(QPen(lineColor(), 1));
            for (int k = 0; k <= steps; k += every) {
                const double x = l.x1() + l.dx() * k * t.step / range;
                p.drawLine(QPointF(x, l.y1() - 3), QPointF(x, l.y1() + 3));
            }
            const QPointF at(l.x1() + l.dx() * (m_hv[i] - m_hMin[i]) / range, l.y1());
            const QRectF thumb(at.x() - 5, at.y() - 8, 10, 16);
            p.setPen(QPen(kShadow, 3));
            p.drawRect(thumb);
            p.fillRect(thumb, accentColor());
            const QString part = n == 3 && hydra::fixedCount(t.tag) == 3 ? QString("RGB"[i])
                                 : t.tag == "pos"                         ? QString("XY"[i])
                                 : n > 1                                  ? QString::number(i + 1)
                                                                          : QString();
            const QString name = i == 0 ? "\\" + t.tag + (part.isEmpty() ? "" : " ") : QString();
            text(at + QPointF(8, -6), name + part + " " + QString::number(m_hv[i], 'f', t.digits));
        }
    }
    const QPointF pos = w({m_v.x, m_v.y});
    switch (m_tool) {
    case Pos:
    case Shifter:
        handle(p, pos, !m_v.has_pos, m_v.has_pos ? handleColor() : lineColor());
        break;
    case Move: {
        const QPointF end = w({m_v.x2, m_v.y2});
        line(p, pos, end);
        handle(p, pos);
        handle(p, end, true, accentColor());
        if (m_v.has_move) { // where the line is on this frame (Kainote)
            const auto r = m_box->tab()->s().row(size_t(m_box->tab()->active()));
            const auto [t1, t2] = moveTimes(m_v, r);
            const double k = std::clamp(double(m_box->time() - r.start - t1) / double(std::max<qint64>(1, t2 - t1)), 0.0, 1.0);
            p.setPen(Qt::NoPen);
            p.setBrush(accentColor());
            p.drawEllipse(pos + (end - pos) * k, 3, 3);
            p.setBrush(Qt::NoBrush);
        }
        if (m_track && m_trackPts) { // the two points tracked, numbered
            if (m_trackPts == 2) line(p, w(m_trackP[0]), w(m_trackP[1]), accentColor(), Qt::DashLine);
            for (int k = 0; k < m_trackPts; ++k) {
                const QPointF at = w(m_trackP[k]);
                handle(p, at, false, accentColor());
                p.setPen(accentColor());
                p.drawText(at + QPointF(7, -6), QString::number(k + 1));
            }
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
        p.setPen(QPen(lineColor(), 1, Qt::DashLine));
        p.drawEllipse(org, 70, 70);
        line(p, org, tip);
        line(p, org, pos, QColor(255, 255, 255, 100), Qt::DotLine);
        handle(p, tip, true);
        handle(p, org, false, accentColor());
        handle(p, pos, true);
        if (m_drag == 4) {
            line(p, m_pressW, m_mouseW, accentColor());
            handle(p, m_pressW, false, accentColor());
            handle(p, m_mouseW, false, accentColor());
        }
        break;
    }
    case RotXY: {
        p.save();
        p.translate(pos);
        // the grid is foreshortened like the rotated text plane
        p.scale(std::cos(qDegreesToRadians(m_v.fry)), std::cos(qDegreesToRadians(m_v.frx)));
        p.setPen(QPen(lineColor(), 1));
        for (int k = -3; k <= 3; ++k) {
            p.drawLine(QPointF(k * 20, -60), QPointF(k * 20, 60));
            p.drawLine(QPointF(-60, k * 20), QPointF(60, k * 20));
        }
        p.restore();
        const QPointF org = w({m_v.org_x, m_v.org_y});
        line(p, org, pos, QColor(255, 255, 255, 100), Qt::DotLine);
        handle(p, pos, true);
        handle(p, org, false, accentColor());
        break;
    }
    case RClip:
        if (m_v.clip == 1) {
            const QRectF r(w({m_v.clip_x1, m_v.clip_y1}), w({m_v.clip_x2, m_v.clip_y2}));
            if (m_v.inverse) p.fillRect(r, theme::color(theme::VisualShade));
            else {
                QPainterPath outside;
                outside.addRect(d);
                outside.addRect(r.normalized());
                p.fillPath(outside, theme::color(theme::VisualShade));
            }
            p.setPen(QPen(lineColor(), 1));
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
            if (!m_v.inverse) p.fillPath(shade, theme::color(theme::VisualShade));
            else p.fillPath(path, theme::color(theme::VisualShade));
        }
        p.setPen(QPen(kShadow, 3));
        p.drawPath(path);
        p.setPen(QPen(lineColor(), 1.2));
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
                handle(p, last, false, c.type == 'm' ? accentColor() : handleColor());
            }
        }
        if (m_tool == Draw) handle(p, pos, true, accentColor());
        p.setPen(Qt::NoPen);
        p.setBrush(accentColor());
        for (const auto &h : m_vecSel) p.drawRect(QRectF(map(m_vec.at(h)) - QPointF(3, 3), QSizeF(6, 6)));
        p.setBrush(Qt::NoBrush);
        if (m_drag == 7) {
            p.setPen(QPen(lineColor(), 1, Qt::DashLine));
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
    case Pos:
        if (m_posWhat & 1) {
            m_drag = 0;
            if (m_button != Qt::LeftButton || !measure()) return true;
            grabLines();
            m_drag = m_fitOn ? rectHit(m_fit, m_pressW) : 1;
            m_fitStart = m_fit;
            return true;
        }
        grabLines();
        break;
    case Move:
        if (m_track) {
            m_drag = 0;
            if (m_button == Qt::RightButton) m_trackPts = 0;
            else if (m_trackPts && near(w(m_trackP[0]), m_pressW, 8)) m_drag = 40;
            else if (m_trackPts == 2 && near(w(m_trackP[1]), m_pressW, 8)) m_drag = 41;
            else if (m_trackPts == 0) {
                m_trackP[0] = m_pressS;
                m_trackT[0] = m_box->time();
                m_trackPts = 1;
                m_trackRow = tab->active();
            } else if (m_trackPts == 1) {
                if (m_box->time() == m_trackT[0]) {
                    QToolTip::showText(e->globalPosition().toPoint(),
                                       tr("Seek the video at least one frame from the first point's time"));
                    return true;
                }
                QPointF at = m_pressS;
                if (e->modifiers() & Qt::ShiftModifier) { // level or plumb with the first point
                    const QPointF d = at - m_trackP[0];
                    std::abs(d.x()) > std::abs(d.y()) ? at.setY(m_trackP[0].y()) : at.setX(m_trackP[0].x());
                }
                m_trackP[1] = at;
                m_trackT[1] = m_box->time();
                m_trackPts = 2;
                grabLines();
                writeTrack();
            }
            m_box->view()->updateOverlay();
            return true;
        }
        if (m_button == Qt::RightButton) m_drag = 2;
        break;
    case Scale:
        if (m_scaleWhat & 64) {
            const bool own = m_button == Qt::RightButton && (m_scaleWhat & 32);
            m_drag = 0;
            if (own) {
                m_drag = 100 + (m_origOn ? rectHit(m_orig, m_pressW) : 1);
                m_origStart = m_orig;
            } else if (m_button == Qt::LeftButton && measure()) {
                grabTexts();
                m_drag = m_fitOn ? rectHit(m_fit, m_pressW) : 1;
                m_fitStart = m_fit;
            }
            return true;
        }
        if (m_scaleWhat & 8) grabTexts();
        break;
    case AllTags: {
        m_drag = 0;
        if (m_hydra.empty()) return true;
        if (m_button == Qt::RightButton) { // the sliders move up or down (Kainote)
            m_drag = 60;
            m_sliderYStart = m_sliderY;
            return true;
        }
        const int i = sliderAt(m_pressW);
        if (m_button != Qt::LeftButton || i < 0) return true;
        grabTexts();
        m_drag = 50 + i; // the thumb goes where the slider was clicked
        break;
    }
    case RotZ:
    case RotXY:
        if (near(w({m_v.org_x, m_v.org_y}), m_pressW, 9)) {
            m_drag = 2;
            grabLines();
            break;
        }
        if (m_tool == RotZ && m_twoPoints) m_drag = 4;
        if (m_rotWhat & (m_tool == RotZ ? 1 : 4)) grabTexts();
        break;
    case RClip:
        grabLines();
        if (m_v.clip == 1) m_drag = rectHit(QRectF(QPointF(m_v.clip_x1, m_v.clip_y1), QPointF(m_v.clip_x2, m_v.clip_y2)), m_pressW);
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
        if (m_posWhat & 1) {
            m_fit = rectDrag(m_fitStart, m_drag, delta, m_pressS, now);
            m_fitOn = true;
            fitPos();
            m_box->view()->updateOverlay();
            break;
        }
        if (m_drag == 1 && delta.isNull()) break; // a click alone writes nothing
        m_drag = 3;
        moveLines(delta); // every selected line by the same delta
        break;
    case Move: {
        if (m_drag >= 40) {
            const int k = m_drag - 40;
            QPointF at = now;
            if (e->modifiers() & Qt::ShiftModifier) { // shares x or y with the other point
                const QPointF d = at - m_trackP[1 - k];
                std::abs(d.x()) > std::abs(d.y()) ? at.setY(m_trackP[1 - k].y()) : at.setX(m_trackP[1 - k].x());
            }
            m_trackP[k] = at;
            if (m_trackPts == 2) writeTrack();
            m_box->view()->updateOverlay();
            break;
        }
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
        if (m_drag >= 100) { // the blue rectangle: drawn only, the next red one uses it
            m_orig = rectDrag(m_origStart, m_drag - 100, delta, m_pressS, now);
            m_origOn = true;
            m_box->view()->updateOverlay();
            break;
        }
        if (m_scaleWhat & 64) {
            m_fit = rectDrag(m_fitStart, m_drag, delta, m_pressS, now);
            m_fitOn = true;
            fitScale();
            m_box->view()->updateOverlay();
            break;
        }
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
        if (m_scaleWhat & 8) {
            // the active line's change as a factor, for every scale tag of every selected line
            const double fx = m_start.fscx > 0 ? sx / m_start.fscx : 1, fy = m_start.fscy > 0 ? sy / m_start.fscy : 1;
            adjustAll("fscx,fscy", 7, {fx, fy});
            if (m_scaleWhat & 16) keepPlaces({m_start.x, m_start.y}, fx, fy, 0);
        } else {
            apply(QString("\\fscx%1\\fscy%2").arg(sx).arg(sy), true);
        }
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
        // the shortest turn from the angle the drag started at
        const auto turned = [](double from, double to) { return std::remainder(to - from, 360.0); };
        if (m_tool == RotXY) {
            double frx = m_start.frx, fry = m_start.fry;
            // two degrees a pixel (Aegisub)
            if (m_button != Qt::RightButton) fry = angle(m_start.fry + dw.x() * 2);
            if (m_button != Qt::LeftButton) frx = angle(m_start.frx - dw.y() * 2);
            if (m_rotWhat & 4) adjustAll("frx,fry", 0, {turned(m_start.frx, frx), turned(m_start.fry, fry)});
            else apply(QString("\\frx%1\\fry%2").arg(n2(frx), n2(fry)), true);
            break;
        }
        double frz;
        if (m_drag == 4) {
            const QPointF d = now - m_pressS;
            if (QLineF(m_pressW, e->position()).length() < 4) break;
            frz = angle(qRadiansToDegrees(std::atan2(-d.y(), d.x())));
            m_box->view()->updateOverlay();
        } else {
            const QPointF c = w({m_start.org_x, m_start.org_y});
            const double a0 = std::atan2(-(m_pressW.y() - c.y()), m_pressW.x() - c.x());
            const double a1 = std::atan2(-(e->position().y() - c.y()), e->position().x() - c.x());
            frz = angle(m_start.frz + qRadiansToDegrees(a1 - a0));
        }
        if (m_rotWhat & 1) {
            const double by = turned(m_start.frz, frz);
            adjustAll("frz", 0, {by});
            if (m_rotWhat & 2) keepPlaces({m_start.org_x, m_start.org_y}, 1, 1, by);
        } else {
            apply(QString("\\frz%1").arg(n2(frz)), true);
        }
        break;
    }
    case RClip: {
        const QRectF r = rectDrag(QRectF(QPointF(m_start.clip_x1, m_start.clip_y1), QPointF(m_start.clip_x2, m_start.clip_y2)),
                                  m_drag, delta, m_pressS, now);
        m_v.clip = 1;
        m_v.clip_x1 = r.left();
        m_v.clip_y1 = r.top();
        m_v.clip_x2 = r.right();
        m_v.clip_y2 = r.bottom();
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
        if (m_drag == 60) {
            const double room = std::max(0.0, m_box->view()->displayRect().height() - 20);
            m_sliderY = std::clamp(m_sliderYStart + e->position().y() - m_pressW.y(), 0.0, room);
            m_box->view()->updateOverlay();
            break;
        }
        const int i = m_drag - 50;
        const QLineF l = sliderLine(i);
        const double f = std::clamp((e->position().x() - l.x1()) / l.dx(), 0.0, 1.0);
        const double v = snapped(i, m_hMin[i] + f * (m_hMax[i] - m_hMin[i]));
        if (v == m_hv[i]) break; // nothing new to write
        m_hv[i] = v;
        applyHydra();
        m_box->view()->updateOverlay();
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
    if ((m_tool == Pos && (m_posWhat & 1)) || (m_tool == Scale && (m_scaleWhat & 64))) {
        // a rectangle of no size is none (Kainote)
        const auto none = [](QRectF r) { return r.normalized().width() < 1 || r.normalized().height() < 1; };
        if (m_drag >= 100) m_origOn = !none(m_orig);
        else if (none(m_fit)) m_fitOn = false;
        else if (m_tool == Scale && m_origOn) m_orig = m_fit; // the text fills the red one now
    }
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

bool VisualTools::wheel(QWheelEvent *e) {
    if (m_tool != AllTags || m_hydra.empty() || !shown()) return false;
    const int d = e->angleDelta().y() ? e->angleDelta().y() : e->angleDelta().x();
    if (!d) return true;
    if (e->modifiers() & Qt::ShiftModifier) { // another tag (Kainote)
        const int n = m_tagName->count();
        m_tagName->setCurrentIndex(((m_tagName->currentIndex() + (d > 0 ? -1 : 1)) % n + n) % n);
        return true;
    }
    // a step on the slider at the mouse's height, the first one elsewhere
    const int i = std::max(0, sliderAt(QPointF(sliderLine(0).x1(), e->position().y())));
    grabTexts();
    m_hv[i] = snapped(i, m_hv[i] + (d > 0 ? htag().step : -htag().step));
    // the notches on one line and tag are one undo step
    m_amend = QString("visual-tags-wheel-%1-%2").arg(m_box->tab()->active()).arg(htag().tag);
    applyHydra();
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
    if (m_tool == AllTags && shown() && !m_hydra.empty() && (htag().tag == "fad" || htag().tag == "t")) {
        // the edit box's keys for the video time's distance from the start or to the end (Kainote):
        // the fade's in and out, or the transform's start and end
        const char *id = m_box->tab()->main()->panelCommand(e, EditBox::keys());
        const bool start = id && qstrcmp(id, "editStartDiff") == 0, end = id && qstrcmp(id, "editEndDiff") == 0;
        if (start || end) {
            const fsb::Row r = m_box->tab()->s().row(size_t(m_box->tab()->active()));
            const qint64 t = m_box->time();
            if (t < r.start || t > r.end) {
                QApplication::beep();
                return true;
            }
            grabTexts();
            m_hv[start ? 0 : 1] = double(start || htag().tag == "t" ? t - r.start : r.end - t);
            m_amend = QString("visual-tags-%1").arg(++m_dragId);
            applyHydra();
            return true;
        }
    }
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

#include "shiftpanel.h"

#include "audiobox.h"
#include "editbox.h"
#include "grid.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QGroupBox *group(const QString &title, QWidget *parent, QVBoxLayout *into) {
    auto *g = new QGroupBox(title, parent);
    new QVBoxLayout(g);
    into->addWidget(g);
    return g;
}
template <class W> W *add(QGroupBox *g, W *w) {
    g->layout()->addWidget(w);
    return w;
}
QSpinBox *msSpin(QWidget *parent, int value) {
    auto *s = new QSpinBox(parent);
    s->setRange(0, 10000);
    s->setSuffix(" ms");
    s->setValue(value);
    return s;
}
} // namespace

ShiftPanel::ShiftPanel(TabPanel *tab) : QScrollArea(tab), m_tab(tab) {
    auto *root = new QWidget(this);
    auto *v = new QVBoxLayout(root);
    v->setContentsMargins(4, 4, 4, 4);
    m_mode = new QPushButton(tr("Shift times"), root);
    v->addWidget(m_mode);
    m_stack = new QStackedWidget(root);
    v->addWidget(m_stack);
    v->addStretch();

    // ---- shift times
    auto *page = new QWidget(m_stack);
    auto *pv = new QVBoxLayout(page);
    pv->setContentsMargins(0, 0, 0, 0);
    auto *tb = group(tr("Time"), page, pv);
    m_time = new TimeEdit(tb);
    auto *row = new QHBoxLayout;
    row->addWidget(m_time);
    auto *go = new QPushButton(tr("Shift"), tb);
    row->addWidget(go);
    static_cast<QVBoxLayout *>(tb->layout())->addLayout(row);
    m_forward = add(tb, new QRadioButton(tr("Forward"), tb));
    m_backward = add(tb, new QRadioButton(tr("Backward"), tb));
    m_forward->setChecked(true);
    m_frames = add(tb, new QCheckBox(tr("Frames"), tb));
    m_tagTimes = add(tb, new QCheckBox(tr("Tag times"), tb));
    m_tagTimes->setToolTip(tr("Keep \\move and \\t times in place when only the start moves"));
    connect(m_frames, &QCheckBox::toggled, m_time, &TimeEdit::setFrames);

    auto *vb = group(tr("Shift by video/audio"), page, pv);
    m_byVideo = add(vb, new QCheckBox(tr("Enable"), vb));
    m_byStart = add(vb, new QRadioButton(tr("Beginning"), vb));
    m_byEnd = add(vb, new QRadioButton(tr("End"), vb));
    m_byStart->setChecked(true);
    m_markerToVideo = add(vb, new QCheckBox(tr("Move the marker to video time"), vb));
    m_markerToAudio = add(vb, new QCheckBox(tr("Move the marker to audio time"), vb));
    // They pick where the marked line moves to; with neither, video wins when loaded.
    m_markerToVideo->setToolTip(tr("Shift so the marked line lands on the video time"));
    m_markerToAudio->setToolTip(tr("Shift so the marked line lands on the audio marker"));

    auto *lb = group(tr("Which lines"), page, pv);
    const char *lines[] = {"All lines", "Selected lines", "From the selected line", "All times higher and equal",
                           "All times lower and equal", "According to the selected styles"};
    auto *lg = new QButtonGroup(this);
    for (int k = 0; k < 6; ++k) {
        m_lines[k] = add(lb, new QRadioButton(tr(lines[k]), lb));
        lg->addButton(m_lines[k]);
    }
    auto *srow = new QHBoxLayout;
    m_styles = new QLineEdit(lb);
    m_styles->setPlaceholderText(tr("styles"));
    auto *pick = new QToolButton(lb);
    pick->setText("+");
    srow->addWidget(m_styles);
    srow->addWidget(pick);
    static_cast<QVBoxLayout *>(lb->layout())->addLayout(srow);
    connect(pick, &QToolButton::clicked, this, [this] {
        QStringList names;
        for (const auto &st : m_tab->s().styles()) names << fs::q(st.name);
        bool ok;
        const QString s = QInputDialog::getItem(this, tr("Styles"), tr("Add style"), names, 0, false, &ok);
        if (ok) m_styles->setText(m_styles->text().isEmpty() ? s : m_styles->text() + "," + s);
    });

    auto *mb = group(tr("A method of time shift"), page, pv);
    const char *which[] = {"Both times", "The starting time", "End time"};
    auto *wg = new QButtonGroup(this);
    for (int k = 0; k < 3; ++k) {
        m_which[k] = add(mb, new QRadioButton(tr(which[k]), mb));
        wg->addButton(m_which[k]);
    }
    auto *eb = group(tr("Correction of end times"), page, pv);
    const char *fix[] = {"Leave unchanged", "Adjust overlapping times", "New times"};
    auto *eg = new QButtonGroup(this);
    for (int k = 0; k < 3; ++k) {
        m_endFix[k] = add(eb, new QRadioButton(tr(fix[k]), eb));
        eg->addButton(m_endFix[k]);
    }
    m_stack->addWidget(page);
    connect(go, &QPushButton::clicked, this, &ShiftPanel::shift);

    // ---- post processor
    auto *pp = new QWidget(m_stack);
    auto *ppv = new QVBoxLayout(pp);
    ppv->setContentsMargins(0, 0, 0, 0);
    auto *run = new QPushButton(tr("Run post processor"), pp);
    ppv->addWidget(run);
    auto *li = group(tr("Lead-in / lead-out"), pp, ppv);
    auto *f1 = new QFormLayout;
    m_leadInOn = new QCheckBox(tr("Lead-in"), li);
    m_leadIn = msSpin(li, 200);
    m_leadOutOn = new QCheckBox(tr("Lead-out"), li);
    m_leadOut = msSpin(li, 300);
    f1->addRow(m_leadInOn, m_leadIn);
    f1->addRow(m_leadOutOn, m_leadOut);
    static_cast<QVBoxLayout *>(li->layout())->addLayout(f1);
    auto *ct = group(tr("Set times as continuous"), pp, ppv);
    auto *f2 = new QFormLayout;
    m_contOn = new QCheckBox(tr("Enable"), ct);
    m_contStart = msSpin(ct, 300);
    m_bias = new QDoubleSpinBox(ct);
    m_bias->setRange(0, 1);
    m_bias->setSingleStep(0.1);
    m_bias->setValue(0.5);
    f2->addRow(m_contOn);
    f2->addRow(tr("Threshold"), m_contStart);
    f2->addRow(tr("Bias (0 = start, 1 = end)"), m_bias);
    static_cast<QVBoxLayout *>(ct->layout())->addLayout(f2);
    auto *kf = group(tr("Snap to keyframes"), pp, ppv);
    auto *f3 = new QFormLayout;
    m_kfOn = new QCheckBox(tr("Enable"), kf);
    m_kfBs = msSpin(kf, 150);
    m_kfAs = msSpin(kf, 150);
    m_kfBe = msSpin(kf, 300);
    m_kfAe = msSpin(kf, 300);
    f3->addRow(m_kfOn);
    f3->addRow(tr("Before start"), m_kfBs);
    f3->addRow(tr("After start"), m_kfAs);
    f3->addRow(tr("Before end"), m_kfBe);
    f3->addRow(tr("After end"), m_kfAe);
    static_cast<QVBoxLayout *>(kf->layout())->addLayout(f3);
    ppv->addStretch();
    m_stack->addWidget(pp);
    connect(run, &QPushButton::clicked, this, &ShiftPanel::postProcess);

    connect(m_mode, &QPushButton::clicked, this, [this] {
        const int i = 1 - m_stack->currentIndex();
        m_stack->setCurrentIndex(i);
        m_mode->setText(i ? tr("Post processor") : tr("Shift times"));
    });

    setWidget(root);
    setWidgetResizable(true);
    setMinimumWidth(root->sizeHint().width() + 20);
    load();
}

int ShiftPanel::whichLines() const {
    for (int k = 0; k < 6; ++k)
        if (m_lines[k]->isChecked()) return k;
    return 0;
}

void ShiftPanel::shift() {
    auto &s = m_tab->s();
    fsb::ShiftSpec spec;
    spec.frames = m_frames->isChecked();
    qint64 amount = spec.frames ? m_time->frame() : m_time->ms();
    if (amount < 0) return;
    if (m_backward->isChecked()) amount = -amount;
    if (m_byVideo->isChecked()) {
        const int marker = m_tab->grid()->syncMarker() >= 0 ? m_tab->grid()->syncMarker() : m_tab->active();
        const fsb::Row r = s.row(size_t(marker));
        const bool video = m_markerToVideo->isChecked() ? true
                           : m_markerToAudio->isChecked() ? false
                                                          : m_tab->video()->hasVideo();
        const qint64 target = video ? (m_tab->video()->hasVideo() ? m_tab->videoTime() : -1) : m_tab->audio()->markTime();
        if (target < 0) return;
        amount = target - (m_byEnd->isChecked() ? r.end : r.start);
        spec.frames = false;
    }
    spec.amount = amount;
    for (int k = 0; k < 3; ++k) {
        if (m_which[k]->isChecked()) spec.which = uint8_t(k);
        if (m_endFix[k]->isChecked()) spec.end_fix = uint8_t(k);
    }
    spec.lines = uint8_t(whichLines());
    for (size_t r : m_tab->selection()) spec.selected.push_back(r);
    spec.active = size_t(std::max(0, m_tab->active()));
    for (const auto &st : m_styles->text().split(',', Qt::SkipEmptyParts)) spec.styles.push_back(fs::rs(st.trimmed()));
    spec.tag_times = m_tagTimes->isChecked();
    if (m_tab->grid()->filterOn && !m_tab->grid()->ignoreFilterInActions && spec.lines == 0) {
        spec.lines = 1;
        spec.selected.clear();
        for (size_t r : m_tab->grid()->visibleRows()) spec.selected.push_back(r);
    }
    const auto tc = m_tab->video()->timecodes();
    const size_t n = s.shift(spec, {tc.data(), tc.size()});
    m_tab->changed();
    m_tab->main()->status(tr("Shifted %1 lines").arg(n));
    save();
}

void ShiftPanel::postProcess() {
    fsb::PostSpec spec;
    spec.lead_in = m_leadInOn->isChecked() ? m_leadIn->value() : 0;
    spec.lead_out = m_leadOutOn->isChecked() ? m_leadOut->value() : 0;
    spec.adjacent = m_contOn->isChecked() ? m_contStart->value() : 0;
    spec.bias = m_bias->value();
    spec.snap_keyframes = m_kfOn->isChecked();
    spec.kf_before_start = m_kfBs->value();
    spec.kf_after_start = m_kfAs->value();
    spec.kf_before_end = m_kfBe->value();
    spec.kf_after_end = m_kfAe->value();
    const auto sel = whichLines() == 1 ? m_tab->selection() : std::vector<size_t>{};
    if (sel.empty())
        for (size_t r = 0; r < m_tab->s().row_count(); ++r) spec.rows.push_back(r);
    else
        for (size_t r : sel) spec.rows.push_back(r);
    const auto kf = m_tab->keyframes();
    const auto tc = m_tab->video()->timecodes();
    m_tab->s().post_process(spec, {kf.data(), kf.size()}, {tc.data(), tc.size()});
    m_tab->changed();
    save();
}

void ShiftPanel::save() const {
    QSettings st;
    st.beginGroup("shiftTimes");
    st.setValue("time", m_time->text());
    st.setValue("backward", m_backward->isChecked());
    st.setValue("frames", m_frames->isChecked());
    st.setValue("tagTimes", m_tagTimes->isChecked());
    st.setValue("byVideo", m_byVideo->isChecked());
    st.setValue("byEnd", m_byEnd->isChecked());
    st.setValue("markerToVideo", m_markerToVideo->isChecked());
    st.setValue("markerToAudio", m_markerToAudio->isChecked());
    st.setValue("lines", whichLines());
    st.setValue("styles", m_styles->text());
    for (int k = 0; k < 3; ++k) {
        if (m_which[k]->isChecked()) st.setValue("which", k);
        if (m_endFix[k]->isChecked()) st.setValue("endFix", k);
    }
    st.setValue("postProcessor", m_stack->currentIndex());
    st.setValue("leadIn", m_leadInOn->isChecked() ? m_leadIn->value() : -m_leadIn->value());
    st.setValue("leadOut", m_leadOutOn->isChecked() ? m_leadOut->value() : -m_leadOut->value());
    st.setValue("continuous", m_contOn->isChecked());
    st.setValue("keyframes", m_kfOn->isChecked());
}

void ShiftPanel::load() {
    QSettings st;
    st.beginGroup("shiftTimes");
    m_frames->setChecked(st.value("frames").toBool());
    m_time->setText(st.value("time", "0:00:00.00").toString());
    (st.value("backward").toBool() ? m_backward : m_forward)->setChecked(true);
    m_tagTimes->setChecked(st.value("tagTimes").toBool());
    m_byVideo->setChecked(st.value("byVideo").toBool());
    (st.value("byEnd").toBool() ? m_byEnd : m_byStart)->setChecked(true);
    m_markerToVideo->setChecked(st.value("markerToVideo").toBool());
    m_markerToAudio->setChecked(st.value("markerToAudio").toBool());
    m_lines[std::clamp(st.value("lines", 1).toInt(), 0, 5)]->setChecked(true);
    m_styles->setText(st.value("styles").toString());
    m_which[std::clamp(st.value("which", 0).toInt(), 0, 2)]->setChecked(true);
    m_endFix[std::clamp(st.value("endFix", 0).toInt(), 0, 2)]->setChecked(true);
    const int pp = st.value("postProcessor", 0).toInt();
    m_stack->setCurrentIndex(pp);
    m_mode->setText(pp ? tr("Post processor") : tr("Shift times"));
    const int li = st.value("leadIn", -200).toInt(), lo = st.value("leadOut", -300).toInt();
    m_leadInOn->setChecked(li > 0);
    m_leadIn->setValue(std::abs(li));
    m_leadOutOn->setChecked(lo > 0);
    m_leadOut->setValue(std::abs(lo));
    m_contOn->setChecked(st.value("continuous").toBool());
    m_kfOn->setChecked(st.value("keyframes").toBool());
}

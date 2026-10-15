#include "shiftpanel.h"

#include "audiobox.h"
#include "editbox.h"
#include "grid.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
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
    auto *l = new QGridLayout(g);
    l->setContentsMargins(4, 4, 4, 4);
    l->setSpacing(3);
    into->addWidget(g);
    return g;
}
QGridLayout *grid(QGroupBox *g) { return static_cast<QGridLayout *>(g->layout()); }
QSpinBox *msSpin(QWidget *parent, int value) {
    auto *s = new QSpinBox(parent);
    s->setRange(0, 10000);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setToolTip(QObject::tr("Milliseconds"));
    s->setValue(value);
    return s;
}
QComboBox *combo(QWidget *parent, std::initializer_list<const char *> items) {
    auto *c = new QComboBox(parent);
    for (const char *i : items) c->addItem(ShiftPanel::tr(i));
    return c;
}
// "Which lines": a choice plus a style list filled by the "+" button
QComboBox *whichLines(QGroupBox *g, QLineEdit *&styles, TabPanel *tab) {
    auto *c = combo(g, {QT_TRANSLATE_NOOP("ShiftPanel", "All lines"), QT_TRANSLATE_NOOP("ShiftPanel", "Selected lines"),
                        QT_TRANSLATE_NOOP("ShiftPanel", "From the selected line"),
                        QT_TRANSLATE_NOOP("ShiftPanel", "All times higher and equal"),
                        QT_TRANSLATE_NOOP("ShiftPanel", "All times lower and equal"),
                        QT_TRANSLATE_NOOP("ShiftPanel", "According to the selected styles")});
    auto *pick = new QToolButton(g);
    pick->setText("+");
    pick->setToolTip(ShiftPanel::tr("Add a style"));
    styles = new QLineEdit(g);
    styles->setPlaceholderText(ShiftPanel::tr("styles"));
    grid(g)->addWidget(c, 0, 0, 1, 2);
    grid(g)->addWidget(pick, 1, 0);
    grid(g)->addWidget(styles, 1, 1);
    QObject::connect(pick, &QToolButton::clicked, g, [g, styles, tab] {
        QStringList names;
        for (const auto &st : tab->s().styles()) names << fs::q(st.name);
        bool ok;
        const QString s = QInputDialog::getItem(g, ShiftPanel::tr("Styles"), ShiftPanel::tr("Add style"), names, 0, false, &ok);
        if (ok) styles->setText(styles->text().isEmpty() ? s : styles->text() + "," + s);
    });
    return c;
}
} // namespace

ShiftPanel::ShiftPanel(TabPanel *tab) : QScrollArea(tab), m_tab(tab) {
    auto *root = new QWidget(this);
    auto *v = new QVBoxLayout(root);
    v->setContentsMargins(4, 4, 4, 4);
    // the header button names the other page, as in Kainote
    m_mode = new QPushButton(root);
    v->addWidget(m_mode);
    m_stack = new QStackedWidget(root);
    v->addWidget(m_stack);
    v->addStretch();

    // ---- shift times
    auto *page = new QWidget(m_stack);
    auto *pv = new QVBoxLayout(page);
    pv->setContentsMargins(0, 0, 0, 0);

    auto *prof = group(tr("Profiles"), page, pv);
    auto *addProf = new QToolButton(prof);
    addProf->setText("+");
    addProf->setToolTip(tr("Save the current settings as a profile"));
    auto *delProf = new QToolButton(prof);
    delProf->setText("-");
    delProf->setToolTip(tr("Remove the chosen profile"));
    m_profiles = new QComboBox(prof);
    grid(prof)->addWidget(addProf, 0, 0);
    grid(prof)->addWidget(delProf, 0, 1);
    grid(prof)->addWidget(m_profiles, 0, 2);
    grid(prof)->setColumnStretch(2, 1);

    auto *tb = group(tr("Time"), page, pv);
    m_time = new TimeEdit(tb);
    auto *go = new QPushButton(tr("Shift"), tb);
    m_forward = new QRadioButton(tr("Forward"), tb);
    m_backward = new QRadioButton(tr("Backward"), tb);
    m_forward->setChecked(true);
    m_frames = new QCheckBox(tr("Frames"), tb);
    m_tagTimes = new QCheckBox(tr("Tag times"), tb);
    m_tagTimes->setToolTip(tr("Keep \\move and \\t times in place when only the start moves"));
    grid(tb)->addWidget(m_time, 0, 0);
    grid(tb)->addWidget(go, 0, 1);
    grid(tb)->addWidget(m_forward, 1, 0);
    grid(tb)->addWidget(m_backward, 1, 1);
    grid(tb)->addWidget(m_frames, 2, 0);
    grid(tb)->addWidget(m_tagTimes, 2, 1);
    connect(m_frames, &QCheckBox::toggled, m_time, &TimeEdit::setFrames);

    auto *vb = group(tr("Shift by video / audio"), page, pv);
    m_byStart = new QRadioButton(tr("Beginning"), vb);
    m_byEnd = new QRadioButton(tr("End"), vb);
    m_byStart->setChecked(true);
    m_markerToVideo = new QCheckBox(tr("Move the marker\nto video time"), vb);
    m_markerToAudio = new QCheckBox(tr("Move the marker\nto audio time"), vb);
    // either one turns "Shift" into: move the marked line (right-click its number) onto the video time / audio marker
    m_markerToVideo->setToolTip(tr("Shift so the marked line lands on the video time"));
    m_markerToAudio->setToolTip(tr("Shift so the marked line lands on the audio marker"));
    connect(m_markerToVideo, &QCheckBox::toggled, this, [this](bool on) { if (on) m_markerToAudio->setChecked(false); });
    connect(m_markerToAudio, &QCheckBox::toggled, this, [this](bool on) { if (on) m_markerToVideo->setChecked(false); });
    grid(vb)->addWidget(m_byStart, 0, 0);
    grid(vb)->addWidget(m_byEnd, 0, 1);
    grid(vb)->addWidget(m_markerToVideo, 1, 0, 1, 2);
    grid(vb)->addWidget(m_markerToAudio, 2, 0, 1, 2);

    m_lines = whichLines(group(tr("Which lines"), page, pv), m_styles, m_tab);
    auto *mb = group(tr("A method of time shift"), page, pv);
    m_which = combo(mb, {QT_TRANSLATE_NOOP("ShiftPanel", "Both times"),
                        QT_TRANSLATE_NOOP("ShiftPanel", "The starting time"),
                        QT_TRANSLATE_NOOP("ShiftPanel", "End time")});
    grid(mb)->addWidget(m_which, 0, 0);
    auto *eb = group(tr("Correction of end times"), page, pv);
    m_endFix = combo(eb, {QT_TRANSLATE_NOOP("ShiftPanel", "Leave unchanged"),
                        QT_TRANSLATE_NOOP("ShiftPanel", "Adjust overlapping times"),
                        QT_TRANSLATE_NOOP("ShiftPanel", "New times")});
    grid(eb)->addWidget(m_endFix, 0, 0);
    m_stack->addWidget(page);
    connect(go, &QPushButton::clicked, this, &ShiftPanel::shift);

    // profiles live under shiftTimes/profiles/<name>
    auto refreshProfiles = [this] {
        QSettings st;
        st.beginGroup("shiftTimes/profiles");
        const QString cur = m_profiles->currentText();
        m_profiles->blockSignals(true);
        m_profiles->clear();
        m_profiles->addItems(st.childGroups());
        m_profiles->setCurrentIndex(m_profiles->findText(cur));
        m_profiles->blockSignals(false);
    };
    refreshProfiles();
    connect(addProf, &QToolButton::clicked, this, [this, refreshProfiles] {
        bool ok;
        const QString name = QInputDialog::getText(this, tr("Profiles"), tr("Profile name"), QLineEdit::Normal,
                                                   m_profiles->currentText(), &ok)
                                 .trimmed()
                                 .remove('/');
        if (!ok || name.isEmpty()) return;
        QSettings st;
        st.beginGroup("shiftTimes/profiles/" + name);
        store(st);
        st.endGroup();
        refreshProfiles();
        m_profiles->setCurrentText(name);
    });
    connect(delProf, &QToolButton::clicked, this, [this, refreshProfiles] {
        if (m_profiles->currentText().isEmpty()) return;
        QSettings().remove("shiftTimes/profiles/" + m_profiles->currentText());
        refreshProfiles();
    });
    connect(m_profiles, &QComboBox::activated, this, [this] {
        QSettings st;
        st.beginGroup("shiftTimes/profiles/" + m_profiles->currentText());
        restore(st);
    });

    // ---- post processor
    auto *pp = new QWidget(m_stack);
    auto *ppv = new QVBoxLayout(pp);
    ppv->setContentsMargins(0, 0, 0, 0);
    auto *run = new QPushButton(tr("Run post processor"), pp);
    ppv->addWidget(run);
    m_ppLines = whichLines(group(tr("Which lines"), pp, ppv), m_ppStyles, m_tab);
    auto *li = group(tr("Lead-in and lead-out"), pp, ppv);
    m_leadInOn = new QCheckBox(tr("Lead-in"), li);
    m_leadIn = msSpin(li, 0);
    m_leadOutOn = new QCheckBox(tr("Lead-out"), li);
    m_leadOut = msSpin(li, 0);
    grid(li)->addWidget(m_leadInOn, 0, 0);
    grid(li)->addWidget(m_leadIn, 0, 1);
    grid(li)->addWidget(m_leadOutOn, 1, 0);
    grid(li)->addWidget(m_leadOut, 1, 1);
    auto *ct = group(tr("Set times as continuous"), pp, ppv);
    m_contOn = new QCheckBox(tr("Enable"), ct);
    m_contStart = msSpin(ct, 0);
    m_contEnd = msSpin(ct, 0);
    m_contStart->setToolTip(tr("Gaps up to this move the next line's start back"));
    m_contEnd->setToolTip(tr("Gaps up to this move the previous line's end forward"));
    grid(ct)->addWidget(m_contOn, 0, 0, 1, 2);
    grid(ct)->addWidget(new QLabel(tr("Start time threshold"), ct), 1, 0);
    grid(ct)->addWidget(m_contStart, 1, 1);
    grid(ct)->addWidget(new QLabel(tr("End time threshold"), ct), 2, 0);
    grid(ct)->addWidget(m_contEnd, 2, 1);
    auto *kf = group(tr("Snap to keyframes"), pp, ppv);
    m_kfOn = new QCheckBox(tr("Enable"), kf);
    m_kfBs = msSpin(kf, 0);
    m_kfAs = msSpin(kf, 0);
    m_kfBe = msSpin(kf, 0);
    m_kfAe = msSpin(kf, 0);
    grid(kf)->addWidget(m_kfOn, 0, 0, 1, 2);
    const std::pair<QString, QSpinBox *> kfRows[] = {{tr("Before start time"), m_kfBs},
                                                     {tr("After start time"), m_kfAs},
                                                     {tr("Before end time"), m_kfBe},
                                                     {tr("After end time"), m_kfAe}};
    for (int k = 0; k < 4; ++k) {
        grid(kf)->addWidget(new QLabel(kfRows[k].first, kf), k + 1, 0);
        grid(kf)->addWidget(kfRows[k].second, k + 1, 1);
    }
    ppv->addStretch();
    m_stack->addWidget(pp);
    connect(run, &QPushButton::clicked, this, &ShiftPanel::postProcess);

    auto showPage = [this](int i) {
        m_stack->setCurrentIndex(i);
        m_mode->setText(i ? tr("Shift times") : tr("Post processor"));
    };
    connect(m_mode, &QPushButton::clicked, this, [this, showPage] {
        showPage(1 - m_stack->currentIndex());
        save();
    });

    setWidget(root);
    setWidgetResizable(true);
    setMinimumWidth(root->sizeHint().width() + 20);
    load();
    showPage(m_stack->currentIndex());
    connect(m_tab, &TabPanel::statusChanged, this, &ShiftPanel::updateEnabled);
}

void ShiftPanel::enterEvent(QEnterEvent *e) {
    updateEnabled();
    QScrollArea::enterEvent(e);
}

void ShiftPanel::updateEnabled() {
    const bool video = m_tab->video()->hasVideo();
    m_frames->setEnabled(video);
    m_markerToVideo->setEnabled(video);
    m_markerToAudio->setEnabled(m_tab->audio()->hasAudio());
    // keyframes snap through the video's frame times
    m_kfOn->setEnabled(!m_tab->keyframes().empty() && !m_tab->video()->timecodes().empty());
}

std::vector<size_t> ShiftPanel::rows(int which, const QString &styles) const {
    rust::Vec<rust::String> st;
    for (const auto &s : styles.split(',', Qt::SkipEmptyParts)) st.push_back(fs::rs(s.trimmed()));
    const auto sel = m_tab->selection();
    const auto r = m_tab->s().spec_rows(uint8_t(which), fs::slice(sel), size_t(std::max(0, m_tab->active())),
                                        {st.data(), st.size()});
    std::vector<size_t> out(r.begin(), r.end());
    // lines the filter or a folded tree hides stay as they are (Kainote), unless the grid's menu says
    if (!m_tab->grid()->ignoreFilterInActions) {
        const auto vis = m_tab->grid()->visibleRows();
        std::erase_if(out, [&](size_t x) { return !std::ranges::binary_search(vis, x); });
    }
    return out;
}

void ShiftPanel::shift() {
    auto &s = m_tab->s();
    fsb::ShiftSpec spec;
    spec.frames = m_frames->isEnabled() && m_frames->isChecked();
    qint64 amount = spec.frames ? m_time->frame() : m_time->ms();
    if (amount < 0) return;
    if (m_backward->isChecked()) amount = -amount;
    const bool byVideo = m_markerToVideo->isEnabled() && m_markerToVideo->isChecked();
    const bool byAudio = m_markerToAudio->isEnabled() && m_markerToAudio->isChecked();
    if (byVideo || byAudio) {
        const int marker = m_tab->grid()->syncMarker() >= 0 ? m_tab->grid()->syncMarker() : m_tab->active();
        const fsb::Row r = s.row(size_t(marker));
        const qint64 target = !byVideo ? m_tab->audio()->markTime()
                              : m_byEnd->isChecked() ? m_tab->videoEndTime()
                                                     : m_tab->videoStartTime();
        if (target < 0) return;
        amount = target - (m_byEnd->isChecked() ? r.end : r.start);
        spec.frames = false;
    }
    spec.amount = amount;
    spec.which = uint8_t(m_which->currentIndex());
    spec.end_fix = uint8_t(m_endFix->currentIndex());
    spec.lines = 1; // the rows, as picked here
    for (size_t r : rows(m_lines->currentIndex(), m_styles->text())) spec.selected.push_back(r);
    spec.tag_times = m_tagTimes->isChecked();
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
    // Kainote's: gaps up to both thresholds close, the end taking its share of the gap
    const int sum = m_contStart->value() + m_contEnd->value();
    spec.adjacent = m_contOn->isChecked() ? sum : 0;
    spec.bias = sum > 0 ? double(m_contEnd->value()) / sum : 0.5;
    spec.snap_keyframes = m_kfOn->isEnabled() && m_kfOn->isChecked();
    spec.kf_before_start = m_kfBs->value();
    spec.kf_after_start = m_kfAs->value();
    spec.kf_before_end = m_kfBe->value();
    spec.kf_after_end = m_kfAe->value();
    for (size_t r : rows(m_ppLines->currentIndex(), m_ppStyles->text())) spec.rows.push_back(r);
    if (spec.rows.empty()) return;
    auto kf = m_tab->keyframes();
    // as Aegisub: the video's last frame snaps like a keyframe, so ends can meet the video's end
    if (m_tab->video()->hasVideo() && !kf.empty()) kf.push_back(m_tab->video()->info().frames - 1);
    const auto tc = m_tab->video()->timecodes();
    m_tab->s().post_process(spec, {kf.data(), kf.size()}, {tc.data(), tc.size()});
    m_tab->changed();
    save();
}

void ShiftPanel::store(QSettings &st) const {
    st.setValue("time", m_time->text());
    st.setValue("backward", m_backward->isChecked());
    st.setValue("frames", m_frames->isChecked());
    st.setValue("tagTimes", m_tagTimes->isChecked());
    st.setValue("byEnd", m_byEnd->isChecked());
    st.setValue("markerToVideo", m_markerToVideo->isChecked());
    st.setValue("markerToAudio", m_markerToAudio->isChecked());
    st.setValue("lines", m_lines->currentIndex());
    st.setValue("styles", m_styles->text());
    st.setValue("which", m_which->currentIndex());
    st.setValue("endFix", m_endFix->currentIndex());
}

void ShiftPanel::restore(QSettings &st) {
    m_frames->setChecked(st.value("frames").toBool());
    m_time->setText(st.value("time", "0:00:00.00").toString());
    (st.value("backward").toBool() ? m_backward : m_forward)->setChecked(true);
    m_tagTimes->setChecked(st.value("tagTimes").toBool());
    (st.value("byEnd").toBool() ? m_byEnd : m_byStart)->setChecked(true);
    m_markerToVideo->setChecked(st.value("markerToVideo").toBool());
    m_markerToAudio->setChecked(st.value("markerToAudio").toBool());
    m_lines->setCurrentIndex(std::clamp(st.value("lines", 0).toInt(), 0, 5));
    m_styles->setText(st.value("styles").toString());
    m_which->setCurrentIndex(std::clamp(st.value("which", 0).toInt(), 0, 2));
    m_endFix->setCurrentIndex(std::clamp(st.value("endFix", 0).toInt(), 0, 2));
}

void ShiftPanel::save() const {
    QSettings st;
    st.beginGroup("shiftTimes");
    store(st);
    st.setValue("postProcessor", m_stack->currentIndex());
    st.setValue("ppLines", m_ppLines->currentIndex());
    st.setValue("ppStyles", m_ppStyles->text());
    st.setValue("leadIn", m_leadInOn->isChecked() ? m_leadIn->value() : -m_leadIn->value());
    st.setValue("leadOut", m_leadOutOn->isChecked() ? m_leadOut->value() : -m_leadOut->value());
    st.setValue("continuous", m_contOn->isChecked());
    st.setValue("contStart", m_contStart->value());
    st.setValue("contEnd", m_contEnd->value());
    st.setValue("keyframes", m_kfOn->isChecked());
    st.setValue("kf", QList<QVariant>{m_kfBs->value(), m_kfAs->value(), m_kfBe->value(), m_kfAe->value()});
}

void ShiftPanel::load() {
    QSettings st;
    st.beginGroup("shiftTimes");
    restore(st);
    m_stack->setCurrentIndex(st.value("postProcessor", 0).toInt());
    m_ppLines->setCurrentIndex(std::clamp(st.value("ppLines", 0).toInt(), 0, 5));
    m_ppStyles->setText(st.value("ppStyles").toString());
    const int li = st.value("leadIn", 0).toInt(), lo = st.value("leadOut", 0).toInt();
    m_leadInOn->setChecked(li > 0);
    m_leadIn->setValue(std::abs(li));
    m_leadOutOn->setChecked(lo > 0);
    m_leadOut->setValue(std::abs(lo));
    m_contOn->setChecked(st.value("continuous").toBool());
    m_contStart->setValue(st.value("contStart", 0).toInt());
    m_contEnd->setValue(st.value("contEnd", 0).toInt());
    m_kfOn->setChecked(st.value("keyframes").toBool());
    const auto kf = st.value("kf").toList();
    QSpinBox *spins[] = {m_kfBs, m_kfAs, m_kfBe, m_kfAe};
    for (int k = 0; k < 4 && k < kf.size(); ++k) spins[k]->setValue(kf[k].toInt());
}

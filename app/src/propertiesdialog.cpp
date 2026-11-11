#include "propertiesdialog.h"

#include "icons.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

PropertiesDialog::PropertiesDialog(TabPanel *tab) : QDialog(tab), m_tab(tab) {
    setWindowTitle(tr("ASS file properties"));
    auto &s = tab->s();
    auto *f = new QFormLayout(this);
    const std::pair<const char *, const char *> info[] = {
        {QT_TR_NOOP("Title"), "Title"}, {QT_TR_NOOP("Author"), "Original Script"},
        {QT_TR_NOOP("Translator"), "Original Translation"}, {QT_TR_NOOP("Proofreading"), "Original Editing"},
        {QT_TR_NOOP("Timer"), "Original Timing"}, {QT_TR_NOOP("Synch point"), "Synch Point"},
        {QT_TR_NOOP("Editing"), "Script Updated By"}, {QT_TR_NOOP("Update details"), "Update Details"}};
    for (auto [label, key] : info) {
        auto *e = new QLineEdit(fs::q(s.info_value(key)), this);
        f->addRow(tr(label), e);
        m_fields.emplace_back(key, e);
    }
    auto spin = [this](const QString &v) {
        auto *sp = new QSpinBox(this);
        sp->setRange(0, 16384);
        sp->setValue(v.toInt());
        return sp;
    };
    const auto fi = s.file_info();
    m_w = spin(QString::number(fi.play_res_x));
    m_h = spin(QString::number(fi.play_res_y));
    auto *res = new QHBoxLayout;
    res->addWidget(m_w);
    res->addWidget(new QLabel("x", this));
    res->addWidget(m_h);
    // Aegisub's From video buttons: the resolutions and the matrix of the video open
    const auto fromVideo = [this](QBoxLayout *row, auto set) {
        auto *b = new QPushButton(tr("From video"), this);
        b->setEnabled(m_tab->video()->hasVideo());
        connect(b, &QPushButton::clicked, this, set);
        row->addWidget(b);
        return b;
    };
    fromVideo(res, [this] {
        m_w->setValue(m_tab->video()->info().width);
        m_h->setValue(m_tab->video()->info().height);
    });
    f->addRow(tr("Resolution"), res);
    m_lw = spin(fs::q(s.info_value("LayoutResX")));
    m_lh = spin(fs::q(s.info_value("LayoutResY")));
    auto *lres = new QHBoxLayout;
    lres->addWidget(m_lw);
    lres->addWidget(new QLabel("x", this));
    lres->addWidget(m_lh);
    auto *layoutFromVideo = fromVideo(lres, [this] {
        m_lw->setValue(m_tab->video()->info().width);
        m_lh->setValue(m_tab->video()->info().height);
    });
    // Kainote's linked resolutions: a layout resolution set follows the script's
    auto *link = new QToolButton(this);
    link->setIcon(icon("link"));
    link->setCheckable(true);
    link->setChecked(QSettings().value("subs/linkResolutions", false).toBool());
    link->setToolTip(tr("Link the layout resolution to the script resolution: one that is set follows it"));
    lres->addWidget(link);
    const auto follow = [this, link, layoutFromVideo] {
        const bool on = link->isChecked();
        m_lw->setEnabled(!on);
        m_lh->setEnabled(!on);
        layoutFromVideo->setEnabled(!on && m_tab->video()->hasVideo());
        if (on && (m_lw->value() || m_lh->value())) {
            m_lw->setValue(m_w->value());
            m_lh->setValue(m_h->value());
        }
    };
    follow();
    connect(link, &QToolButton::toggled, this, [follow](bool on) {
        QSettings().setValue("subs/linkResolutions", on);
        follow();
    });
    connect(m_w, &QSpinBox::valueChanged, this, follow);
    connect(m_h, &QSpinBox::valueChanged, this, follow);
    f->addRow(tr("Layout resolution"), lres);
    m_matrix = new QComboBox(this);
    m_matrix->addItems({"None", "TV.601", "PC.601", "TV.709", "PC.709", "TV.FCC", "PC.FCC", "TV.240M", "PC.240M"});
    const QString matrix = fs::q(s.info_value("YCbCr Matrix"));
    if (m_matrix->findText(matrix) < 0) m_matrix->insertItem(0, matrix); // kept as it is, even left out: None changes the colours
    m_matrix->setCurrentText(matrix);
    auto *mrow = new QHBoxLayout;
    mrow->addWidget(m_matrix, 1);
    fromVideo(mrow, [this] {
        const QString tagged = fs::q(m_tab->video()->info().matrix); // untagged: None, as Aegisub's best practice
        const QString v = tagged.isEmpty() ? QStringLiteral("None") : tagged;
        if (m_matrix->findText(v) < 0) m_matrix->addItem(v);
        m_matrix->setCurrentText(v);
    });
    f->addRow(tr("YCbCr matrix"), mrow);
    m_wrap = new QComboBox(this);
    m_wrap->addItems({tr("0: Smart wrapping, top line is wider"), tr("1: End-of-line word wrapping, only \\N breaks"),
                      tr("2: No word wrapping, \\n and \\N break"), tr("3: Smart wrapping, bottom line is wider")});
    m_wrap->setCurrentIndex(std::clamp(fs::q(s.info_value("WrapStyle")).toInt(), 0, 3));
    f->addRow(tr("Wrap style"), m_wrap);
    m_collisions = new QComboBox(this);
    m_collisions->addItems({"Normal", "Reverse"});
    m_collisions->setCurrentText(fs::q(s.info_value("Collisions")).isEmpty() ? "Normal" : fs::q(s.info_value("Collisions")));
    f->addRow(tr("Colliding lines"), m_collisions);
    m_scaled = new QCheckBox(tr("Scale border and shadow"), this);
    m_scaled->setToolTip(tr("Scale border and shadow together with script/render resolution. If this is unchecked, "
                            "relative border and shadow size will depend on renderer."));
    m_scaled->setChecked(fs::q(s.info_value("ScaledBorderAndShadow")).compare("yes", Qt::CaseInsensitive) == 0);
    f->addRow(m_scaled);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
}

void PropertiesDialog::accept() {
    auto &s = m_tab->s();
    static int edits = 0;
    const QString amend = QString("properties-%1").arg(++edits); // one undo step
    auto set = [&](const QString &k, const QString &v) {
        if (fs::q(s.info_value(fs::Utf8(k))) != v) s.set_info_value(fs::Utf8(k), fs::Utf8(v), fs::Utf8(amend));
    };
    for (auto &[k, e] : m_fields) set(k, e->text());
    const auto fi = s.file_info();
    if (m_w->value() != fi.play_res_x) set("PlayResX", QString::number(m_w->value()));
    if (m_h->value() != fi.play_res_y) set("PlayResY", QString::number(m_h->value()));
    set("LayoutResX", m_lw->value() ? QString::number(m_lw->value()) : QString());
    set("LayoutResY", m_lh->value() ? QString::number(m_lh->value()) : QString());
    set("YCbCr Matrix", m_matrix->currentText());
    set("WrapStyle", QString::number(m_wrap->currentIndex()));
    set("Collisions", m_collisions->currentText());
    set("ScaledBorderAndShadow", m_scaled->isChecked() ? "yes" : "no");
    m_tab->changed();
    QDialog::accept();
}

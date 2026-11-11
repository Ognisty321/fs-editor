#include "resampledialog.h"

#include "tabpanel.h"
#include "videobox.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

// Aegisub's Resample Resolution: both resolutions and colour matrices, what a changed aspect ratio
// does, and the source margins of the manual mode.
ResampleDialog::ResampleDialog(TabPanel *tab) : QDialog(tab) {
    setWindowTitle(tr("Resample subtitles"));
    auto *v = new QVBoxLayout(this);
    const auto fi = tab->s().file_info();
    // blank: colours stay; "None" has no fixed meaning, so it is not offered (Aegisub)
    const QStringList matrices{"", "TV.601", "PC.601", "TV.709", "PC.709", "TV.FCC", "PC.FCC", "TV.240M", "PC.240M"};
    QString scriptMatrix = fs::q(tab->s().info_value("YCbCr Matrix"));
    if (scriptMatrix != "None" && matrices.indexOf(scriptMatrix) < 1) scriptMatrix = "TV.601"; // how it renders
    const QString videoMatrix = tab->video()->hasVideo() ? fs::q(tab->video()->info().matrix) : QString();
    const auto resolution = [&](const QString &title, QSpinBox *&x, QSpinBox *&y, QComboBox *&matrix,
                                QPushButton *from) {
        auto *box = new QGroupBox(title, this);
        auto *g = new QGridLayout(box);
        x = new QSpinBox(box);
        y = new QSpinBox(box);
        for (auto *s : {x, y}) s->setRange(1, 99999);
        matrix = new QComboBox(box);
        matrix->addItems(matrices);
        g->addWidget(x, 0, 0);
        g->addWidget(new QLabel("×", box), 0, 1);
        g->addWidget(y, 0, 2);
        g->addWidget(from, 0, 3);
        g->addWidget(new QLabel(tr("YCbCr matrix:"), box), 1, 0);
        g->addWidget(matrix, 1, 1, 1, 3);
        v->addWidget(box);
    };
    QSpinBox *sx, *sy, *dx, *dy;
    QComboBox *sm, *dm;
    auto *fromScript = new QPushButton(tr("From script"), this);
    auto *fromVideo = new QPushButton(tr("From video"), this);
    resolution(tr("Source resolution"), sx, sy, sm, fromScript);
    resolution(tr("Destination resolution"), dx, dy, dm, fromVideo);
    // Aegisub proposes converting the colours when the script was made for another matrix than the video's
    if (matrices.indexOf(scriptMatrix) > 0 && matrices.indexOf(videoMatrix) > 0 && scriptMatrix != videoMatrix) {
        sm->setCurrentText(scriptMatrix);
        dm->setCurrentText(videoMatrix);
    }
    const bool video = tab->video()->hasVideo();
    const int vw = video ? tab->video()->info().width : fi.play_res_x;
    const int vh = video ? tab->video()->info().height : fi.play_res_y;
    sx->setValue(fi.play_res_x);
    sy->setValue(fi.play_res_y);
    dx->setValue(vw);
    dy->setValue(vh);

    auto *arBox = new QGroupBox(tr("Aspect ratio handling"), this);
    auto *arLayout = new QVBoxLayout(arBox);
    auto *ar = new QButtonGroup(this);
    for (const char *label : {QT_TR_NOOP("Stretch"), QT_TR_NOOP("Add borders"), QT_TR_NOOP("Remove borders"),
                              QT_TR_NOOP("Manual")}) {
        auto *b = new QRadioButton(tr(label), arBox);
        ar->addButton(b, int(ar->buttons().size()));
        arLayout->addWidget(b);
    }
    ar->button(std::clamp(QSettings().value("resample/arMode", 0).toInt(), 0, 3))->setChecked(true);
    v->addWidget(arBox);

    auto *marginBox = new QGroupBox(tr("Margin offset"), this);
    auto *mg = new QGridLayout(marginBox);
    std::array<QSpinBox *, 4> m; // left, right, top, bottom
    for (auto *&s : m) {
        s = new QSpinBox(marginBox);
        s->setRange(-9999, 9999);
    }
    auto *symmetrical = new QCheckBox(tr("Symmetrical"), marginBox);
    symmetrical->setChecked(true);
    mg->addWidget(m[2], 0, 1);
    mg->addWidget(m[0], 1, 0);
    mg->addWidget(symmetrical, 1, 1);
    mg->addWidget(m[1], 1, 2);
    mg->addWidget(m[3], 2, 1);
    v->addWidget(marginBox);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    v->addWidget(bb);

    const auto update = [=] {
        fromVideo->setEnabled(video && (dx->value() != vw || dy->value() != vh));
        fromScript->setEnabled(sx->value() != fi.play_res_x || sy->value() != fi.play_res_y);
        const double from = double(sx->value()) / sy->value(), to = double(dx->value()) / dy->value();
        const bool changed = std::abs(from - to) / to > .01;
        arBox->setEnabled(changed);
        marginBox->setEnabled(changed && ar->checkedId() == 3);
        m[1]->setEnabled(!symmetrical->isChecked());
        m[3]->setEnabled(!symmetrical->isChecked());
    };
    for (auto *s : {sx, sy, dx, dy}) connect(s, &QSpinBox::valueChanged, this, update);
    connect(ar, &QButtonGroup::idToggled, this, update);
    connect(symmetrical, &QCheckBox::toggled, this, [=](bool on) {
        if (on) {
            m[1]->setValue(m[0]->value());
            m[3]->setValue(m[2]->value());
        }
        update();
    });
    for (int k : {0, 2})
        connect(m[k], &QSpinBox::valueChanged, this, [=](int value) {
            if (symmetrical->isChecked()) m[k + 1]->setValue(value);
        });
    connect(fromScript, &QPushButton::clicked, this, [=] {
        sx->setValue(fi.play_res_x);
        sy->setValue(fi.play_res_y);
        sm->setCurrentIndex(std::max(0, int(matrices.indexOf(scriptMatrix))));
    });
    connect(fromVideo, &QPushButton::clicked, this, [=] {
        dx->setValue(vw);
        dy->setValue(vh);
        dm->setCurrentIndex(std::max(0, int(matrices.indexOf(videoMatrix))));
    });
    update();
    connect(bb, &QDialogButtonBox::accepted, this, [=, this] {
        fsb::ResampleSpec r;
        r.from_x = sx->value();
        r.from_y = sy->value();
        r.to_x = dx->value();
        r.to_y = dy->value();
        r.margin_l = m[0]->value();
        r.margin_r = m[1]->value();
        r.margin_t = m[2]->value();
        r.margin_b = m[3]->value();
        r.ar_mode = uint8_t(ar->checkedId());
        r.from_matrix = fs::rs(sm->currentText());
        r.to_matrix = fs::rs(dm->currentText());
        QSettings().setValue("resample/arMode", ar->checkedId());
        tab->s().resample(r);
        tab->changed();
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

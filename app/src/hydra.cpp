#include "hydra.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace hydra {

const QList<const char *> &modes() {
    static const QList<const char *> m{
        QT_TRANSLATE_NOOP("VisualTools", "Add"),
        QT_TRANSLATE_NOOP("VisualTools", "Set"),
        QT_TRANSLATE_NOOP("VisualTools", "Multiply by line number"),
        QT_TRANSLATE_NOOP("VisualTools", "Gradient text increasing"),
        QT_TRANSLATE_NOOP("VisualTools", "Gradient text decreasing"),
        QT_TRANSLATE_NOOP("VisualTools", "Gradient lines increasing"),
        QT_TRANSLATE_NOOP("VisualTools", "Gradient lines decreasing"),
        QT_TRANSLATE_NOOP("VisualTools", "Insert"),
        QT_TRANSLATE_NOOP("VisualTools", "Step from the active line"),
    };
    return m;
}

std::vector<Tag> defaults() {
    // Kainote's list (its change modes in ours: insert 7, multiply 8), then the scale and
    // rotation tags this tool had before
    enum { Add = 0, Insert = 7, Step = 8 };
    struct Row {
        const char *name, *tag;
        double min, max, step;
        int digits;
        QList<double> values;
        bool atStart;
        int mode;
    };
    static const Row rows[] = {
        {QT_TRANSLATE_NOOP("hydra", "blur"), "blur", 0, 100, 0.5, 1, {0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "border"), "bord", 0, 50, 1, 1, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "blur edge"), "be", 0, 100, 1, 1, {0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "alpha"), "alpha", 0, 255, 1, 0, {0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "primary alpha"), "1a", 0, 255, 1, 0, {0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "border alpha"), "3a", 0, 255, 1, 0, {0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "shadow alpha"), "4a", 0, 255, 1, 0, {0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "primary color"), "1c", 0, 255, 1, 0, {0, 0, 0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "border color"), "3c", 0, 255, 1, 0, {0, 0, 0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "shadow color"), "4c", 0, 255, 1, 0, {0, 0, 0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "fading"), "fad", 0, 2000, 5, 0, {0, 0}, true, Insert},
        {QT_TRANSLATE_NOOP("hydra", "shear x"), "fax", -10, 10, 0.01, 2, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "shear y"), "fay", -10, 10, 0.01, 2, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "font size"), "fs", 20, 300, 1, 1, {70}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "spacing"), "fsp", -100, 100, 1, 1, {0}, false, Insert},
        {QT_TRANSLATE_NOOP("hydra", "shadow"), "shad", 0, 80, 1, 1, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "border x"), "xbord", 0, 80, 1, 1, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "border y"), "ybord", 0, 80, 1, 1, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "shadow x"), "xshad", -80, 80, 1, 1, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "shadow y"), "yshad", -80, 80, 1, 1, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "position"), "pos", 0, 1920, 0.1, 2, {0, 0}, true, Step},
        {QT_TRANSLATE_NOOP("hydra", "transform times"), "t", 0, 9999, 10, 0, {0, 0}, true, Insert},
        {QT_TRANSLATE_NOOP("hydra", "scale x"), "fscx", 0, 500, 1, 1, {100}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "scale y"), "fscy", 0, 500, 1, 1, {100}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "rotation z"), "frz", -360, 360, 0.5, 1, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "rotation x"), "frx", -360, 360, 0.5, 1, {0}, false, Add},
        {QT_TRANSLATE_NOOP("hydra", "rotation y"), "fry", -360, 360, 0.5, 1, {0}, false, Add},
    };
    std::vector<Tag> out;
    for (const Row &r : rows)
        out.push_back({QCoreApplication::translate("hydra", r.name), r.tag, r.min, r.max, r.step, r.digits, r.values,
                       r.atStart, r.mode});
    return out;
}

std::vector<Tag> load() {
    QSettings st;
    const int n = st.beginReadArray("hydra/tags");
    std::vector<Tag> out;
    for (int i = 0; i < n; ++i) {
        st.setArrayIndex(i);
        Tag t;
        t.name = st.value("name").toString();
        t.tag = st.value("tag").toString();
        t.min = st.value("min", 0).toDouble();
        t.max = st.value("max", 100).toDouble();
        t.step = st.value("step", 1).toDouble();
        t.digits = std::clamp(st.value("digits", 1).toInt(), 0, 2);
        t.values.clear();
        for (const QVariant &v : st.value("values").toList()) t.values << v.toDouble();
        if (t.values.isEmpty()) t.values << 0;
        while (t.values.size() > 4) t.values.removeLast();
        t.atStart = st.value("atStart").toBool();
        t.mode = std::clamp(st.value("mode").toInt(), 0, int(modes().size()) - 1);
        // a hand-edited entry that cannot work is left out
        if (!t.name.isEmpty() && !t.tag.isEmpty() && t.min < t.max && t.step > 0) out.push_back(t);
    }
    st.endArray();
    return out.empty() ? defaults() : out;
}

void save(const std::vector<Tag> &tags) {
    QSettings st;
    st.remove("hydra/tags");
    if (tags == defaults()) return; // the defaults follow the language and later versions
    st.beginWriteArray("hydra/tags", int(tags.size()));
    for (int i = 0; i < int(tags.size()); ++i) {
        const Tag &t = tags[size_t(i)];
        st.setArrayIndex(i);
        st.setValue("name", t.name);
        st.setValue("tag", t.tag);
        st.setValue("min", t.min);
        st.setValue("max", t.max);
        st.setValue("step", t.step);
        st.setValue("digits", t.digits);
        QVariantList v;
        for (double x : t.values) v << x;
        st.setValue("values", v);
        st.setValue("atStart", t.atStart);
        st.setValue("mode", t.mode);
    }
    st.endArray();
}

int fixedCount(const QString &tag) {
    static const QStringList alpha{"alpha", "1a", "2a", "3a", "4a"}, color{"c", "1c", "2c", "3c", "4c"};
    return alpha.contains(tag) ? 1 : color.contains(tag) ? 3 : 0;
}

int count(const Tag &t) {
    const int fixed = fixedCount(t.tag);
    return fixed ? fixed : int(std::clamp<qsizetype>(t.values.size(), 1, 4));
}

} // namespace hydra

// ---- the editor ---------------------------------------------------------------

HydraDialog::HydraDialog(QWidget *parent, std::vector<hydra::Tag> tags, int current)
    : QDialog(parent), m_tags(std::move(tags)) {
    setWindowTitle(tr("All tags tool: tags"));
    auto *lay = new QHBoxLayout(this);
    auto *left = new QVBoxLayout;
    m_list = new QListWidget(this);
    left->addWidget(m_list);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add"), this), *remove = new QPushButton(tr("Remove"), this);
    auto *reset = new QPushButton(tr("Restore defaults"), this);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    left->addLayout(buttons);
    left->addWidget(reset);
    lay->addLayout(left);

    auto *right = new QVBoxLayout;
    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_tag = new QLineEdit(this);
    m_tag->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9A-Za-z]+"), m_tag));
    m_tag->setToolTip(tr("The tag without its backslash"));
    const auto number = [this](double lo, double hi) {
        auto *s = new QDoubleSpinBox(this);
        s->setRange(lo, hi);
        s->setDecimals(2);
        return s;
    };
    m_min = number(-100000, 100000);
    m_max = number(-100000, 100000);
    m_step = number(0.01, 10000);
    m_digits = new QSpinBox(this);
    m_digits->setRange(0, 2);
    m_count = new QSpinBox(this);
    m_count->setRange(1, 4);
    m_count->setToolTip(tr("Sliders: alpha has one, a colour three (red, green, blue), others as many of their "
                           "leading numbers as are changed (\\pos and \\fad two)"));
    auto *values = new QHBoxLayout;
    for (auto *&v : m_values) {
        v = number(-100000, 100000);
        values->addWidget(v);
    }
    m_at = new QComboBox(this);
    m_at->addItems({tr("At the cursor"), tr("At the line's start")});
    m_mode = new QComboBox(this);
    for (const char *m : hydra::modes()) m_mode->addItem(QCoreApplication::translate("VisualTools", m));
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Tag:"), m_tag);
    form->addRow(tr("Minimum:"), m_min);
    form->addRow(tr("Maximum:"), m_max);
    form->addRow(tr("Step:"), m_step);
    form->addRow(tr("Digits after the point:"), m_digits);
    form->addRow(tr("Values:"), m_count);
    form->addRow(tr("Default values:"), values);
    form->addRow(tr("Insert puts it:"), m_at);
    form->addRow(tr("Change mode:"), m_mode);
    right->addLayout(form);
    right->addStretch();
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    right->addWidget(box);
    lay->addLayout(right, 1);
    connect(box, &QDialogButtonBox::accepted, this, &HydraDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (QWidget *w : std::initializer_list<QWidget *>{m_name, m_tag})
        connect(static_cast<QLineEdit *>(w), &QLineEdit::textEdited, this, [this] { store(); });
    for (QDoubleSpinBox *s : {m_min, m_max, m_step, m_values[0], m_values[1], m_values[2], m_values[3]})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { store(); });
    for (QSpinBox *s : {m_digits, m_count}) connect(s, &QSpinBox::valueChanged, this, [this] { store(); });
    for (QComboBox *c : {m_at, m_mode}) connect(c, &QComboBox::currentIndexChanged, this, [this] { store(); });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int i) { showTag(i); });
    connect(add, &QPushButton::clicked, this, [this] {
        hydra::Tag t;
        t.name = t.tag = "blur";
        m_tags.push_back(t);
        fill();
        m_list->setCurrentRow(int(m_tags.size()) - 1);
        m_name->setFocus();
        m_name->selectAll();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_shown < 0 || m_tags.size() < 2) return; // the tool needs one
        m_tags.erase(m_tags.begin() + m_shown);
        const int at = std::min(m_shown, int(m_tags.size()) - 1);
        m_shown = -1;
        fill();
        m_list->setCurrentRow(at);
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        m_tags = hydra::defaults();
        m_shown = -1;
        fill();
        m_list->setCurrentRow(0);
    });
    fill();
    m_list->setCurrentRow(std::clamp(current, 0, int(m_tags.size()) - 1));
}

int HydraDialog::current() const { return m_list->currentRow(); }

void HydraDialog::fill() {
    const QSignalBlocker quiet(m_list);
    m_list->clear();
    for (const auto &t : m_tags) m_list->addItem(QString("%1 (\\%2)").arg(t.name, t.tag));
}

void HydraDialog::showTag(int i) {
    if (i < 0 || i >= int(m_tags.size())) return;
    m_shown = i;
    const hydra::Tag &t = m_tags[size_t(i)];
    m_loading = true;
    m_name->setText(t.name);
    m_tag->setText(t.tag);
    m_min->setValue(t.min);
    m_max->setValue(t.max);
    m_step->setValue(t.step);
    m_digits->setValue(t.digits);
    m_count->setValue(int(t.values.size()));
    for (int k = 0; k < 4; ++k) m_values[k]->setValue(t.values.value(k));
    m_at->setCurrentIndex(t.atStart ? 1 : 0);
    m_mode->setCurrentIndex(t.mode);
    m_loading = false;
    store(); // the count of a colour or an alpha follows the tag
}

void HydraDialog::store() {
    if (m_loading || m_shown < 0) return;
    hydra::Tag &t = m_tags[size_t(m_shown)];
    t.name = m_name->text().trimmed();
    t.tag = m_tag->text().trimmed();
    t.min = m_min->value();
    t.max = m_max->value();
    t.step = m_step->value();
    t.digits = m_digits->value();
    t.atStart = m_at->currentIndex() == 1;
    t.mode = m_mode->currentIndex();
    t.values = {m_values[0]->value()};
    for (int k = 1; k < m_count->value(); ++k) t.values << m_values[k]->value();
    const int fixed = hydra::fixedCount(t.tag);
    const bool own = !fixed; // not an alpha or a colour: the count is the user's
    if (!own) {
        while (t.values.size() < fixed) t.values << 0;
        while (t.values.size() > fixed) t.values.removeLast();
        const QSignalBlocker quiet(m_count);
        m_count->setValue(fixed);
    }
    m_count->setEnabled(own);
    for (int k = 0; k < 4; ++k) m_values[k]->setEnabled(k < t.values.size());
    if (QListWidgetItem *it = m_list->item(m_shown)) it->setText(QString("%1 (\\%2)").arg(t.name, t.tag));
}

void HydraDialog::accept() {
    QStringList names;
    for (int i = 0; i < int(m_tags.size()); ++i) {
        const hydra::Tag &t = m_tags[size_t(i)];
        QString why;
        if (t.name.isEmpty()) why = tr("A tag needs a name.");
        else if (names.contains(t.name)) why = tr("Two tags are named \"%1\".").arg(t.name);
        else if (t.tag.isEmpty()) why = tr("\"%1\" needs a tag.").arg(t.name);
        else if (t.min >= t.max) why = tr("The minimum of \"%1\" is not below its maximum.").arg(t.name);
        names << t.name;
        if (!why.isEmpty()) {
            m_list->setCurrentRow(i);
            QMessageBox::warning(this, windowTitle(), why);
            return;
        }
    }
    QDialog::accept();
}
